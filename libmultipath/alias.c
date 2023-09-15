/*
 * Copyright (c) 2005 Christophe Varoqui
 * Copyright (c) 2005 Benjamin Marzinski, Redhat
 */
#include <stdlib.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <stdbool.h>

#include "debug.h"
#include "util.h"
#include "uxsock.h"
#include "alias.h"
#include "file.h"
#include "vector.h"
#include "checkers.h"
#include "structs.h"
#include "config.h"
#include "util.h"
#include "errno.h"
#include "devmapper.h"


/*
 * significant parts of this file were taken from iscsi-bindings.c of the
 * linux-iscsi project.
 * Copyright (C) 2002 Cisco Systems, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * See the file COPYING included with this distribution for more details.
 */

#define BINDINGS_FILE_HEADER		\
"# Multipath bindings, Version : 1.0\n" \
"# NOTE: this file is automatically maintained by the multipath program.\n" \
"# You should not need to edit this file in normal circumstances.\n" \
"#\n" \
"# Format:\n" \
"# alias wwid\n" \
"#\n"

static const char bindings_file_header[] = BINDINGS_FILE_HEADER;

int
valid_alias(const char *alias)
{
	if (strchr(alias, '/') != NULL)
		return 0;
	return 1;
}


static int
format_devname(char *name, int id, int len, const char *prefix)
{
	int pos;
	int prefix_len = strlen(prefix);

	if (len <= prefix_len + 1 || id <= 0)
		return -1;

	memset(name, 0, len);
	strcpy(name, prefix);
	name[len - 1] = '\0';
	for (pos = len - 2; pos >= prefix_len; pos--) {
		id--;
		name[pos] = 'a' + id % 26;
		if (id < 26)
			break;
		id /= 26;
	}
	if (pos < prefix_len)
		return -1;

	memmove(name + prefix_len, name + pos, len - pos);
	return (prefix_len + len - pos - 1);
}

static int
scan_devname(const char *alias, const char *prefix)
{
	const char *c;
	int i, n = 0;
	static const int last_26 = INT_MAX / 26;

	if (!prefix || strncmp(alias, prefix, strlen(prefix)))
		return -1;

	if (strlen(alias) == strlen(prefix))
		return -1;

	if (strlen(alias) > strlen(prefix) + 7)
		/* id of 'aaaaaaaa' overflows int */
		return -1;

	c = alias + strlen(prefix);
	while (*c != '\0' && *c != ' ' && *c != '\t') {
		if (*c < 'a' || *c > 'z')
			return -1;
		i = *c - 'a';
		if (n > last_26 || (n == last_26 && i >= INT_MAX % 26))
			return -1;
		n = n * 26 + i;
		c++;
		n++;
	}

	return n;
}

static bool alias_already_taken(const char *alias, const char *map_wwid)
{
	if (dm_map_present(alias)) {
		char wwid[WWID_SIZE];

		/* If both the name and the wwid match, then it's fine.*/
		if (dm_get_uuid(alias, wwid, sizeof(wwid)) == 0 &&
		    strncmp(map_wwid, wwid, sizeof(wwid)) == 0)
			return false;
		condlog(3, "%s: alias '%s' already taken, but not in bindings file. reselecting alias",
			map_wwid, alias);
		return true;
	}
	return false;
}

static bool id_already_taken(int id, const char *prefix, const char *map_wwid)
{
	char alias[LINE_MAX];

	if (format_devname(alias, id, LINE_MAX, prefix) < 0)
		return false;

	return alias_already_taken(alias, map_wwid);
}

/*
 * Returns: 0   if matching entry in WWIDs file found
 *         -1   if an error occurs
 *         >0   a free ID that could be used for the WWID at hand
 * *map_alias is set to a freshly allocated string with the matching alias if
 * the function returns 0, or to NULL otherwise.
 */
