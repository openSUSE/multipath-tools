#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <urcu.h>
#include <urcu/uatomic.h>
#include <assert.h>

#include "debug.h"
#include "checkers.h"
#include "async_checker.h"
#include "vector.h"
#include "util.h"

static const char * const checker_dir = MULTIPATH_DIR;

static const char *checker_state_names[PATH_MAX_STATE] = {
	[PATH_WILD] = "wild",
	[PATH_UNCHECKED] = "unchecked",
	[PATH_DOWN] = "down",
	[PATH_UP] = "up",
	[PATH_SHAKY] = "shaky",
	[PATH_GHOST] = "ghost",
	[PATH_PENDING] = "pending",
	[PATH_TIMEOUT] = "timeout",
	[PATH_REMOVED] = "removed",
	[PATH_DELAYED] = "delayed",
	[PATH_DISCONNECTED] = "disconnected",
};

static LIST_HEAD(checkers);

const char *checker_state_name(int i)
{
	if (i < 0 || i >= PATH_MAX_STATE) {
		condlog (2, "invalid state index = %d", i);
		return INVALID;
	}
	return checker_state_names[i];
}

static void free_checker_class(void *ptr)
{
	struct checker_class *c = ptr;
	if (!c)
		return;
	condlog(3, "unloading %s checker", c->name);
	list_del(&c->node);
	if (c->reset)
		c->reset();
	if (c->handle) {
		if (dlclose(c->handle) != 0) {
			condlog(0, "Cannot unload checker %s: %s",
				c->name, dlerror());
		}
	}
}

static struct checker_class *alloc_checker_class(void)
{
	struct checker_class *c;

	c = alloc_shared_ptr(sizeof(*c), free_checker_class);
	if (c) {
		memset(c, 0, sizeof(*c));
		INIT_LIST_HEAD(&c->node);
	}
	return c;
}

void cleanup_checkers (void)
{
	struct checker_class *checker_loop;
	struct checker_class *checker_temp;

	list_for_each_entry_safe(checker_loop, checker_temp, &checkers, node)
		put_shared_ptr(checker_loop);
}

static struct checker_class *checker_class_lookup(const char *name)
{
	struct checker_class *c;

	if (!name || !strlen(name))
		return NULL;
	list_for_each_entry(c, &checkers, node) {
		if (!strncmp(name, c->name, CHECKER_NAME_LEN))
			return c;
	}
	return NULL;
}

void reset_checker_classes(void)
{
	struct checker_class *c;

	list_for_each_entry(c, &checkers, node) {
		if (c->reset)
			c->reset();
	}
}

static struct checker_class *add_async_checker_class(struct checker_class *c)
{
	c->init = async_check_init;
	c->check = async_check_check;
	c->need_wait = async_check_need_wait;
	c->pending = async_check_pending;
	c->free = async_check_free;

	list_add(&c->node, &checkers);
	return c;
}

static struct checker_class *add_checker_class(const char *name)
{
	char libname[LIB_CHECKER_NAMELEN];
	struct stat stbuf;
	struct checker_class *c;
	char *errstr;

	c = alloc_checker_class();
	if (!c)
		return NULL;
	snprintf(c->name, CHECKER_NAME_LEN, "%s", name);
	if (!strncmp(c->name, NONE, 4))
		goto done;
	snprintf(libname, LIB_CHECKER_NAMELEN, "%s/libcheck%s.so",
		 checker_dir, name);
	if (stat(libname,&stbuf) < 0) {
		condlog(0,"Checker '%s' not found in %s",
			name, checker_dir);
		goto out;
	}
	condlog(3, "loading %s checker", libname);
	c->handle = dlopen(libname, RTLD_NOW);
	if (!c->handle) {
		if ((errstr = dlerror()) != NULL)
			condlog(0, "A dynamic linking error occurred: (%s)",
				errstr);
		goto out;
	}

	c->msgtable_size = 0;
	c->msgtable = dlsym(c->handle, "libcheck_msgtable");

