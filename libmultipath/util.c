#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <libudev.h>
#include <mpath_persist.h>

#include "util.h"
#include "debug.h"
#include "checkers.h"
#include "vector.h"
#include "structs.h"
#include "config.h"
#include "log.h"

size_t
strchop(char *str)
{
	size_t i;

	for (i = strlen(str) - 1; i != (size_t) -1 && isspace(str[i]); i--) ;
	str[++i] = '\0';
	return i;
}

#ifndef __GLIBC__
/*
 * glibc's non-destructive version of basename()
 * License: LGPL-2.1-or-later
 */
static const char *__basename(const char *filename)
{
	char *p = strrchr(filename, '/');
	return p ? p + 1 : filename;
}
#define basename(x) __basename(x)
#endif

int
basenamecpy (const char *src, char *dst, size_t size)
{
	const char *p, *e;

	if (!src || !dst || !strlen(src))
		return 0;

	p = basename(src);

	for (e = p + strlen(p) - 1; e >= p && isspace(*e); --e) ;
	if (e < p || (size_t)(e - p) > size - 2)
		return 0;

	strlcpy(dst, p, e - p + 2);
	return strlen(dst);
}

int
filepresent (const char *run) {
	struct stat buf;

	if(!stat(run, &buf))
		return 1;
	return 0;
}

char *get_next_string(char **temp, const char *split_char)
{
	char *token = NULL;
	token = strsep(temp, split_char);
	while (token != NULL && !strcmp(token, ""))
		token = strsep(temp, split_char);
	return token;
}

int
get_word (const char *sentence, char **word)
{
	const char *p;
	int len;
	int skip = 0;

	if (word)
		*word = NULL;

	while (*sentence ==  ' ') {
		sentence++;
		skip++;
	}
	if (*sentence == '\0')
		return 0;

	p = sentence;

	while (*p !=  ' ' && *p != '\0')
		p++;

	len = (int) (p - sentence);

	if (!word)
		return skip + len;

	*word = calloc(1, len + 1);

	if (!*word) {
		condlog(0, "get_word : oom");
		return 0;
	}
	strncpy(*word, sentence, len);
	strchop(*word);
	condlog(5, "*word = %s, len = %i", *word, len);

	if (*p == '\0')
		return 0;

	return skip + len;
}

size_t strlcpy(char * restrict dst, const char * restrict src, size_t size)
{
	size_t bytes = 0;
	char ch;

	while ((ch = *src++)) {
		if (bytes + 1 < size)
			*dst++ = ch;
		bytes++;
	}

	/* If size == 0 there is no space for a final null... */
	if (size)
		*dst = '\0';
	return bytes;
}

size_t strlcat(char * restrict dst, const char * restrict src, size_t size)
{
	size_t bytes = 0;
	char ch;

	while (bytes < size && *dst) {
		dst++;
		bytes++;
	}
	if (bytes == size)
		return (bytes + strlen(src));

	while ((ch = *src++)) {
		if (bytes + 1 < size)
			*dst++ = ch;
		bytes++;
	}

	*dst = '\0';
	return bytes;
}

int devt2devname(char *devname, int devname_len, const char *devt)
{
	struct udev_device *u_dev;
	const char * dev_name;
	int r;

	if (!devname || !devname_len || !devt)
		return 1;

	u_dev = udev_device_new_from_devnum(udev, 'b', parse_devt(devt));
	if (!u_dev) {
		condlog(0, "\"%s\": invalid major/minor numbers, not found in sysfs", devt);
		return 1;
	}

	dev_name = udev_device_get_sysname(u_dev);
	if (!dev_name) {
		udev_device_unref(u_dev);
		return 1;
	}
	r = strlcpy(devname, dev_name, devname_len);
	udev_device_unref(u_dev);

	return !(r < devname_len);
}

/* This function returns a pointer inside of the supplied pathname string.
 * If is_path_device is true, it may also modify the supplied string */
char *convert_dev(char *name, int is_path_device)
{
	char *ptr;

	if (!name)
		return NULL;
	if (is_path_device) {
		ptr = strstr(name, "cciss/");
		if (ptr) {
			ptr += 5;
			*ptr = '!';
		}
	}
	if (!strncmp(name, "/dev/", 5) && strlen(name) > 5)
		ptr = name + 5;
	else
		ptr = name;
	return ptr;
}

dev_t parse_devt(const char *dev_t)
{
	int maj, min;

	if (sscanf(dev_t,"%d:%d", &maj, &min) != 2)
		return 0;

	return makedev(maj, min);
}

void
setup_thread_attr(pthread_attr_t *attr, size_t stacksize, int detached)
{
	int ret;

	ret = pthread_attr_init(attr);
	assert(ret == 0);
	if (PTHREAD_STACK_MIN > 0 && stacksize < (size_t)PTHREAD_STACK_MIN)
		stacksize = (size_t)PTHREAD_STACK_MIN;
	ret = pthread_attr_setstacksize(attr, stacksize);
	assert(ret == 0);
	if (detached) {
		ret = pthread_attr_setdetachstate(attr,
						  PTHREAD_CREATE_DETACHED);
		assert(ret == 0);
	}
}