static int
lookup_binding(FILE *f, const char *map_wwid, char **map_alias,
	       const char *prefix, int check_if_taken)
{
	char buf[LINE_MAX];
	unsigned int line_nr = 0;
	int id = 1;
	int biggest_id = 1;
	int smallest_bigger_id = INT_MAX;

	*map_alias = NULL;

	rewind(f);
	while (fgets(buf, LINE_MAX, f)) {
		const char *alias, *wwid;
		char *c, *saveptr;
		int curr_id;

		line_nr++;
		c = strpbrk(buf, "#\n\r");
		if (c)
			*c = '\0';
		alias = strtok_r(buf, " \t", &saveptr);
		if (!alias) /* blank line */
			continue;
		curr_id = scan_devname(alias, prefix);
		if (curr_id == id) {
			if (id < INT_MAX)
				id++;
			else {
				id = -1;
				break;
			}
		}
		if (curr_id > biggest_id)
			biggest_id = curr_id;
		if (curr_id > id && curr_id < smallest_bigger_id)
			smallest_bigger_id = curr_id;
		wwid = strtok_r(NULL, " \t", &saveptr);
		if (!wwid){
			condlog(3,
				"Ignoring malformed line %u in bindings file",
				line_nr);
			continue;
		}
		if (strcmp(wwid, map_wwid) == 0){
			condlog(3, "Found matching wwid [%s] in bindings file."
				" Setting alias to %s", wwid, alias);
			*map_alias = strdup(alias);
			if (*map_alias == NULL) {
				condlog(0, "Cannot copy alias from bindings "
					"file: out of memory");
				return -1;
			}
			return 0;
		}
	}
	if (!prefix && check_if_taken)
		id = -1;
	if (id >= smallest_bigger_id) {
		if (biggest_id < INT_MAX)
			id = biggest_id + 1;
		else
			id = -1;
	}
	if (id > 0 && check_if_taken) {
		while(id_already_taken(id, prefix, map_wwid)) {
			if (id == INT_MAX) {
				id = -1;
				break;
			}
			id++;
			if (id == smallest_bigger_id) {
				if (biggest_id == INT_MAX) {
					id = -1;
					break;
				}
				if (biggest_id >= smallest_bigger_id)
					id = biggest_id + 1;
			}
		}
	}
	if (id < 0) {
		condlog(0, "no more available user_friendly_names");
		return -1;
	} else
		condlog(3, "No matching wwid [%s] in bindings file.", map_wwid);
	return id;
}

static int
rlookup_binding(FILE *f, char *buff, const char *map_alias)
{
	char line[LINE_MAX];
	unsigned int line_nr = 0;

	buff[0] = '\0';

	while (fgets(line, LINE_MAX, f)) {
		char *c, *saveptr;
		const char *alias, *wwid;

		line_nr++;
		c = strpbrk(line, "#\n\r");
		if (c)
			*c = '\0';
		alias = strtok_r(line, " \t", &saveptr);
		if (!alias) /* blank line */
			continue;
		wwid = strtok_r(NULL, " \t", &saveptr);
		if (!wwid){
			condlog(3,
				"Ignoring malformed line %u in bindings file",
				line_nr);
			continue;
		}
		if (strlen(wwid) > WWID_SIZE - 1) {
			condlog(3,
				"Ignoring too large wwid at %u in bindings file", line_nr);
			continue;
		}
		if (strcmp(alias, map_alias) == 0){
			condlog(3, "Found matching alias [%s] in bindings file."
				" Setting wwid to %s", alias, wwid);
			strlcpy(buff, wwid, WWID_SIZE);
			return 0;
		}
	}
	condlog(3, "No matching alias [%s] in bindings file.", map_alias);

	return -1;
}

static char *
allocate_binding(int fd, const char *wwid, int id, const char *prefix)
{
	char buf[LINE_MAX];
	off_t offset;
	char *alias, *c;
	int i;

	if (id <= 0) {
		condlog(0, "%s: cannot allocate new binding for id %d",
			__func__, id);
		return NULL;
	}

	i = format_devname(buf, id, LINE_MAX, prefix);
	if (i == -1)
		return NULL;

	c = buf + i;
	if (snprintf(c, LINE_MAX - i, " %s\n", wwid) >= LINE_MAX - i) {
		condlog(1, "%s: line too long for %s\n", __func__, wwid);
		return NULL;
	}
	buf[LINE_MAX - 1] = '\0';

	offset = lseek(fd, 0, SEEK_END);
	if (offset < 0){
		condlog(0, "Cannot seek to end of bindings file : %s",
			strerror(errno));
		return NULL;
	}
	if (write(fd, buf, strlen(buf)) != (ssize_t)strlen(buf)){
		condlog(0, "Cannot write binding to bindings file : %s",
			strerror(errno));
		/* clear partial write */
		if (ftruncate(fd, offset))
			condlog(0, "Cannot truncate the header : %s",
				strerror(errno));
		return NULL;
	}
	c = strchr(buf, ' ');
	if (c)
		*c = '\0';

	condlog(3, "Created new binding [%s] for WWID [%s]", buf, wwid);
	alias = strdup(buf);
	if (alias == NULL)
		condlog(0, "cannot copy new alias from bindings file: out of memory");

	return alias;
}