	if (c->msgtable != NULL) {
		const char **p;

		for (p = c->msgtable;
		     *p && (p - c->msgtable) < CHECKER_MSGTABLE_SIZE; p++)
			/* nothing */;

		c->msgtable_size = p - c->msgtable;
	} else
		c->msgtable_size = 0;
	condlog(3, "checker %s: message table size = %d", c->name,
		c->msgtable_size);

	c->async_func = (int (*)(struct runner_data *))
		dlsym(c->handle, "libcheck_async_func");
	errstr = dlerror();
	if (c->async_func)
		return add_async_checker_class(c);

	c->check = (int (*)(struct checker *)) dlsym(c->handle, "libcheck_check");
	errstr = dlerror();
	if (errstr != NULL)
		condlog(0, "A dynamic linking error occurred: (%s)", errstr);
	if (!c->check)
		goto out;

	c->init = (int (*)(struct checker *)) dlsym(c->handle, "libcheck_init");
	errstr = dlerror();
	if (errstr != NULL)
		condlog(0, "A dynamic linking error occurred: (%s)", errstr);
	if (!c->init)
		goto out;

	c->mp_init = (int (*)(struct checker *)) dlsym(c->handle, "libcheck_mp_init");
	c->reset = (void (*)(void))dlsym(c->handle, "libcheck_reset");
	c->pending = (int (*)(struct checker *)) dlsym(c->handle, "libcheck_pending");
	c->need_wait = (bool (*)(struct checker *)) dlsym(c->handle, "libcheck_need_wait");
	/* These 5 functions can be NULL. call dlerror() to clear out any
	 * error string */
	dlerror();

	c->free = (void (*)(struct checker *)) dlsym(c->handle, "libcheck_free");
	errstr = dlerror();
	if (errstr != NULL)
		condlog(0, "A dynamic linking error occurred: (%s)", errstr);
	if (!c->free)
		goto out;

done:
	c->sync = 1;
	list_add(&c->node, &checkers);
	return c;
out:
	put_shared_ptr(c);
	return NULL;
}

void checker_set_fd (struct checker * c, int fd)
{
	if (!c)
		return;
	c->fd = fd;
}

void checker_set_sync (struct checker * c)
{
	if (!c || !c->cls)
		return;
	c->cls->sync = 1;
}

void checker_set_async (struct checker * c)
{
	if (!c || !c->cls)
		return;
	c->cls->sync = 0;
}

void checker_enable (struct checker * c)
{
	if (!c)
		return;
	c->disable = 0;
}

void checker_disable (struct checker * c)
{
	if (!c)
		return;
	c->disable = 1;
	c->msgid = CHECKER_MSGID_DISABLED;
	c->path_state = PATH_UNCHECKED;
}

int checker_init (struct checker * c, void ** mpctxt_addr)
{
	if (!c || !c->cls)
		return 1;
	c->mpcontext = mpctxt_addr;
	if (c->cls->init && c->cls->init(c) != 0)
		return 1;
	if (mpctxt_addr && *mpctxt_addr == NULL && c->cls->mp_init &&
	    c->cls->mp_init(c) != 0) /* continue even if mp_init fails */
		c->mpcontext = NULL;
	return 0;
}

int checker_mp_init(struct checker * c, void ** mpctxt_addr)
{
	if (!c || !c->cls)
		return 1;
	if (c->mpcontext || !mpctxt_addr)
		return 0;
	c->mpcontext = mpctxt_addr;
	if (*mpctxt_addr == NULL && c->cls->mp_init &&
	    c->cls->mp_init(c) != 0) {
		c->mpcontext = NULL;
		return 1;
	}
	return 0;
}

void checker_clear (struct checker *c)
{
	memset(c, 0x0, sizeof(struct checker));
	c->fd = -1;
}