int systemd_service_enabled_in(const char *dev, const char *prefix)
{
	static const char service[] = "multipathd.service";
	char path[PATH_MAX], file[PATH_MAX];
	DIR *dirfd;
	struct dirent *d;
	int found = 0;

	if (safe_sprintf(path, "%s/systemd/system", prefix))
		return 0;

	condlog(3, "%s: checking for %s in %s", dev, service, path);

	dirfd = opendir(path);
	if (dirfd == NULL)
		return 0;

	while ((d = readdir(dirfd)) != NULL) {
		char *p;
		struct stat stbuf;

		if ((strcmp(d->d_name,".") == 0) ||
		    (strcmp(d->d_name,"..") == 0))
			continue;

		if (strlen(d->d_name) < 6)
			continue;

		p = d->d_name + strlen(d->d_name) - 6;
		if (strcmp(p, ".wants"))
			continue;
		if (!safe_sprintf(file, "%s/%s/%s",
				  path, d->d_name, service)
		    && stat(file, &stbuf) == 0) {
			condlog(3, "%s: found %s", dev, file);
			found++;
			break;
		}
	}
	closedir(dirfd);

	return found;
}

int systemd_service_enabled(const char *dev)
{
	int found = 0;

	found = systemd_service_enabled_in(dev, "/etc");
	if (!found)
		found = systemd_service_enabled_in(dev, "/usr/lib");
	if (!found)
		found = systemd_service_enabled_in(dev, "/lib");
	if (!found)
		found = systemd_service_enabled_in(dev, "/run");
	return found;
}

static int _linux_version_code;
static pthread_once_t _lvc_initialized = PTHREAD_ONCE_INIT;

/* Returns current kernel version encoded as major*65536 + minor*256 + patch,
 * so, for example,  to check if the kernel is greater than 2.2.11:
 *
 *     if (get_linux_version_code() > KERNEL_VERSION(2,2,11)) { <stuff> }
 *
 * Copyright (C) 1999-2004 by Erik Andersen <andersen@codepoet.org>
 * Code copied from busybox (GPLv2 or later)
 */
static void
_set_linux_version_code(void)
{
	struct utsname name;
	char *t;
	int i, r;

	uname(&name); /* never fails */
	t = name.release;
	r = 0;
	for (i = 0; i < 3; i++) {
		t = strtok(t, ".");
		r = r * 256 + (t ? atoi(t) : 0);
		t = NULL;
	}
	_linux_version_code = r;
}

int get_linux_version_code(void)
{
	pthread_once(&_lvc_initialized, _set_linux_version_code);
	return _linux_version_code;
}

int parse_prkey(const char *ptr, uint64_t *prkey)
{
	if (!ptr)
		return 1;
	if (*ptr == '0')
		ptr++;
	if (*ptr == 'x' || *ptr == 'X')
		ptr++;
	if (*ptr == '\0' || strlen(ptr) > 16)
		return 1;
	if (strlen(ptr) != strspn(ptr, "0123456789aAbBcCdDeEfF"))
		return 1;
	if (sscanf(ptr, "%" SCNx64 "", prkey) != 1)
		return 1;
	return 0;
}

int parse_prkey_flags(const char *ptr, uint64_t *prkey, uint8_t *flags)
{
	char *flagstr;

	flagstr = strchr(ptr, ':');
	*flags = 0;
	if (flagstr) {
		*flagstr++ = '\0';
		if (strlen(flagstr) == 5 && strcmp(flagstr, "aptpl") == 0)
			*flags = MPATH_F_APTPL_MASK;
	}
	return parse_prkey(ptr, prkey);
}

int safe_write(int fd, const void *buf, size_t count)
{
	while (count > 0) {
		ssize_t r = write(fd, buf, count);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		count -= r;
		buf = (const char *)buf + r;
	}
	return 0;
}

void set_max_fds(rlim_t max_fds)
{
	struct rlimit fd_limit;

	if (!max_fds)
		return;

	if (getrlimit(RLIMIT_NOFILE, &fd_limit) < 0) {
		condlog(0, "can't get open fds limit: %s",
			strerror(errno));
		fd_limit.rlim_cur = 0;
		fd_limit.rlim_max = 0;
	}
	if (fd_limit.rlim_cur < max_fds) {
		fd_limit.rlim_cur = max_fds;
		if (fd_limit.rlim_max < max_fds)
			fd_limit.rlim_max = max_fds;
		if (setrlimit(RLIMIT_NOFILE, &fd_limit) < 0) {
			condlog(0, "can't set open fds limit to "
				"%lu/%lu : %s",
				(unsigned long)fd_limit.rlim_cur,
				(unsigned long)fd_limit.rlim_max,
				strerror(errno));
		} else {
			condlog(3, "set open fds limit to %lu/%lu",
				(unsigned long)fd_limit.rlim_cur,
				(unsigned long)fd_limit.rlim_max);
		}
	}
}

void free_scandir_result(struct scandir_result *res)
{
	int i;

	for (i = 0; i < res->n; i++)
		free(res->di[i]);
	free(res->di);
}

void close_fd(void *arg)
{
	close((long)arg);
}

void cleanup_free_ptr(void *arg)
{
	void **p = arg;

	if (p && *p)
		free(*p);
}

void cleanup_mutex(void *arg)
{
	pthread_mutex_unlock(arg);
}

struct bitfield *alloc_bitfield(unsigned int maxbit)
{
	unsigned int n;
	struct bitfield *bf;

	if (maxbit == 0) {
		errno = EINVAL;
		return NULL;
	}

	n = (maxbit - 1) / bits_per_slot + 1;
	bf = calloc(1, sizeof(struct bitfield) + n * sizeof(bitfield_t));
	if (bf)
		bf->len = maxbit;
	return bf;
}

void _log_bitfield_overflow(const char *f, unsigned int bit, unsigned int len)
{
	condlog(0, "%s: bitfield overflow: %u >= %u", f, bit, len);
}

int should_exit(void)
{
	return 0;
}

void cleanup_charp(char **p)
{
	free(*p);
}

void cleanup_ucharp(unsigned char **p)
{
	free(*p);
}