char *
use_existing_alias (const char *wwid, const char *file, const char *alias_old,
		    const char *prefix, int bindings_read_only)
{
	char *alias = NULL;
	int id = 0;
	int fd, can_write;
	char buff[WWID_SIZE];
	FILE *f;

	fd = open_file(file, &can_write, bindings_file_header);
	if (fd < 0)
		return NULL;

	f = fdopen(fd, "r");
	if (!f) {
		condlog(0, "cannot fdopen on bindings file descriptor");
		close(fd);
		return NULL;
	}
	/* lookup the binding. if it exists, the wwid will be in buff
	 * either way, id contains the id for the alias
	 */
	rlookup_binding(f, buff, alias_old);

	if (strlen(buff) > 0) {
		/* if buff is our wwid, it's already
		 * allocated correctly
		 */
		if (strcmp(buff, wwid) == 0)
			alias = STRDUP(alias_old);
		else {
			alias = NULL;
			condlog(0, "alias %s already bound to wwid %s, cannot reuse",
				alias_old, buff);
		}
		goto out;
	}

	id = lookup_binding(f, wwid, &alias, NULL, 0);
	if (alias) {
		condlog(3, "Use existing binding [%s] for WWID [%s]",
			alias, wwid);
		goto out;
	}

	/* allocate the existing alias in the bindings file */
	id = scan_devname(alias_old, prefix);
	if (id <= 0)
		goto out;

	if (fflush(f) != 0) {
		condlog(0, "cannot fflush bindings file stream : %s",
			strerror(errno));
		goto out;
	}

	if (can_write && !bindings_read_only) {
		alias = allocate_binding(fd, wwid, id, prefix);
		condlog(0, "Allocated existing binding [%s] for WWID [%s]",
			alias, wwid);
	}

out:
	pthread_cleanup_push(free, alias);
	fclose(f);
	pthread_cleanup_pop(0);
	return alias;
}

char *
get_user_friendly_alias(const char *wwid, const char *file, const char *prefix,
			int bindings_read_only)
{
	char *alias;
	int fd, id;
	FILE *f;
	int can_write;

	if (!wwid || *wwid == '\0') {
		condlog(3, "Cannot find binding for empty WWID");
		return NULL;
	}

	fd = open_file(file, &can_write, bindings_file_header);
	if (fd < 0)
		return NULL;

	f = fdopen(fd, "r");
	if (!f) {
		condlog(0, "cannot fdopen on bindings file descriptor : %s",
			strerror(errno));
		close(fd);
		return NULL;
	}

	id = lookup_binding(f, wwid, &alias, prefix, 1);
	if (id < 0) {
		fclose(f);
		return NULL;
	}

	pthread_cleanup_push(free, alias);

	if (fflush(f) != 0) {
		condlog(0, "cannot fflush bindings file stream : %s",
			strerror(errno));
		free(alias);
		alias = NULL;
	} else if (can_write && !bindings_read_only && !alias)
		alias = allocate_binding(fd, wwid, id, prefix);

	fclose(f);

	pthread_cleanup_pop(0);
	return alias;
}

int
get_user_friendly_wwid(const char *alias, char *buff, const char *file)
{
	int fd, unused;
	FILE *f;

	if (!alias || *alias == '\0') {
		condlog(3, "Cannot find binding for empty alias");
		return -1;
	}

	fd = open_file(file, &unused, bindings_file_header);
	if (fd < 0)
		return -1;

	f = fdopen(fd, "r");
	if (!f) {
		condlog(0, "cannot fdopen on bindings file descriptor : %s",
			strerror(errno));
		close(fd);
		return -1;
	}

	rlookup_binding(f, buff, alias);
	if (!strlen(buff)) {
		fclose(f);
		return -1;
	}

	fclose(f);
	return 0;
}

struct binding {
	char *alias;
	char *wwid;
};

static void _free_binding(struct binding *bdg)
{
	free(bdg->wwid);
	free(bdg->alias);
	free(bdg);
}