void checker_put (struct checker * dst)
{
	struct checker_class *src;

	if (!dst)
		return;
	src = dst->cls;

	if (src && src->free)
		src->free(dst);
	checker_clear(dst);
	put_shared_ptr(src);
}

int checker_get_state(struct checker *c)
{
	if (!c || !c->cls)
		return PATH_UNCHECKED;
	if (c->path_state != PATH_PENDING || !c->cls->pending)
		return c->path_state;
	c->path_state = c->cls->pending(c);
	return c->path_state;
}

bool checker_need_wait(struct checker *c)
{
	if (!c || !c->cls || c->path_state != PATH_PENDING ||
	    !c->cls->need_wait)
		return false;
	return c->cls->need_wait(c);
}

void checker_check (struct checker * c, int path_state)
{
	if (!c)
		return;

	c->msgid = CHECKER_MSGID_NONE;
	if (c->disable) {
		c->msgid = CHECKER_MSGID_DISABLED;
		c->path_state = PATH_UNCHECKED;
	} else if (!strncmp(c->cls->name, NONE, 4)) {
		c->path_state = path_state;
	} else if (c->fd < 0) {
		c->msgid = CHECKER_MSGID_NO_FD;
		c->path_state = PATH_WILD;
	} else {
		c->path_state = c->cls->check(c);
	}
}

const char *checker_name(const struct checker *c)
{
	if (!c || !c->cls)
		return NULL;
	return c->cls->name;
}

int checker_is_sync(const struct checker *c)
{
	return c && c->cls && c->cls->sync;
}

static const char *generic_msg[CHECKER_GENERIC_MSGTABLE_SIZE] = {
	[CHECKER_MSGID_NONE] = "",
	[CHECKER_MSGID_DISABLED] = " is disabled",
	[CHECKER_MSGID_NO_FD] = " has no usable fd",
	[CHECKER_MSGID_INVALID] = " provided invalid message id",
	[CHECKER_MSGID_UP] = " reports path is up",
	[CHECKER_MSGID_DOWN] = " reports path is down",
	[CHECKER_MSGID_GHOST] = " reports path is ghost",
	[CHECKER_MSGID_UNSUPPORTED] = " doesn't support this device",
	[CHECKER_MSGID_DISCONNECTED] = " no access to this device",
	[CHECKER_MSGID_TIMEOUT] = " timed out",
	[CHECKER_MSGID_RUNNING] = " still running",
};

const char *checker_message(const struct checker *c)
{
	int id;

	if (!c || !c->cls || c->msgid < 0 ||
	    (c->msgid >= CHECKER_GENERIC_MSGTABLE_SIZE &&
	     c->msgid < CHECKER_FIRST_MSGID))
		goto bad_id;

	if (c->msgid < CHECKER_GENERIC_MSGTABLE_SIZE)
		return generic_msg[c->msgid];

	id = c->msgid - CHECKER_FIRST_MSGID;
	if (id < c->cls->msgtable_size)
		return c->cls->msgtable[id];

bad_id:
	return generic_msg[CHECKER_MSGID_NONE];
}

void checker_clear_message (struct checker *c)
{
	if (!c)
		return;
	c->msgid = CHECKER_MSGID_NONE;
}

void checker_get(struct checker *dst, const char *name)
{
	struct checker_class *src = NULL;

	if (!dst)
		return;

	if (name && strlen(name)) {
		src = checker_class_lookup(name);
		if (!src)
			src = add_checker_class(name);
	}
	dst->cls = src;
	if (!src)
		return;

	get_shared_ptr(dst->cls);
}

int init_checkers(void)
{
#ifdef LOAD_ALL_SHARED_LIBS
	static const char *const all_checkers[] = {
		DIRECTIO,
		TUR,
		HP_SW,
		RDAC,
		EMC_CLARIION,
		READSECTOR0,
		CCISS_TUR,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(all_checkers); i++)
		add_checker_class(all_checkers[i]);
#else
	if (!add_checker_class(DEFAULT_CHECKER))
		return 1;
#endif
	return 0;
}