/*
 * Perhaps one day we'll implement this more efficiently, thus use
 * an abstract type.
 */
typedef struct _vector Bindings;

static void free_bindings(Bindings *bindings)
{
	struct binding *bdg;
	int i;

	vector_foreach_slot(bindings, bdg, i)
		_free_binding(bdg);
	vector_reset(bindings);
}

enum {
	BINDING_EXISTS,
	BINDING_CONFLICT,
	BINDING_ADDED,
	BINDING_DELETED,
	BINDING_NOTFOUND,
	BINDING_ERROR,
};

static int add_binding(Bindings *bindings, const char *alias, const char *wwid)
{
	struct binding *bdg;
	int i, cmp = 0;

	/*
	 * Keep the bindings array sorted by alias.
	 * Optimization: Search backwards, assuming that the bindings file is
	 * sorted already.
	 */
	vector_foreach_slot_backwards(bindings, bdg, i) {
		if ((cmp = strcmp(bdg->alias, alias)) <= 0)
			break;
	}

	/* Check for exact match */
	if (i >= 0 && cmp == 0)
		return strcmp(bdg->wwid, wwid) ?
			BINDING_CONFLICT : BINDING_EXISTS;

	i++;
	bdg = calloc(1, sizeof(*bdg));
	if (bdg) {
		bdg->wwid = strdup(wwid);
		bdg->alias = strdup(alias);
		if (bdg->wwid && bdg->alias &&
		    vector_insert_slot(bindings, i, bdg))
			return BINDING_ADDED;
		else
			_free_binding(bdg);
	}

	return BINDING_ERROR;
}

static int write_bindings_file(const Bindings *bindings, int fd)
{
	struct binding *bnd;
	char line[LINE_MAX];
	int i;

	if (write(fd, BINDINGS_FILE_HEADER, sizeof(BINDINGS_FILE_HEADER) - 1)
	    != sizeof(BINDINGS_FILE_HEADER) - 1)
		return -1;

	vector_foreach_slot(bindings, bnd, i) {
		int len;

		len = snprintf(line, sizeof(line), "%s %s\n",
			       bnd->alias, bnd->wwid);

		if (len < 0 || (size_t)len >= sizeof(line)) {
			condlog(1, "%s: line overflow", __func__);
			return -1;
		}

		if (write(fd, line, len) != len)
			return -1;
	}
	return 0;
}

static int fix_bindings_file(const struct config *conf,
			     const Bindings *bindings)
{
	int rc;
	long fd;
	char tempname[PATH_MAX];

	if (safe_sprintf(tempname, "%s.XXXXXX", conf->bindings_file))
		return -1;
	if ((fd = mkstemp(tempname)) == -1) {
		condlog(1, "%s: mkstemp: %m", __func__);
		return -1;
	}
	pthread_cleanup_push(close_fd, (void*)fd);
	rc = write_bindings_file(bindings, fd);
	pthread_cleanup_pop(1);
	if (rc == -1) {
		condlog(1, "failed to write new bindings file %s",
			tempname);
		unlink(tempname);
		return rc;
	}
	if ((rc = rename(tempname, conf->bindings_file)) == -1)
		condlog(0, "%s: rename: %m", __func__);
	else
		condlog(1, "updated bindings file %s", conf->bindings_file);
	return rc;
}

static int _check_bindings_file(const struct config *conf, FILE *file,
				 Bindings *bindings)
{
	int rc = 0;
	unsigned int linenr = 0;
	char *line = NULL;
	size_t line_len = 0;
	ssize_t n;

	pthread_cleanup_push(cleanup_free_ptr, &line);
	while ((n = getline(&line, &line_len, file)) >= 0) {
		char *c, *alias, *wwid, *saveptr;
		const char *mpe_wwid;

		linenr++;
		c = strpbrk(line, "#\n\r");
		if (c)
			*c = '\0';
		alias = strtok_r(line, " \t", &saveptr);
		if (!alias) /* blank line */
			continue;
		wwid = strtok_r(NULL, " \t", &saveptr);
		if (!wwid) {
			condlog(1, "invalid line %d in bindings file, missing WWID",
				linenr);
			continue;
		}
		c = strtok_r(NULL, " \t", &saveptr);
		if (c)
			/* This is non-fatal */
			condlog(1, "invalid line %d in bindings file, extra args \"%s\"",
				linenr, c);

		mpe_wwid = get_mpe_wwid(conf->mptable, alias);
		if (mpe_wwid && strcmp(mpe_wwid, wwid)) {
			condlog(0, "ERROR: alias \"%s\" for WWID %s in bindings file "
				"on line %u conflicts with multipath.conf entry for %s",
				alias, wwid, linenr, mpe_wwid);
			rc = -1;
			continue;
		}

		switch (add_binding(bindings, alias, wwid)) {
		case BINDING_CONFLICT:
			condlog(0, "ERROR: multiple bindings for alias \"%s\" in "
				"bindings file on line %u, discarding binding to WWID %s",
				alias, linenr, wwid);
			rc = -1;
			break;
		case BINDING_EXISTS:
			condlog(2, "duplicate line for alias %s in bindings file on line %u",
				alias, linenr);
			break;
		case BINDING_ERROR:
			condlog(2, "error adding binding %s -> %s",
				alias, wwid);
			break;
		default:
			break;
		}
	}
	pthread_cleanup_pop(1);
	return rc;
}

static void cleanup_fclose(void *p)
{
	fclose(p);
}

static int alias_compar(const void *p1, const void *p2)
{
	const char *alias1 = (*(struct mpentry * const *)p1)->alias;
	const char *alias2 = (*(struct mpentry * const *)p2)->alias;

	if (alias1 && alias2)
		return strcmp(alias1, alias2);
	else
		/* Move NULL alias to the end */
		return alias1 ? -1 : alias2 ? 1 : 0;
}

static void cleanup_vector_free(void *arg)
{
	if  (arg)
		vector_free((vector)arg);
}

/*
 * check_alias_settings(): test for inconsistent alias configuration
 *
 * It's a fatal configuration error if the same alias is assigned to
 * multiple WWIDs. In the worst case, it can cause data corruption
 * by mangling devices with different WWIDs into the same multipath map.
 * This function tests the configuration from multipath.conf and the
 * bindings file for consistency, drops inconsistent multipath.conf
 * alias settings, and rewrites the bindings file if necessary, dropping
 * conflicting lines (if user_friendly_names is on, multipathd will
 * fill in the deleted lines with a newly generated alias later).
 * Note that multipath.conf is not rewritten. Use "multipath -T" for that.
 *
 * Returns: 0 in case of success, -1 if the configuration was bad
 * and couldn't be fixed.
 */
int check_alias_settings(const struct config *conf)
{
	int can_write;
	int rc = 0, i, fd;
	Bindings bindings = {.allocated = 0, };
	vector mptable = NULL;
	struct mpentry *mpe;

	mptable = vector_convert(NULL, conf->mptable, struct mpentry *, identity);
	if (!mptable)
		return -1;

	pthread_cleanup_push_cast(free_bindings, &bindings);
	pthread_cleanup_push(cleanup_vector_free, mptable);

	vector_sort(mptable, alias_compar);
	vector_foreach_slot(mptable, mpe, i) {
		if (!mpe->alias)
			/*
			 * alias_compar() sorts NULL alias at the end,
			 * so we can stop if we encounter this.
			 */
			break;
		if (add_binding(&bindings, mpe->alias, mpe->wwid) ==
		    BINDING_CONFLICT) {
			condlog(0, "ERROR: alias \"%s\" bound to multiple wwids in multipath.conf, "
				"discarding binding to %s",
				mpe->alias, mpe->wwid);
			free(mpe->alias);
			mpe->alias = NULL;
		}
	}
	/* This clears the bindings */
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);

	pthread_cleanup_push_cast(free_bindings, &bindings);
	fd = open_file(conf->bindings_file, &can_write, BINDINGS_FILE_HEADER);
	if (fd != -1) {
		FILE *file = fdopen(fd, "r");

		if (file != NULL) {
			pthread_cleanup_push(cleanup_fclose, file);
			rc = _check_bindings_file(conf, file, &bindings);
			pthread_cleanup_pop(1);
			if (rc == -1 && can_write && !conf->bindings_read_only)
				rc = fix_bindings_file(conf, &bindings);
			else if (rc == -1)
				condlog(0, "ERROR: bad settings in read-only bindings file %s",
					conf->bindings_file);
		} else {
			condlog(1, "failed to fdopen %s: %m",
				conf->bindings_file);
			close(fd);
		}
	}
	pthread_cleanup_pop(1);
	return rc;
}
