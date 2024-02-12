/*
 * Based on Alexandre Cassen template for keepalived
 * Copyright (c) 2004, 2005, 2006  Christophe Varoqui
 * Copyright (c) 2005 Benjamin Marzinski, Redhat
 * Copyright (c) 2005 Kiyoshi Ueda, NEC
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <string.h>
#include "checkers.h"
#include "vector.h"
#include "hwtable.h"
#include "structs.h"
#include "parser.h"
#include "config.h"
#include "debug.h"
#include "pgpolicies.h"
#include "blacklist.h"
#include "defaults.h"
#include "prio.h"
#include "util.h"
#include <errno.h>
#include <inttypes.h>
#include <libudev.h>
#include "autoconfig.h"
#include "mpath_cmd.h"
#include "dict.h"
#include "strbuf.h"
#include "prkey.h"

static void
do_set_int(vector strvec, void *ptr, int min, int max, const char *file,
	int line_nr, char *buff)
{
	int *int_ptr = (int *)ptr;
	char *eptr;
	long res;

	res = strtol(buff, &eptr, 10);
	if (eptr > buff)
		while (isspace(*eptr))
			eptr++;
	if (*buff == '\0' || *eptr != '\0') {
		condlog(1, "%s line %d, invalid value for %s: \"%s\"",
			file, line_nr, (char*)VECTOR_SLOT(strvec, 0), buff);
		return;
	}
	if (res > max || res < min) {
		res = (res > max) ? max : min;
		condlog(1, "%s line %d, value for %s too %s, capping at %ld",
			file, line_nr, (char*)VECTOR_SLOT(strvec, 0),
		(res == max)? "large" : "small", res);
	}
	*int_ptr = res;
	return;
}

static int
set_int(vector strvec, void *ptr, int min, int max, const char *file,
	int line_nr)
{
	char *buff;

	buff = set_value(strvec);
	if (!buff)
		return 1;

	do_set_int(strvec, ptr, min, max, file, line_nr, buff);

	free(buff);
	return 0;
}

static int
set_uint(vector strvec, void *ptr, const char *file, int line_nr)
{
	unsigned int *uint_ptr = (unsigned int *)ptr;
	char *buff, *eptr, *p;
	unsigned long res;

	buff = set_value(strvec);
	if (!buff)
		return 1;

	p = buff;
	while (isspace(*p))
		p++;
	res = strtoul(p, &eptr, 10);
	if (eptr > buff)
		while (isspace(*eptr))
			eptr++;
	if (*buff == '\0' || *eptr != '\0' || !isdigit(*p) || res > UINT_MAX)
		condlog(1, "%s line %d, invalid value for %s: \"%s\"",
			file, line_nr, (char*)VECTOR_SLOT(strvec, 0), buff);
	else
		*uint_ptr = res;

	free(buff);
	return 0;
}

static int
set_str(vector strvec, void *ptr, const char *file, int line_nr)
{
	char **str_ptr = (char **)ptr;

	if (*str_ptr)
		free(*str_ptr);
	*str_ptr = set_value(strvec);

	if (!*str_ptr)
		return 1;

	return 0;
}

static int
set_arg_str(vector strvec, void *ptr, int count_idx, const char *file,
	    int line_nr)
{
	char **str_ptr = (char **)ptr;
	char *old_str = *str_ptr;
	const char * const spaces = " \f\r\t\v";
	char *p, *end;
	int idx = -1;
	long int count = -1;

	*str_ptr = set_value(strvec);
	if (!*str_ptr) {
		free(old_str);
		return 1;
	}
	p = *str_ptr;
	while (*p != '\0') {
		p += strspn(p, spaces);
		if (*p == '\0')
			break;
		idx += 1;
		if (idx == count_idx) {
			errno = 0;
			count = strtol(p, &end, 10);
			if (errno == ERANGE || end == p ||
			    !(isspace(*end) || *end == '\0')) {
				count = -1;
				break;
			}
		}
		p += strcspn(p, spaces);
	}
	if (count < 0) {
		condlog(1, "%s line %d, missing argument count for %s",
			file, line_nr, (char*)VECTOR_SLOT(strvec, 0));
		goto fail;
	}
	if (count != idx - count_idx) {
		condlog(1, "%s line %d, invalid argument count for %s:, got '%ld' expected '%d'",
			file, line_nr, (char*)VECTOR_SLOT(strvec, 0), count,
			idx - count_idx);
		goto fail;
	}
	free(old_str);
	return 0;
fail:
	free(*str_ptr);
	*str_ptr = old_str;
	return 0;
}

static int
set_str_noslash(vector strvec, void *ptr, const char *file, int line_nr)
{
	char **str_ptr = (char **)ptr;
	char *old_str = *str_ptr;

	*str_ptr = set_value(strvec);
	if (!*str_ptr) {
		free(old_str);
		return 1;
	}
	if (strchr(*str_ptr, '/')) {
		condlog(1, "%s line %d, %s cannot contain a slash. Ignoring",
			file, line_nr, *str_ptr);
		free(*str_ptr);
		*str_ptr = old_str;
	} else
		free(old_str);
	return 0;
}

static int
set_yes_no(vector strvec, void *ptr, const char *file, int line_nr)
{
	char * buff;
	int *int_ptr = (int *)ptr;

	buff = set_value(strvec);
	if (!buff)
		return 1;

	if (strcmp(buff, "yes") == 0 || strcmp(buff, "1") == 0)
		*int_ptr = YN_YES;
	else if (strcmp(buff, "no") == 0 || strcmp(buff, "0") == 0)
		*int_ptr = YN_NO;
	else
		condlog(1, "%s line %d, invalid value for %s: \"%s\"",
			file, line_nr, (char*)VECTOR_SLOT(strvec, 0), buff);

	free(buff);
	return 0;
}

static int
set_yes_no_undef(vector strvec, void *ptr, const char *file, int line_nr)
{
	char * buff;
	int *int_ptr = (int *)ptr;

	buff = set_value(strvec);
	if (!buff)
		return 1;

	if (strcmp(buff, "no") == 0 || strcmp(buff, "0") == 0)
		*int_ptr = YNU_NO;
	else if (strcmp(buff, "yes") == 0 || strcmp(buff, "1") == 0)
		*int_ptr = YNU_YES;
	else
		condlog(1, "%s line %d, invalid value for %s: \"%s\"",
			file, line_nr, (char*)VECTOR_SLOT(strvec, 0), buff);

	free(buff);
	return 0;
}

static int print_int(struct strbuf *buff, long v)
{
	return print_strbuf(buff, "%li", v);
}

static int print_nonzero(struct strbuf *buff, long v)
{
	if (!v)
		return 0;
	return print_strbuf(buff, "%li", v);
}

static int print_str(struct strbuf *buff, const char *ptr)
{
	int ret = append_strbuf_quoted(buff, ptr);

	/*
	 * -EINVAL aka (ptr == NULL) means "not set".
	 * Returning an error here breaks unit tests
	 * (logic in snprint_keyword()).
	 */
	return ret == -EINVAL ? 0 : ret;
}

static int print_yes_no(struct strbuf *buff, long v)
{
	return append_strbuf_quoted(buff, v == YN_NO ? "no" : "yes");
}

static int print_yes_no_undef(struct strbuf *buff, long v)
{
	if (!v)
		return 0;
	return append_strbuf_quoted(buff, v == YNU_NO? "no" : "yes");
}

#define declare_def_handler(option, function)				\
static int								\
def_ ## option ## _handler (struct config *conf, vector strvec,		\
			    const char *file, int line_nr)		\
{									\
	return function (strvec, &conf->option, file, line_nr);		\
}

#define declare_def_warn_handler(option, function)			\
static int								\
def_ ## option ## _handler (struct config *conf, vector strvec,		\
			    const char *file, int line_nr)		\
{									\
	static bool warned;						\
	if (!warned) {							\
		condlog(2, "%s line %d, \"" #option "\" is deprecated and will be disabled in a future release", file, line_nr); \
		warned = true;						\
	}								\
	return function (strvec, &conf->option, file, line_nr);		\
}

static int deprecated_handler(struct config *conf, vector strvec, const char *file,
			      int line_nr);

#define declare_deprecated_handler(option, default)				\
static int								\
deprecated_ ## option ## _handler (struct config *conf, vector strvec,	\
				   const char *file, int line_nr)	\
{									\
	static bool warned;						\
	if (!warned) {							\
		condlog(1, "%s line %d: ignoring deprecated option \""	\
			#option "\", using built-in value: \"%s\"",	\
			file, line_nr, default);			\
		warned = true;						\
	}								\
	return deprecated_handler(conf, strvec, file, line_nr);		\
}

#define declare_def_range_handler(option, minval, maxval)			\
static int								\
def_ ## option ## _handler (struct config *conf, vector strvec,         \
			    const char *file, int line_nr)		\
{									\
	return set_int(strvec, &conf->option, minval, maxval, file, line_nr); \
}

#define declare_def_arg_str_handler(option, count_idx)			\
static int								\
def_ ## option ## _handler (struct config *conf, vector strvec,		\
			    const char *file, int line_nr)		\
{									\
	return set_arg_str(strvec, &conf->option, count_idx, file, line_nr); \
}

#define declare_def_snprint(option, function)				\
static int								\
snprint_def_ ## option (struct config *conf, struct strbuf *buff,	\
			const void *data)				\
{									\
	return function(buff, conf->option);				\
}

#define declare_def_snprint_defint(option, function, value)		\
static int								\
snprint_def_ ## option (struct config *conf, struct strbuf *buff,	\
			const void *data)				\
{									\
	int i = value;							\
	if (!conf->option)						\
		return function(buff, i);				\
	return function (buff, conf->option);				\
}

#define declare_def_snprint_defstr(option, function, value)		\
static int								\
snprint_def_ ## option (struct config *conf, struct strbuf *buff,	\
			const void *data)				\
{									\
	static const char *s = value;					\
	if (!conf->option)						\
		return function(buff, s);				\
	return function(buff, conf->option);				\
}

#define declare_hw_handler(option, function)				\
static int								\
hw_ ## option ## _handler (struct config *conf, vector strvec,		\
			   const char *file, int line_nr)		\
{									\
	struct hwentry * hwe = VECTOR_LAST_SLOT(conf->hwtable);		\
	if (!hwe)							\
		return 1;						\
	return function (strvec, &hwe->option, file, line_nr);		\
}

#define declare_hw_range_handler(option, minval, maxval)		\
static int								\
hw_ ## option ## _handler (struct config *conf, vector strvec,		\
			   const char *file, int line_nr)		\
{									\
	struct hwentry * hwe = VECTOR_LAST_SLOT(conf->hwtable);		\
	if (!hwe)							\
		return 1;						\
	return set_int(strvec, &hwe->option, minval, maxval, file, line_nr); \
}

#define declare_hw_arg_str_handler(option, count_idx)			\
static int								\
hw_ ## option ## _handler (struct config *conf, vector strvec,		\
			    const char *file, int line_nr)		\
{									\
	struct hwentry * hwe = VECTOR_LAST_SLOT(conf->hwtable);		\
	if (!hwe)							\
		return 1;						\
	return set_arg_str(strvec, &hwe->option, count_idx, file, line_nr); \
}


#define declare_hw_snprint(option, function)				\
static int								\
snprint_hw_ ## option (struct config *conf, struct strbuf *buff,	\
		       const void *data)				\
{									\
	const struct hwentry * hwe = (const struct hwentry *)data;	\
	return function(buff, hwe->option);				\
}

#define declare_ovr_handler(option, function)				\
static int								\
ovr_ ## option ## _handler (struct config *conf, vector strvec,		\
			    const char *file, int line_nr)		\
{									\
	if (!conf->overrides)						\
		return 1;						\
	return function (strvec, &conf->overrides->option, file, line_nr); \
}

#define declare_ovr_range_handler(option, minval, maxval)		\
static int								\
ovr_ ## option ## _handler (struct config *conf, vector strvec,		\
			    const char *file, int line_nr)		\
{									\
	if (!conf->overrides)						\
		return 1;						\
	return set_int(strvec, &conf->overrides->option, minval, maxval, \
		       file, line_nr); \
}

#define declare_ovr_arg_str_handler(option, count_idx)			\
static int								\
ovr_ ## option ## _handler (struct config *conf, vector strvec,		\
			    const char *file, int line_nr)		\
{									\
	if (!conf->overrides)						\
		return 1;						\
	return set_arg_str(strvec, &conf->overrides->option, count_idx, file, line_nr); \
}

#define declare_ovr_snprint(option, function)				\
static int								\
snprint_ovr_ ## option (struct config *conf, struct strbuf *buff,	\
			const void *data)				\
{									\
	return function (buff, conf->overrides->option);		\
}

#define declare_mp_handler(option, function)				\
static int								\
mp_ ## option ## _handler (struct config *conf, vector strvec,		\
			   const char *file, int line_nr)		\
{									\
	struct mpentry * mpe = VECTOR_LAST_SLOT(conf->mptable);		\
	if (!mpe)							\
		return 1;						\
	return function (strvec, &mpe->option, file, line_nr);		\
}

#define declare_mp_range_handler(option, minval, maxval)		\
static int								\
mp_ ## option ## _handler (struct config *conf, vector strvec,		\
			   const char *file, int line_nr)		\
{									\
	struct mpentry * mpe = VECTOR_LAST_SLOT(conf->mptable);		\
	if (!mpe)							\
		return 1;						\
	return set_int(strvec, &mpe->option, minval, maxval, file, line_nr); \
}

#define declare_mp_arg_str_handler(option, count_idx)			\
static int								\
mp_ ## option ## _handler (struct config *conf, vector strvec,		\
			    const char *file, int line_nr)		\
{									\
	struct mpentry * mpe = VECTOR_LAST_SLOT(conf->mptable);		\
	if (!mpe)							\
		return 1;						\
	return set_arg_str(strvec, &mpe->option, count_idx, file, line_nr); \
}

#define declare_mp_snprint(option, function)				\
static int								\
snprint_mp_ ## option (struct config *conf, struct strbuf *buff,	\
		       const void *data)				\
{									\
	const struct mpentry * mpe = (const struct mpentry *)data;	\
	return function(buff, mpe->option);				\
}

#define declare_pc_handler(option, function)				\
static int								\
pc_ ## option ## _handler (struct config *conf, vector strvec,		\
			   const char *file, int line_nr)		\
{									\
	struct pcentry *pce;						\
	if (!conf->overrides || !conf->overrides->pctable)		\
		return 1;						\
	pce = VECTOR_LAST_SLOT(conf->overrides->pctable);		\
	if (!pce)							\
		return 1;						\
	return function (strvec, &pce->option, file, line_nr);		\
}

#define declare_pc_snprint(option, function)				\
static int								\
snprint_pc_ ## option (struct config *conf, struct strbuf *buff,	\
		       const void *data)				\
{									\
	const struct pcentry *pce  = (const struct pcentry *)data;	\
	return function(buff, pce->option);				\
}

static int checkint_handler(struct config *conf, vector strvec,
			    const char *file, int line_nr)
{
	int rc = set_uint(strvec, &conf->checkint, file, line_nr);

	if (rc)
		return rc;
	if (conf->checkint == CHECKINT_UNDEF)
		conf->checkint--;
	return 0;
}

declare_def_snprint(checkint, print_int)

declare_def_handler(max_checkint, set_uint)
declare_def_snprint(max_checkint, print_int)

declare_def_range_handler(verbosity, 0, MAX_VERBOSITY)
declare_def_snprint(verbosity, print_int)

declare_def_handler(reassign_maps, set_yes_no)
declare_def_snprint(reassign_maps, print_yes_no)


static int def_partition_delim_handler(struct config *conf, vector strvec,
				       const char *file, int line_nr)
{
	int rc = set_str_noslash(strvec, &conf->partition_delim, file, line_nr);

	if (rc != 0)
		return rc;

	if (!strcmp(conf->partition_delim, UNSET_PARTITION_DELIM)) {
		free(conf->partition_delim);
		conf->partition_delim = NULL;
	}
	return 0;
}

static int snprint_def_partition_delim(struct config *conf, struct strbuf *buff,
				       const void *data)
{
	if (default_partition_delim == NULL || conf->partition_delim != NULL)
		return print_str(buff, conf->partition_delim);
	else
		return print_str(buff, UNSET_PARTITION_DELIM);
}

static const char * const find_multipaths_optvals[] = {
	[FIND_MULTIPATHS_OFF] = "off",
	[FIND_MULTIPATHS_ON] = "on",
	[FIND_MULTIPATHS_STRICT] = "strict",
	[FIND_MULTIPATHS_GREEDY] = "greedy",
	[FIND_MULTIPATHS_SMART] = "smart",
};

static int
def_find_multipaths_handler(struct config *conf, vector strvec,
			    const char *file, int line_nr)
{
	char *buff;
	int i;

	buff = set_value(strvec);
	if (!buff)
		return 1;

	for (i = FIND_MULTIPATHS_OFF; i < __FIND_MULTIPATHS_LAST; i++) {
		if (find_multipaths_optvals[i] != NULL &&
		    !strcmp(buff, find_multipaths_optvals[i])) {
			conf->find_multipaths = i;
			break;
		}
	}

	if (i >= __FIND_MULTIPATHS_LAST) {
		if (strcmp(buff, "no") == 0 || strcmp(buff, "0") == 0)
			conf->find_multipaths = FIND_MULTIPATHS_OFF;
		else if (strcmp(buff, "yes") == 0 || strcmp(buff, "1") == 0)
			conf->find_multipaths = FIND_MULTIPATHS_ON;
		else
			condlog(1, "%s line %d, invalid value for find_multipaths: \"%s\"",
				file, line_nr, buff);
	}

	free(buff);
	return 0;
}

static int
snprint_def_find_multipaths(struct config *conf, struct strbuf *buff,
			    const void *data)
{
	return append_strbuf_quoted(buff,
			 find_multipaths_optvals[conf->find_multipaths]);
}

static const char * const marginal_pathgroups_optvals[] = {
	[MARGINAL_PATHGROUP_OFF] = "off",
	[MARGINAL_PATHGROUP_ON] = "on",
#ifdef FPIN_EVENT_HANDLER
	[MARGINAL_PATHGROUP_FPIN] = "fpin",
#endif
};

static int
def_marginal_pathgroups_handler(struct config *conf, vector strvec,
			    const char *file, int line_nr)
{
	char *buff;
	unsigned int i;

	buff = set_value(strvec);
	if (!buff)
		return 1;
	for (i = MARGINAL_PATHGROUP_OFF;
	     i < ARRAY_SIZE(marginal_pathgroups_optvals); i++) {
		if (marginal_pathgroups_optvals[i] != NULL &&
		    !strcmp(buff, marginal_pathgroups_optvals[i])) {
			conf->marginal_pathgroups = i;
			break;
		}
	}

	if (i >= ARRAY_SIZE(marginal_pathgroups_optvals)) {
		if (strcmp(buff, "no") == 0 || strcmp(buff, "0") == 0)
			conf->marginal_pathgroups = MARGINAL_PATHGROUP_OFF;
		else if (strcmp(buff, "yes") == 0 || strcmp(buff, "1") == 0)
			conf->marginal_pathgroups = MARGINAL_PATHGROUP_ON;
		/* This can only be true if FPIN_EVENT_HANDLER isn't defined,
		 * otherwise this check will have already happened above */
		else if (strcmp(buff, "fpin") == 0)
			condlog(1, "%s line %d, support for \"fpin\" is not compiled in for marginal_pathgroups", file, line_nr);
		else
			condlog(1, "%s line %d, invalid value for marginal_pathgroups: \"%s\"",
				file, line_nr, buff);
	}
	free(buff);
	return 0;
}

static int
snprint_def_marginal_pathgroups(struct config *conf, struct strbuf *buff,
			    const void *data)
{
	return append_strbuf_quoted(buff,
			 marginal_pathgroups_optvals[conf->marginal_pathgroups]);
}


declare_def_arg_str_handler(selector, 1)
declare_def_snprint_defstr(selector, print_str, DEFAULT_SELECTOR)
declare_hw_arg_str_handler(selector, 1)
declare_hw_snprint(selector, print_str)
declare_ovr_arg_str_handler(selector, 1)
declare_ovr_snprint(selector, print_str)
declare_mp_arg_str_handler(selector, 1)
declare_mp_snprint(selector, print_str)

static int snprint_uid_attrs(struct config *conf, struct strbuf *buff,
			     const void *dummy)
{
	int j, ret, total = 0;
	const char *att;

	vector_foreach_slot(&conf->uid_attrs, att, j) {
		ret = print_strbuf(buff, "%s%s", j == 0 ? "" : " ", att);
		if (ret < 0)
			return ret;
		total += ret;
	}
	return total;
}

static int uid_attrs_handler(struct config *conf, vector strvec,
			     const char *file, int line_nr)
{
	char *val;
	void *ptr;
	int i;

	vector_foreach_slot(&conf->uid_attrs, ptr, i)
		free(ptr);
	vector_reset(&conf->uid_attrs);

	val = set_value(strvec);
	if (!val)
		return 1;
	if (parse_uid_attrs(val, conf))
		condlog(1, "%s line %d,error parsing uid_attrs: \"%s\"", file,
			line_nr, val);
	else
		condlog(4, "parsed %d uid_attrs", VECTOR_SIZE(&conf->uid_attrs));
	free(val);
	return 0;
}

declare_def_handler(uid_attribute, set_str)
declare_def_snprint_defstr(uid_attribute, print_str, DEFAULT_UID_ATTRIBUTE)
declare_ovr_handler(uid_attribute, set_str)
declare_ovr_snprint(uid_attribute, print_str)
declare_hw_handler(uid_attribute, set_str)
declare_hw_snprint(uid_attribute, print_str)

declare_def_handler(prio_name, set_str)
declare_def_snprint_defstr(prio_name, print_str, DEFAULT_PRIO)
declare_ovr_handler(prio_name, set_str)
declare_ovr_snprint(prio_name, print_str)
declare_hw_handler(prio_name, set_str)
declare_hw_snprint(prio_name, print_str)
declare_mp_handler(prio_name, set_str)
declare_mp_snprint(prio_name, print_str)

declare_def_handler(alias_prefix, set_str_noslash)
declare_def_snprint_defstr(alias_prefix, print_str, DEFAULT_ALIAS_PREFIX)
declare_ovr_handler(alias_prefix, set_str_noslash)
declare_ovr_snprint(alias_prefix, print_str)
declare_hw_handler(alias_prefix, set_str_noslash)
declare_hw_snprint(alias_prefix, print_str)

declare_def_handler(prio_args, set_str)
declare_def_snprint_defstr(prio_args, print_str, DEFAULT_PRIO_ARGS)
declare_ovr_handler(prio_args, set_str)
declare_ovr_snprint(prio_args, print_str)
declare_hw_handler(prio_args, set_str)
declare_hw_snprint(prio_args, print_str)
declare_mp_handler(prio_args, set_str)
declare_mp_snprint(prio_args, print_str)

declare_def_arg_str_handler(features, 0)
declare_def_snprint_defstr(features, print_str, DEFAULT_FEATURES)
declare_ovr_arg_str_handler(features, 0)
declare_ovr_snprint(features, print_str)
declare_hw_arg_str_handler(features, 0)
declare_hw_snprint(features, print_str)
declare_mp_arg_str_handler(features, 0)
declare_mp_snprint(features, print_str)

declare_def_handler(checker_name, set_str)
declare_def_snprint_defstr(checker_name, print_str, DEFAULT_CHECKER)
declare_ovr_handler(checker_name, set_str)
declare_ovr_snprint(checker_name, print_str)
declare_hw_handler(checker_name, set_str)
declare_hw_snprint(checker_name, print_str)

declare_def_range_handler(minio, 0, INT_MAX)
declare_def_snprint_defint(minio, print_int, DEFAULT_MINIO)
declare_ovr_range_handler(minio, 0, INT_MAX)
declare_ovr_snprint(minio, print_nonzero)
declare_hw_range_handler(minio, 0, INT_MAX)
declare_hw_snprint(minio, print_nonzero)
declare_mp_range_handler(minio, 0, INT_MAX)
declare_mp_snprint(minio, print_nonzero)

declare_def_range_handler(minio_rq, 0, INT_MAX)
declare_def_snprint_defint(minio_rq, print_int, DEFAULT_MINIO_RQ)
declare_ovr_range_handler(minio_rq, 0, INT_MAX)
declare_ovr_snprint(minio_rq, print_nonzero)
declare_hw_range_handler(minio_rq, 0, INT_MAX)
declare_hw_snprint(minio_rq, print_nonzero)
declare_mp_range_handler(minio_rq, 0, INT_MAX)
declare_mp_snprint(minio_rq, print_nonzero)

declare_def_handler(queue_without_daemon, set_yes_no)
static int
snprint_def_queue_without_daemon(struct config *conf, struct strbuf *buff,
				 const void * data)
{
	const char *qwd = "unknown";

	switch (conf->queue_without_daemon) {
	case QUE_NO_DAEMON_OFF:
		qwd = "no";
		break;
	case QUE_NO_DAEMON_ON:
		qwd = "yes";
		break;
	case QUE_NO_DAEMON_FORCE:
		qwd = "forced";
		break;
	}
	return append_strbuf_quoted(buff, qwd);
}

declare_def_range_handler(checker_timeout, 0, INT_MAX)
declare_def_snprint(checker_timeout, print_nonzero)

declare_def_handler(allow_usb_devices, set_yes_no)
declare_def_snprint(allow_usb_devices, print_yes_no)

declare_def_handler(flush_on_last_del, set_yes_no_undef)
declare_def_snprint_defint(flush_on_last_del, print_yes_no_undef, DEFAULT_FLUSH)
declare_ovr_handler(flush_on_last_del, set_yes_no_undef)
declare_ovr_snprint(flush_on_last_del, print_yes_no_undef)
declare_hw_handler(flush_on_last_del, set_yes_no_undef)
declare_hw_snprint(flush_on_last_del, print_yes_no_undef)
declare_mp_handler(flush_on_last_del, set_yes_no_undef)
declare_mp_snprint(flush_on_last_del, print_yes_no_undef)

declare_def_handler(user_friendly_names, set_yes_no_undef)
declare_def_snprint_defint(user_friendly_names, print_yes_no_undef,
			   DEFAULT_USER_FRIENDLY_NAMES)
declare_ovr_handler(user_friendly_names, set_yes_no_undef)
declare_ovr_snprint(user_friendly_names, print_yes_no_undef)
declare_hw_handler(user_friendly_names, set_yes_no_undef)
declare_hw_snprint(user_friendly_names, print_yes_no_undef)
declare_mp_handler(user_friendly_names, set_yes_no_undef)
declare_mp_snprint(user_friendly_names, print_yes_no_undef)

declare_def_handler(retain_hwhandler, set_yes_no_undef)
declare_def_snprint_defint(retain_hwhandler, print_yes_no_undef,
			   DEFAULT_RETAIN_HWHANDLER)
declare_ovr_handler(retain_hwhandler, set_yes_no_undef)
declare_ovr_snprint(retain_hwhandler, print_yes_no_undef)
declare_hw_handler(retain_hwhandler, set_yes_no_undef)
declare_hw_snprint(retain_hwhandler, print_yes_no_undef)

declare_def_handler(detect_prio, set_yes_no_undef)
declare_def_snprint_defint(detect_prio, print_yes_no_undef,
			   DEFAULT_DETECT_PRIO)
declare_ovr_handler(detect_prio, set_yes_no_undef)
declare_ovr_snprint(detect_prio, print_yes_no_undef)
declare_hw_handler(detect_prio, set_yes_no_undef)
declare_hw_snprint(detect_prio, print_yes_no_undef)

declare_def_handler(detect_checker, set_yes_no_undef)
declare_def_snprint_defint(detect_checker, print_yes_no_undef,
			   DEFAULT_DETECT_CHECKER)
declare_ovr_handler(detect_checker, set_yes_no_undef)
declare_ovr_snprint(detect_checker, print_yes_no_undef)
declare_hw_handler(detect_checker, set_yes_no_undef)
declare_hw_snprint(detect_checker, print_yes_no_undef)

declare_def_handler(detect_pgpolicy, set_yes_no_undef)
declare_def_snprint_defint(detect_pgpolicy, print_yes_no_undef,
			   DEFAULT_DETECT_PGPOLICY)
declare_ovr_handler(detect_pgpolicy, set_yes_no_undef)
declare_ovr_snprint(detect_pgpolicy, print_yes_no_undef)
declare_hw_handler(detect_pgpolicy, set_yes_no_undef)
declare_hw_snprint(detect_pgpolicy, print_yes_no_undef)

declare_def_handler(detect_pgpolicy_use_tpg, set_yes_no_undef)
declare_def_snprint_defint(detect_pgpolicy_use_tpg, print_yes_no_undef,
			   DEFAULT_DETECT_PGPOLICY_USE_TPG)
declare_ovr_handler(detect_pgpolicy_use_tpg, set_yes_no_undef)
declare_ovr_snprint(detect_pgpolicy_use_tpg, print_yes_no_undef)
declare_hw_handler(detect_pgpolicy_use_tpg, set_yes_no_undef)
declare_hw_snprint(detect_pgpolicy_use_tpg, print_yes_no_undef)

declare_def_handler(force_sync, set_yes_no)
declare_def_snprint(force_sync, print_yes_no)

declare_def_handler(deferred_remove, set_yes_no_undef)
declare_def_snprint_defint(deferred_remove, print_yes_no_undef,
			   DEFAULT_DEFERRED_REMOVE)
declare_ovr_handler(deferred_remove, set_yes_no_undef)
declare_ovr_snprint(deferred_remove, print_yes_no_undef)
declare_hw_handler(deferred_remove, set_yes_no_undef)
declare_hw_snprint(deferred_remove, print_yes_no_undef)
declare_mp_handler(deferred_remove, set_yes_no_undef)
declare_mp_snprint(deferred_remove, print_yes_no_undef)

declare_def_range_handler(retrigger_tries, 0, INT_MAX)
declare_def_snprint(retrigger_tries, print_int)

declare_def_range_handler(retrigger_delay, 0, INT_MAX)
declare_def_snprint(retrigger_delay, print_int)

declare_def_range_handler(uev_wait_timeout, 0, INT_MAX)
declare_def_snprint(uev_wait_timeout, print_int)

declare_def_handler(strict_timing, set_yes_no)
declare_def_snprint(strict_timing, print_yes_no)

declare_def_handler(skip_kpartx, set_yes_no_undef)
declare_def_snprint_defint(skip_kpartx, print_yes_no_undef,
			   DEFAULT_SKIP_KPARTX)
declare_ovr_handler(skip_kpartx, set_yes_no_undef)
declare_ovr_snprint(skip_kpartx, print_yes_no_undef)
declare_hw_handler(skip_kpartx, set_yes_no_undef)
declare_hw_snprint(skip_kpartx, print_yes_no_undef)
declare_mp_handler(skip_kpartx, set_yes_no_undef)
declare_mp_snprint(skip_kpartx, print_yes_no_undef)

declare_def_range_handler(remove_retries, 0, INT_MAX)
declare_def_snprint(remove_retries, print_int)

declare_def_range_handler(max_sectors_kb, 0, INT_MAX)
declare_def_snprint(max_sectors_kb, print_nonzero)
declare_ovr_range_handler(max_sectors_kb, 0, INT_MAX)
declare_ovr_snprint(max_sectors_kb, print_nonzero)
declare_hw_range_handler(max_sectors_kb, 0, INT_MAX)
declare_hw_snprint(max_sectors_kb, print_nonzero)
declare_mp_range_handler(max_sectors_kb, 0, INT_MAX)
declare_mp_snprint(max_sectors_kb, print_nonzero)

declare_def_range_handler(find_multipaths_timeout, INT_MIN, INT_MAX)
declare_def_snprint_defint(find_multipaths_timeout, print_int,
			   DEFAULT_FIND_MULTIPATHS_TIMEOUT)

declare_def_handler(enable_foreign, set_str)
declare_def_snprint_defstr(enable_foreign, print_str,
			   DEFAULT_ENABLE_FOREIGN)

#define declare_def_attr_handler(option, function)			\
static int								\
def_ ## option ## _handler (struct config *conf, vector strvec,		\
			    const char *file, int line_nr)		\
{									\
	return function (strvec, &conf->option, &conf->attribute_flags, \
			 file, line_nr);				\
}

#define declare_def_attr_snprint(option, function)			\
static int								\
snprint_def_ ## option (struct config *conf, struct strbuf *buff,	\
			const void *data)				\
{									\
	return function(buff, conf->option, conf->attribute_flags);	\
}

#define declare_mp_attr_handler(option, function)			\
static int								\
mp_ ## option ## _handler (struct config *conf, vector strvec,		\
			   const char *file, int line_nr)		\
{									\
	struct mpentry * mpe = VECTOR_LAST_SLOT(conf->mptable);		\
	if (!mpe)							\
		return 1;						\
	return function (strvec, &mpe->option, &mpe->attribute_flags,	\
			 file, line_nr);				\
}

#define declare_mp_attr_snprint(option, function)			\
static int								\
snprint_mp_ ## option (struct config *conf, struct strbuf *buff,	\
		       const void * data)				\
{									\
	const struct mpentry * mpe = (const struct mpentry *)data;	\
	return function(buff, mpe->option, mpe->attribute_flags);	\
}

static int
set_mode(vector strvec, void *ptr, int *flags, const char *file, int line_nr)
{
	mode_t mode;
	mode_t *mode_ptr = (mode_t *)ptr;
	char *buff;

	buff = set_value(strvec);

	if (!buff)
		return 1;

	if (sscanf(buff, "%o", &mode) == 1 && mode <= 0777) {
		*flags |= (1 << ATTR_MODE);
		*mode_ptr = mode;
	} else
		condlog(1, "%s line %d, invalid value for mode: \"%s\"",
			file, line_nr, buff);

	free(buff);
	return 0;
}

static int
set_uid(vector strvec, void *ptr, int *flags, const char *file, int line_nr)
{
	uid_t uid;
	uid_t *uid_ptr = (uid_t *)ptr;
	char *buff;
	char passwd_buf[1024];
	struct passwd info, *found;

	buff = set_value(strvec);
	if (!buff)
		return 1;
	if (getpwnam_r(buff, &info, passwd_buf, 1024, &found) == 0 && found) {
		*flags |= (1 << ATTR_UID);
		*uid_ptr = info.pw_uid;
	}
	else if (sscanf(buff, "%u", &uid) == 1){
		*flags |= (1 << ATTR_UID);
		*uid_ptr = uid;
	} else
		condlog(1, "%s line %d, invalid value for uid: \"%s\"",
			file, line_nr, buff);

	free(buff);
	return 0;
}

static int
set_gid(vector strvec, void *ptr, int *flags, const char *file, int line_nr)
{
	gid_t gid;
	gid_t *gid_ptr = (gid_t *)ptr;
	char *buff;
	char passwd_buf[1024];
	struct passwd info, *found;

	buff = set_value(strvec);
	if (!buff)
		return 1;

	if (getpwnam_r(buff, &info, passwd_buf, 1024, &found) == 0 && found) {
		*flags |= (1 << ATTR_GID);
		*gid_ptr = info.pw_gid;
	}
	else if (sscanf(buff, "%u", &gid) == 1){
		*flags |= (1 << ATTR_GID);
		*gid_ptr = gid;
	} else
		condlog(1, "%s line %d, invalid value for gid: \"%s\"",
			file, line_nr, buff);
	free(buff);
	return 0;
}

static int
print_mode(struct strbuf *buff, long v, int flags)
{
	mode_t mode = (mode_t)v;
	if ((flags & (1 << ATTR_MODE)) == 0)
		return 0;
	return print_strbuf(buff, "0%o", mode);
}

static int
print_uid(struct strbuf *buff, long v, int flags)
{
	uid_t uid = (uid_t)v;
	if ((flags & (1 << ATTR_UID)) == 0)
		return 0;
	return print_strbuf(buff, "0%o", uid);
}

static int
print_gid(struct strbuf *buff, long v, int flags)
{
	gid_t gid = (gid_t)v;
	if ((flags & (1 << ATTR_GID)) == 0)
		return 0;
	return print_strbuf(buff, "0%o", gid);
}

declare_def_attr_handler(mode, set_mode)
declare_def_attr_snprint(mode, print_mode)
declare_mp_attr_handler(mode, set_mode)
declare_mp_attr_snprint(mode, print_mode)

declare_def_attr_handler(uid, set_uid)
declare_def_attr_snprint(uid, print_uid)
declare_mp_attr_handler(uid, set_uid)
declare_mp_attr_snprint(uid, print_uid)

declare_def_attr_handler(gid, set_gid)
declare_def_attr_snprint(gid, print_gid)
declare_mp_attr_handler(gid, set_gid)
declare_mp_attr_snprint(gid, print_gid)

static int
set_undef_off_zero(vector strvec, void *ptr, const char *file, int line_nr)
{
	char * buff;
	int *int_ptr = (int *)ptr;

	buff = set_value(strvec);
	if (!buff)
		return 1;

	if (strcmp(buff, "off") == 0)
		*int_ptr = UOZ_OFF;
	else if (strcmp(buff, "0") == 0)
		*int_ptr = UOZ_ZERO;
	else
		do_set_int(strvec, int_ptr, 1, INT_MAX, file, line_nr, buff);

	free(buff);
	return 0;
}

int print_undef_off_zero(struct strbuf *buff, long v)
{
	if (v == UOZ_UNDEF)
		return 0;
	if (v == UOZ_OFF)
		return append_strbuf_str(buff, "off");
	if (v == UOZ_ZERO)
		return append_strbuf_str(buff, "0");
	return print_int(buff, v);
}

declare_def_handler(fast_io_fail, set_undef_off_zero)
declare_def_snprint_defint(fast_io_fail, print_undef_off_zero,
			   DEFAULT_FAST_IO_FAIL)
declare_ovr_handler(fast_io_fail, set_undef_off_zero)
declare_ovr_snprint(fast_io_fail, print_undef_off_zero)
declare_hw_handler(fast_io_fail, set_undef_off_zero)
declare_hw_snprint(fast_io_fail, print_undef_off_zero)
declare_pc_handler(fast_io_fail, set_undef_off_zero)
declare_pc_snprint(fast_io_fail, print_undef_off_zero)

static int
set_dev_loss(vector strvec, void *ptr, const char *file, int line_nr)
{
	char * buff;
	unsigned int *uint_ptr = (unsigned int *)ptr;

	buff = set_value(strvec);
	if (!buff)
		return 1;

	if (!strcmp(buff, "infinity"))
		*uint_ptr = MAX_DEV_LOSS_TMO;
	else if (sscanf(buff, "%u", uint_ptr) != 1)
		condlog(1, "%s line %d, invalid value for dev_loss_tmo: \"%s\"",
			file, line_nr, buff);

	free(buff);
	return 0;
}

int
print_dev_loss(struct strbuf *buff, unsigned long v)
{
	if (v == DEV_LOSS_TMO_UNSET)
		return 0;
	if (v >= MAX_DEV_LOSS_TMO)
		return append_strbuf_quoted(buff, "infinity");
	return print_strbuf(buff, "%lu", v);
}

declare_def_handler(dev_loss, set_dev_loss)
declare_def_snprint(dev_loss, print_dev_loss)
declare_ovr_handler(dev_loss, set_dev_loss)
declare_ovr_snprint(dev_loss, print_dev_loss)
declare_hw_handler(dev_loss, set_dev_loss)
declare_hw_snprint(dev_loss, print_dev_loss)
declare_pc_handler(dev_loss, set_dev_loss)
declare_pc_snprint(dev_loss, print_dev_loss)

declare_def_handler(eh_deadline, set_undef_off_zero)
declare_def_snprint(eh_deadline, print_undef_off_zero)
declare_ovr_handler(eh_deadline, set_undef_off_zero)
declare_ovr_snprint(eh_deadline, print_undef_off_zero)
declare_hw_handler(eh_deadline, set_undef_off_zero)
declare_hw_snprint(eh_deadline, print_undef_off_zero)
declare_pc_handler(eh_deadline, set_undef_off_zero)
declare_pc_snprint(eh_deadline, print_undef_off_zero)

static int
def_max_retries_handler(struct config *conf, vector strvec, const char *file,
			int line_nr)
{
	char * buff;

	buff = set_value(strvec);
	if (!buff)
		return 1;

	if (strcmp(buff, "off") == 0)
		conf->max_retries = MAX_RETRIES_OFF;
	else if (strcmp(buff, "0") == 0)
		conf->max_retries = MAX_RETRIES_ZERO;
	else
		do_set_int(strvec, &conf->max_retries, 1, 5, file, line_nr,
			   buff);

	free(buff);
	return 0;
}

declare_def_snprint(max_retries, print_undef_off_zero)

static int
set_pgpolicy(vector strvec, void *ptr, const char *file, int line_nr)
{
	char * buff;
	int policy;
	int *int_ptr = (int *)ptr;

	buff = set_value(strvec);
	if (!buff)
		return 1;

	policy = get_pgpolicy_id(buff);
	if (policy != IOPOLICY_UNDEF)
		*int_ptr = policy;
	else
		condlog(1, "%s line %d, invalid value for path_grouping_policy: \"%s\"",
			file, line_nr, buff);
	free(buff);

	return 0;
}

int
print_pgpolicy(struct strbuf *buff, long pgpolicy)
{
	if (!pgpolicy)
		return 0;

	return append_strbuf_quoted(buff, get_pgpolicy_name(pgpolicy));
}

declare_def_handler(pgpolicy, set_pgpolicy)
declare_def_snprint_defint(pgpolicy, print_pgpolicy, DEFAULT_PGPOLICY)
declare_ovr_handler(pgpolicy, set_pgpolicy)
declare_ovr_snprint(pgpolicy, print_pgpolicy)
declare_hw_handler(pgpolicy, set_pgpolicy)
declare_hw_snprint(pgpolicy, print_pgpolicy)
declare_mp_handler(pgpolicy, set_pgpolicy)
declare_mp_snprint(pgpolicy, print_pgpolicy)

int
get_sys_max_fds(int *max_fds)
{
	FILE *file;
	int nr_open;
	int ret = 1;

	file = fopen("/proc/sys/fs/nr_open", "r");
	if (!file) {
		fprintf(stderr, "Cannot open /proc/sys/fs/nr_open : %s\n",
			strerror(errno));
		return 1;
	}
	if (fscanf(file, "%d", &nr_open) != 1) {
		fprintf(stderr, "Cannot read max open fds from /proc/sys/fs/nr_open");
		if (ferror(file))
			fprintf(stderr, " : %s\n", strerror(errno));
		else
			fprintf(stderr, "\n");
	} else {
		*max_fds = nr_open;
		ret = 0;
	}
	fclose(file);
	return ret;
}


static int
max_fds_handler(struct config *conf, vector strvec, const char *file,
		int line_nr)
{
	char * buff;
	int max_fds;

	buff = set_value(strvec);

	if (!buff)
		return 1;

	if (get_sys_max_fds(&max_fds) != 0)
		max_fds = 4096;  /* Assume safe limit */
	if (!strcmp(buff, "max"))
		conf->max_fds = max_fds;
	else
		do_set_int(strvec, &conf->max_fds, 0, max_fds, file, line_nr,
			   buff);

	free(buff);

	return 0;
}

static int
snprint_max_fds (struct config *conf, struct strbuf *buff, const void *data)
{
	int r = 0, max_fds;

	if (!conf->max_fds)
		return 0;

	r = get_sys_max_fds(&max_fds);
	if (!r && conf->max_fds >= max_fds)
		return append_strbuf_quoted(buff, "max");
	else
		return print_int(buff, conf->max_fds);
}

static int
set_rr_weight(vector strvec, void *ptr, const char *file, int line_nr)
{
	int *int_ptr = (int *)ptr;
	char * buff;

	buff = set_value(strvec);

	if (!buff)
		return 1;

	if (!strcmp(buff, "priorities"))
		*int_ptr = RR_WEIGHT_PRIO;
	else if (!strcmp(buff, "uniform"))
		*int_ptr = RR_WEIGHT_NONE;
	else
		condlog(1, "%s line %d, invalid value for rr_weight: \"%s\"",
			file, line_nr, buff);
	free(buff);

	return 0;
}

int
print_rr_weight (struct strbuf *buff, long v)
{
	if (!v)
		return 0;
	if (v == RR_WEIGHT_PRIO)
		return append_strbuf_quoted(buff, "priorities");
	if (v == RR_WEIGHT_NONE)
		return append_strbuf_quoted(buff, "uniform");

	return 0;
}

declare_def_handler(rr_weight, set_rr_weight)
declare_def_snprint_defint(rr_weight, print_rr_weight, DEFAULT_RR_WEIGHT)
declare_ovr_handler(rr_weight, set_rr_weight)
declare_ovr_snprint(rr_weight, print_rr_weight)
declare_hw_handler(rr_weight, set_rr_weight)
declare_hw_snprint(rr_weight, print_rr_weight)
declare_mp_handler(rr_weight, set_rr_weight)
declare_mp_snprint(rr_weight, print_rr_weight)

static int
set_pgfailback(vector strvec, void *ptr, const char *file, int line_nr)
{
	int *int_ptr = (int *)ptr;
	char * buff;

	buff = set_value(strvec);
	if (!buff)
		return 1;

	if (strlen(buff) == 6 && !strcmp(buff, "manual"))
		*int_ptr = -FAILBACK_MANUAL;
	else if (strlen(buff) == 9 && !strcmp(buff, "immediate"))
		*int_ptr = -FAILBACK_IMMEDIATE;
	else if (strlen(buff) == 10 && !strcmp(buff, "followover"))
		*int_ptr = -FAILBACK_FOLLOWOVER;
	else
		do_set_int(strvec, ptr, 0, INT_MAX, file, line_nr, buff);

	free(buff);

	return 0;
}

int
print_pgfailback (struct strbuf *buff, long v)
{
	switch(v) {
	case  FAILBACK_UNDEF:
		return 0;
	case -FAILBACK_MANUAL:
		return append_strbuf_quoted(buff, "manual");
	case -FAILBACK_IMMEDIATE:
		return append_strbuf_quoted(buff, "immediate");
	case -FAILBACK_FOLLOWOVER:
		return append_strbuf_quoted(buff, "followover");
	default:
		return print_int(buff, v);
	}
}

declare_def_handler(pgfailback, set_pgfailback)
declare_def_snprint_defint(pgfailback, print_pgfailback, DEFAULT_FAILBACK)
declare_ovr_handler(pgfailback, set_pgfailback)
declare_ovr_snprint(pgfailback, print_pgfailback)
declare_hw_handler(pgfailback, set_pgfailback)
declare_hw_snprint(pgfailback, print_pgfailback)
declare_mp_handler(pgfailback, set_pgfailback)
declare_mp_snprint(pgfailback, print_pgfailback)

static int
no_path_retry_helper(vector strvec, void *ptr, const char *file, int line_nr)
{
	int *int_ptr = (int *)ptr;
	char * buff;

	buff = set_value(strvec);
	if (!buff)
		return 1;

	if (!strcmp(buff, "fail") || !strcmp(buff, "0"))
		*int_ptr = NO_PATH_RETRY_FAIL;
	else if (!strcmp(buff, "queue"))
		*int_ptr = NO_PATH_RETRY_QUEUE;
	else
		do_set_int(strvec, ptr, 1, INT_MAX, file, line_nr, buff);

	free(buff);
	return 0;
}

int
print_no_path_retry(struct strbuf *buff, long v)
{
	switch(v) {
	case NO_PATH_RETRY_UNDEF:
		return 0;
	case NO_PATH_RETRY_FAIL:
		return append_strbuf_quoted(buff, "fail");
	case NO_PATH_RETRY_QUEUE:
		return append_strbuf_quoted(buff, "queue");
	default:
		return print_int(buff, v);
	}
}

declare_def_handler(no_path_retry, no_path_retry_helper)
declare_def_snprint(no_path_retry, print_no_path_retry)
declare_ovr_handler(no_path_retry, no_path_retry_helper)
declare_ovr_snprint(no_path_retry, print_no_path_retry)
declare_hw_handler(no_path_retry, no_path_retry_helper)
declare_hw_snprint(no_path_retry, print_no_path_retry)
declare_mp_handler(no_path_retry, no_path_retry_helper)
declare_mp_snprint(no_path_retry, print_no_path_retry)

static int
def_log_checker_err_handler(struct config *conf, vector strvec,
			    const char *file, int line_nr)
{
	char * buff;

	buff = set_value(strvec);

	if (!buff)
		return 1;

	if (!strcmp(buff, "once"))
		conf->log_checker_err = LOG_CHKR_ERR_ONCE;
	else if (!strcmp(buff, "always"))
		conf->log_checker_err = LOG_CHKR_ERR_ALWAYS;
	else
		condlog(1, "%s line %d, invalid value for log_checker_err: \"%s\"",
			file, line_nr, buff);

	free(buff);
	return 0;
}

static int
snprint_def_log_checker_err(struct config *conf, struct strbuf *buff,
			    const void * data)
{
	if (conf->log_checker_err == LOG_CHKR_ERR_ONCE)
		return append_strbuf_quoted(buff, "once");
	return append_strbuf_quoted(buff, "always");
}

static int
set_reservation_key(vector strvec, struct be64 *be64_ptr, uint8_t *flags_ptr,
		    int *source_ptr)
{
	char *buff;
	uint64_t prkey;
	uint8_t sa_flags;

	buff = set_value(strvec);
	if (!buff)
		return 1;

	if (strcmp(buff, "file") == 0) {
		*source_ptr = PRKEY_SOURCE_FILE;
		*flags_ptr = 0;
		put_be64(*be64_ptr, 0);
		free(buff);
		return 0;
	}

	if (parse_prkey_flags(buff, &prkey, &sa_flags) != 0) {
		free(buff);
		return 1;
	}
	*source_ptr = PRKEY_SOURCE_CONF;
	*flags_ptr = sa_flags;
	put_be64(*be64_ptr, prkey);
	free(buff);
	return 0;
}

static int
def_reservation_key_handler(struct config *conf, vector strvec,
			    const char *file, int line_nr)
{
	return set_reservation_key(strvec, &conf->reservation_key,
				   &conf->sa_flags,
				   &conf->prkey_source);
}

static int
snprint_def_reservation_key (struct config *conf, struct strbuf *buff,
			     const void * data)
{
	return print_reservation_key(buff, conf->reservation_key,
				     conf->sa_flags, conf->prkey_source);
}

static int
mp_reservation_key_handler(struct config *conf, vector strvec, const char *file,
			   int line_nr)
{
	struct mpentry * mpe = VECTOR_LAST_SLOT(conf->mptable);
	if (!mpe)
		return 1;
	return set_reservation_key(strvec, &mpe->reservation_key,
				   &mpe->sa_flags,
				   &mpe->prkey_source);
}

static int
snprint_mp_reservation_key (struct config *conf, struct strbuf *buff,
			    const void *data)
{
	const struct mpentry * mpe = (const struct mpentry *)data;
	return print_reservation_key(buff, mpe->reservation_key,
				     mpe->sa_flags, mpe->prkey_source);
}

static int
set_off_int_undef(vector strvec, void *ptr, const char *file, int line_nr)
{
	int *int_ptr = (int *)ptr;
	char * buff;

	buff = set_value(strvec);
	if (!buff)
		return 1;

	if (!strcmp(buff, "no") || !strcmp(buff, "0"))
		*int_ptr = NU_NO;
	else
		do_set_int(strvec, ptr, 1, INT_MAX, file, line_nr, buff);

	free(buff);
	return 0;
}

int
print_off_int_undef(struct strbuf *buff, long v)
{
	switch(v) {
	case NU_UNDEF:
		return 0;
	case NU_NO:
		return append_strbuf_quoted(buff, "no");
	default:
		return print_int(buff, v);
	}
}

declare_def_handler(delay_watch_checks, set_off_int_undef)
declare_def_snprint_defint(delay_watch_checks, print_off_int_undef,
			   DEFAULT_DELAY_CHECKS)
declare_ovr_handler(delay_watch_checks, set_off_int_undef)
declare_ovr_snprint(delay_watch_checks, print_off_int_undef)
declare_hw_handler(delay_watch_checks, set_off_int_undef)
declare_hw_snprint(delay_watch_checks, print_off_int_undef)
declare_mp_handler(delay_watch_checks, set_off_int_undef)
declare_mp_snprint(delay_watch_checks, print_off_int_undef)
declare_def_handler(delay_wait_checks, set_off_int_undef)
declare_def_snprint_defint(delay_wait_checks, print_off_int_undef,
			   DEFAULT_DELAY_CHECKS)
declare_ovr_handler(delay_wait_checks, set_off_int_undef)
declare_ovr_snprint(delay_wait_checks, print_off_int_undef)
declare_hw_handler(delay_wait_checks, set_off_int_undef)
declare_hw_snprint(delay_wait_checks, print_off_int_undef)
declare_mp_handler(delay_wait_checks, set_off_int_undef)
declare_mp_snprint(delay_wait_checks, print_off_int_undef)
declare_def_handler(san_path_err_threshold, set_off_int_undef)
declare_def_snprint_defint(san_path_err_threshold, print_off_int_undef,
			   DEFAULT_ERR_CHECKS)
declare_ovr_handler(san_path_err_threshold, set_off_int_undef)
declare_ovr_snprint(san_path_err_threshold, print_off_int_undef)
declare_hw_handler(san_path_err_threshold, set_off_int_undef)
declare_hw_snprint(san_path_err_threshold, print_off_int_undef)
declare_mp_handler(san_path_err_threshold, set_off_int_undef)
declare_mp_snprint(san_path_err_threshold, print_off_int_undef)
declare_def_handler(san_path_err_forget_rate, set_off_int_undef)
declare_def_snprint_defint(san_path_err_forget_rate, print_off_int_undef,
			   DEFAULT_ERR_CHECKS)
declare_ovr_handler(san_path_err_forget_rate, set_off_int_undef)
declare_ovr_snprint(san_path_err_forget_rate, print_off_int_undef)
declare_hw_handler(san_path_err_forget_rate, set_off_int_undef)
declare_hw_snprint(san_path_err_forget_rate, print_off_int_undef)
declare_mp_handler(san_path_err_forget_rate, set_off_int_undef)
declare_mp_snprint(san_path_err_forget_rate, print_off_int_undef)
declare_def_handler(san_path_err_recovery_time, set_off_int_undef)
declare_def_snprint_defint(san_path_err_recovery_time, print_off_int_undef,
			   DEFAULT_ERR_CHECKS)
declare_ovr_handler(san_path_err_recovery_time, set_off_int_undef)
declare_ovr_snprint(san_path_err_recovery_time, print_off_int_undef)
declare_hw_handler(san_path_err_recovery_time, set_off_int_undef)
declare_hw_snprint(san_path_err_recovery_time, print_off_int_undef)
declare_mp_handler(san_path_err_recovery_time, set_off_int_undef)
declare_mp_snprint(san_path_err_recovery_time, print_off_int_undef)
declare_def_handler(marginal_path_err_sample_time, set_off_int_undef)
declare_def_snprint_defint(marginal_path_err_sample_time, print_off_int_undef,
			   DEFAULT_ERR_CHECKS)
declare_ovr_handler(marginal_path_err_sample_time, set_off_int_undef)
declare_ovr_snprint(marginal_path_err_sample_time, print_off_int_undef)
declare_hw_handler(marginal_path_err_sample_time, set_off_int_undef)
declare_hw_snprint(marginal_path_err_sample_time, print_off_int_undef)
declare_mp_handler(marginal_path_err_sample_time, set_off_int_undef)
declare_mp_snprint(marginal_path_err_sample_time, print_off_int_undef)
declare_def_handler(marginal_path_err_rate_threshold, set_off_int_undef)
declare_def_snprint_defint(marginal_path_err_rate_threshold, print_off_int_undef,
			   DEFAULT_ERR_CHECKS)
declare_ovr_handler(marginal_path_err_rate_threshold, set_off_int_undef)
declare_ovr_snprint(marginal_path_err_rate_threshold, print_off_int_undef)
declare_hw_handler(marginal_path_err_rate_threshold, set_off_int_undef)
declare_hw_snprint(marginal_path_err_rate_threshold, print_off_int_undef)
declare_mp_handler(marginal_path_err_rate_threshold, set_off_int_undef)
declare_mp_snprint(marginal_path_err_rate_threshold, print_off_int_undef)
declare_def_handler(marginal_path_err_recheck_gap_time, set_off_int_undef)
declare_def_snprint_defint(marginal_path_err_recheck_gap_time, print_off_int_undef,
			   DEFAULT_ERR_CHECKS)
declare_ovr_handler(marginal_path_err_recheck_gap_time, set_off_int_undef)
declare_ovr_snprint(marginal_path_err_recheck_gap_time, print_off_int_undef)
declare_hw_handler(marginal_path_err_recheck_gap_time, set_off_int_undef)
declare_hw_snprint(marginal_path_err_recheck_gap_time, print_off_int_undef)
declare_mp_handler(marginal_path_err_recheck_gap_time, set_off_int_undef)
declare_mp_snprint(marginal_path_err_recheck_gap_time, print_off_int_undef)
declare_def_handler(marginal_path_double_failed_time, set_off_int_undef)
declare_def_snprint_defint(marginal_path_double_failed_time, print_off_int_undef,
			   DEFAULT_ERR_CHECKS)
declare_ovr_handler(marginal_path_double_failed_time, set_off_int_undef)
declare_ovr_snprint(marginal_path_double_failed_time, print_off_int_undef)
declare_hw_handler(marginal_path_double_failed_time, set_off_int_undef)
declare_hw_snprint(marginal_path_double_failed_time, print_off_int_undef)
declare_mp_handler(marginal_path_double_failed_time, set_off_int_undef)
declare_mp_snprint(marginal_path_double_failed_time, print_off_int_undef)

declare_def_handler(ghost_delay, set_off_int_undef)
declare_def_snprint(ghost_delay, print_off_int_undef)
declare_ovr_handler(ghost_delay, set_off_int_undef)
declare_ovr_snprint(ghost_delay, print_off_int_undef)
declare_hw_handler(ghost_delay, set_off_int_undef)
declare_hw_snprint(ghost_delay, print_off_int_undef)
declare_mp_handler(ghost_delay, set_off_int_undef)
declare_mp_snprint(ghost_delay, print_off_int_undef)

declare_def_handler(all_tg_pt, set_yes_no_undef)
declare_def_snprint_defint(all_tg_pt, print_yes_no_undef, DEFAULT_ALL_TG_PT)
declare_ovr_handler(all_tg_pt, set_yes_no_undef)
declare_ovr_snprint(all_tg_pt, print_yes_no_undef)
declare_hw_handler(all_tg_pt, set_yes_no_undef)
declare_hw_snprint(all_tg_pt, print_yes_no_undef)

declare_def_handler(recheck_wwid, set_yes_no_undef)
declare_def_snprint_defint(recheck_wwid, print_yes_no_undef, DEFAULT_RECHECK_WWID)
declare_ovr_handler(recheck_wwid, set_yes_no_undef)
declare_ovr_snprint(recheck_wwid, print_yes_no_undef)
declare_hw_handler(recheck_wwid, set_yes_no_undef)
declare_hw_snprint(recheck_wwid, print_yes_no_undef)

declare_def_range_handler(uxsock_timeout, DEFAULT_REPLY_TIMEOUT, INT_MAX)

static int
def_auto_resize_handler(struct config *conf, vector strvec, const char *file,
			int line_nr)
{
	char * buff;

	buff = set_value(strvec);
	if (!buff)
		return 1;

	if (strcmp(buff, "never") == 0)
		conf->auto_resize = AUTO_RESIZE_NEVER;
	else if (strcmp(buff, "grow_only") == 0)
		conf->auto_resize = AUTO_RESIZE_GROW_ONLY;
	else if (strcmp(buff, "grow_shrink") == 0)
		conf->auto_resize = AUTO_RESIZE_GROW_SHRINK;
	else
		condlog(1, "%s line %d, invalid value for auto_resize: \"%s\"",
			file, line_nr, buff);

	free(buff);
	return 0;
}

int
print_auto_resize(struct strbuf *buff, long v)
{
	return append_strbuf_quoted(buff,
			v == AUTO_RESIZE_GROW_ONLY ? "grow_only" :
			v == AUTO_RESIZE_GROW_SHRINK ? "grow_shrink" :
			"never");
}

declare_def_snprint(auto_resize, print_auto_resize)

static int
hw_vpd_vendor_handler(struct config *conf, vector strvec, const char *file,
		      int line_nr)
{
	int i;
	char *buff;

	struct hwentry * hwe = VECTOR_LAST_SLOT(conf->hwtable);
	if (!hwe)
		return 1;

	buff = set_value(strvec);
	if (!buff)
		return 1;
	for (i = 0; i < VPD_VP_ARRAY_SIZE; i++) {
		if (strcmp(buff, vpd_vendor_pages[i].name) == 0) {
			hwe->vpd_vendor_id = i;
			goto out;
		}
	}
	condlog(1, "%s line %d, invalid value for vpd_vendor: \"%s\"",
		file, line_nr, buff);
out:
	free(buff);
	return 0;
}

static int
snprint_hw_vpd_vendor(struct config *conf, struct strbuf *buff,
		      const void * data)
{
	const struct hwentry * hwe = (const struct hwentry *)data;

	if (hwe->vpd_vendor_id > 0 && hwe->vpd_vendor_id < VPD_VP_ARRAY_SIZE)
		return append_strbuf_quoted(buff,
				vpd_vendor_pages[hwe->vpd_vendor_id].name);
	return 0;
}

/*
 * blacklist block handlers
 */
static int
blacklist_handler(struct config *conf, vector strvec, const char*file,
		  int line_nr)
{
	if (!conf->blist_devnode)
		conf->blist_devnode = vector_alloc();
	if (!conf->blist_wwid)
		conf->blist_wwid = vector_alloc();
	if (!conf->blist_device)
		conf->blist_device = vector_alloc();
	if (!conf->blist_property)
		conf->blist_property = vector_alloc();
	if (!conf->blist_protocol)
		conf->blist_protocol = vector_alloc();

	if (!conf->blist_devnode || !conf->blist_wwid ||
	    !conf->blist_device || !conf->blist_property ||
	    !conf->blist_protocol)
		return 1;

	return 0;
}

static int
blacklist_exceptions_handler(struct config *conf, vector strvec,
			     const char *file, int line_nr)
{
	if (!conf->elist_devnode)
		conf->elist_devnode = vector_alloc();
	if (!conf->elist_wwid)
		conf->elist_wwid = vector_alloc();
	if (!conf->elist_device)
		conf->elist_device = vector_alloc();
	if (!conf->elist_property)
		conf->elist_property = vector_alloc();
	if (!conf->elist_protocol)
		conf->elist_protocol = vector_alloc();

	if (!conf->elist_devnode || !conf->elist_wwid ||
	    !conf->elist_device || !conf->elist_property ||
	    !conf->elist_protocol)
		return 1;

	return 0;
}

#define declare_ble_handler(option)					\
static int								\
ble_ ## option ## _handler (struct config *conf, vector strvec,		\
			    const char *file, int line_nr)		\
{									\
	char *buff;							\
	int rc;								\
									\
	if (!conf->option)						\
		return 1;						\
									\
	buff = set_value(strvec);					\
	if (!buff)							\
		return 1;						\
									\
	rc = store_ble(conf->option, buff, ORIGIN_CONFIG);		\
	free(buff);							\
	return rc;							\
}

#define declare_ble_device_handler(name, option, vend, prod)		\
static int								\
ble_ ## option ## _ ## name ## _handler (struct config *conf, vector strvec, \
					 const char *file, int line_nr)	\
{									\
	char * buff;							\
	int rc;								\
									\
	if (!conf->option)						\
		return 1;						\
									\
	buff = set_value(strvec);					\
	if (!buff)							\
		return 1;						\
									\
	rc = set_ble_device(conf->option, vend, prod, ORIGIN_CONFIG);	\
	free(buff);							\
	return rc;							\
}

declare_ble_handler(blist_devnode)
declare_ble_handler(elist_devnode)
declare_ble_handler(blist_wwid)
declare_ble_handler(elist_wwid)
declare_ble_handler(blist_property)
declare_ble_handler(elist_property)
declare_ble_handler(blist_protocol)
declare_ble_handler(elist_protocol)

static int
snprint_def_uxsock_timeout(struct config *conf, struct strbuf *buff,
			   const void *data)
{
	return print_strbuf(buff, "%u", conf->uxsock_timeout);
}

static int
snprint_ble_simple (struct config *conf, struct strbuf *buff, const void *data)
{
	const struct blentry *ble = (const struct blentry *)data;

	return print_str(buff, ble->str);
}

static int
ble_device_handler(struct config *conf, vector strvec, const char *file,
		   int line_nr)
{
	return alloc_ble_device(conf->blist_device);
}

static int
ble_except_device_handler(struct config *conf, vector strvec, const char *file,
			  int line_nr)
{
	return alloc_ble_device(conf->elist_device);
}

declare_ble_device_handler(vendor, blist_device, buff, NULL)
declare_ble_device_handler(vendor, elist_device, buff, NULL)
declare_ble_device_handler(product, blist_device, NULL, buff)
declare_ble_device_handler(product, elist_device, NULL, buff)

static int snprint_bled_vendor(struct config *conf, struct strbuf *buff,
			       const void * data)
{
	const struct blentry_device * bled =
		(const struct blentry_device *)data;

	return print_str(buff, bled->vendor);
}

static int snprint_bled_product(struct config *conf, struct strbuf *buff,
				const void *data)
{
	const struct blentry_device * bled =
		(const struct blentry_device *)data;

	return print_str(buff, bled->product);
}

/*
 * devices block handlers
 */
static int
devices_handler(struct config *conf, vector strvec, const char *file,
		int line_nr)
{
	if (!conf->hwtable)
		conf->hwtable = vector_alloc();

	if (!conf->hwtable)
		return 1;

	return 0;
}

static int
device_handler(struct config *conf, vector strvec, const char *file,
	       int line_nr)
{
	struct hwentry * hwe;

	hwe = alloc_hwe();

	if (!hwe)
		return 1;

	if (!vector_alloc_slot(conf->hwtable)) {
		free_hwe(hwe);
		return 1;
	}
	vector_set_slot(conf->hwtable, hwe);

	return 0;
}

declare_hw_handler(vendor, set_str)
declare_hw_snprint(vendor, print_str)

declare_hw_handler(product, set_str)
declare_hw_snprint(product, print_str)

declare_hw_handler(revision, set_str)
declare_hw_snprint(revision, print_str)

declare_hw_handler(bl_product, set_str)
declare_hw_snprint(bl_product, print_str)

declare_hw_arg_str_handler(hwhandler, 0)
declare_hw_snprint(hwhandler, print_str)

/*
 * overrides handlers
 */
static int
overrides_handler(struct config *conf, vector strvec, const char *file,
		  int line_nr)
{
	if (!conf->overrides)
		conf->overrides = alloc_hwe();

	if (!conf->overrides)
		return 1;

	return 0;
}



/*
 * multipaths block handlers
 */
static int
multipaths_handler(struct config *conf, vector strvec, const char *file,
		   int line_nr)
{
	if (!conf->mptable)
		conf->mptable = vector_alloc();

	if (!conf->mptable)
		return 1;

	return 0;
}

static int
multipath_handler(struct config *conf, vector strvec, const char *file,
		  int line_nr)
{
	struct mpentry * mpe;

	mpe = alloc_mpe();

	if (!mpe)
		return 1;

	if (!vector_alloc_slot(conf->mptable)) {
		free_mpe(mpe);
		return 1;
	}
	vector_set_slot(conf->mptable, mpe);

	return 0;
}

declare_mp_handler(wwid, set_str)
declare_mp_snprint(wwid, print_str)

declare_mp_handler(alias, set_str_noslash)
declare_mp_snprint(alias, print_str)


static int
protocol_handler(struct config *conf, vector strvec, const char *file,
               int line_nr)
{
	struct pcentry *pce;

	if (!conf->overrides)
		return 1;

	if (!conf->overrides->pctable &&
	    !(conf->overrides->pctable = vector_alloc()))
		return 1;

	if (!(pce = alloc_pce()))
		return 1;

	if (!vector_alloc_slot(conf->overrides->pctable)) {
		free(pce);
		return 1;
	}
	vector_set_slot(conf->overrides->pctable, pce);

	return 0;
}

static int
set_protocol_type(vector strvec, void *ptr, const char *file, int line_nr)
{
	int *int_ptr = (int *)ptr;
	char *buff;
	int i;

	buff = set_value(strvec);

	if (!buff)
		return 1;

	for (i = 0; i <= LAST_BUS_PROTOCOL_ID; i++) {
		if (protocol_name[i] && !strcmp(buff, protocol_name[i])) {
			*int_ptr = i;
			break;
		}
	}
	if (i > LAST_BUS_PROTOCOL_ID)
		condlog(1, "%s line %d, invalid value for type: \"%s\"",
			file, line_nr, buff);

	free(buff);
	return 0;
}

static int
print_protocol_type(struct strbuf *buff, int type)
{
	if (type < 0)
		return 0;
	return append_strbuf_quoted(buff, protocol_name[type]);
}

declare_pc_handler(type, set_protocol_type)
declare_pc_snprint(type, print_protocol_type)

/*
 * deprecated handlers
 */

static int
deprecated_handler(struct config *conf, vector strvec, const char *file,
		   int line_nr)
{
	char * buff;

	buff = set_value(strvec);

	if (!buff)
		return 1;

	free(buff);
	return 0;
}

static int
snprint_deprecated (struct config *conf, struct strbuf *buff, const void * data)
{
	return 0;
}

// Deprecated keywords
declare_deprecated_handler(config_dir, CONFIG_DIR)
declare_deprecated_handler(disable_changed_wwids, "yes")
declare_deprecated_handler(getuid_callout, "(not set)")
declare_deprecated_handler(multipath_dir, MULTIPATH_DIR)
declare_deprecated_handler(pg_timeout, "(not set)")
declare_deprecated_handler(bindings_file, DEFAULT_BINDINGS_FILE)
declare_deprecated_handler(wwids_file, DEFAULT_WWIDS_FILE)
declare_deprecated_handler(prkeys_file, DEFAULT_PRKEYS_FILE)

/*
 * If you add or remove a keyword also update multipath/multipath.conf.5
 */
void
init_keywords(vector keywords)
{
	install_keyword_root("defaults", NULL);
	install_keyword("verbosity", &def_verbosity_handler, &snprint_def_verbosity);
	install_keyword("polling_interval", &checkint_handler, &snprint_def_checkint);
	install_keyword("max_polling_interval", &def_max_checkint_handler, &snprint_def_max_checkint);
	install_keyword("reassign_maps", &def_reassign_maps_handler, &snprint_def_reassign_maps);
	install_keyword("multipath_dir", &deprecated_multipath_dir_handler, &snprint_deprecated);
	install_keyword("path_selector", &def_selector_handler, &snprint_def_selector);
	install_keyword("path_grouping_policy", &def_pgpolicy_handler, &snprint_def_pgpolicy);
	install_keyword("uid_attrs", &uid_attrs_handler, &snprint_uid_attrs);
	install_keyword("uid_attribute", &def_uid_attribute_handler, &snprint_def_uid_attribute);
	install_keyword("getuid_callout", &deprecated_getuid_callout_handler, &snprint_deprecated);
	install_keyword("prio", &def_prio_name_handler, &snprint_def_prio_name);
	install_keyword("prio_args", &def_prio_args_handler, &snprint_def_prio_args);
	install_keyword("features", &def_features_handler, &snprint_def_features);
	install_keyword("path_checker", &def_checker_name_handler, &snprint_def_checker_name);
	install_keyword("checker", &def_checker_name_handler, NULL);
	install_keyword("alias_prefix", &def_alias_prefix_handler, &snprint_def_alias_prefix);
	install_keyword("failback", &def_pgfailback_handler, &snprint_def_pgfailback);
	install_keyword("rr_min_io", &def_minio_handler, &snprint_def_minio);
	install_keyword("rr_min_io_rq", &def_minio_rq_handler, &snprint_def_minio_rq);
	install_keyword("max_fds", &max_fds_handler, &snprint_max_fds);
	install_keyword("rr_weight", &def_rr_weight_handler, &snprint_def_rr_weight);
	install_keyword("no_path_retry", &def_no_path_retry_handler, &snprint_def_no_path_retry);
	install_keyword("queue_without_daemon", &def_queue_without_daemon_handler, &snprint_def_queue_without_daemon);
	install_keyword("checker_timeout", &def_checker_timeout_handler, &snprint_def_checker_timeout);
	install_keyword("allow_usb_devices", &def_allow_usb_devices_handler, &snprint_def_allow_usb_devices);
	install_keyword("pg_timeout", &deprecated_pg_timeout_handler, &snprint_deprecated);
	install_keyword("flush_on_last_del", &def_flush_on_last_del_handler, &snprint_def_flush_on_last_del);
	install_keyword("user_friendly_names", &def_user_friendly_names_handler, &snprint_def_user_friendly_names);
	install_keyword("mode", &def_mode_handler, &snprint_def_mode);
	install_keyword("uid", &def_uid_handler, &snprint_def_uid);
	install_keyword("gid", &def_gid_handler, &snprint_def_gid);
	install_keyword("fast_io_fail_tmo", &def_fast_io_fail_handler, &snprint_def_fast_io_fail);
	install_keyword("dev_loss_tmo", &def_dev_loss_handler, &snprint_def_dev_loss);
	install_keyword("eh_deadline", &def_eh_deadline_handler, &snprint_def_eh_deadline);
	install_keyword("max_retries", &def_max_retries_handler, &snprint_def_max_retries);
	install_keyword("bindings_file", &deprecated_bindings_file_handler, &snprint_deprecated);
	install_keyword("wwids_file", &deprecated_wwids_file_handler, &snprint_deprecated);
	install_keyword("prkeys_file", &deprecated_prkeys_file_handler, &snprint_deprecated);
	install_keyword("log_checker_err", &def_log_checker_err_handler, &snprint_def_log_checker_err);
	install_keyword("reservation_key", &def_reservation_key_handler, &snprint_def_reservation_key);
	install_keyword("all_tg_pt", &def_all_tg_pt_handler, &snprint_def_all_tg_pt);
	install_keyword("retain_attached_hw_handler", &def_retain_hwhandler_handler, &snprint_def_retain_hwhandler);
	install_keyword("detect_prio", &def_detect_prio_handler, &snprint_def_detect_prio);
	install_keyword("detect_checker", &def_detect_checker_handler, &snprint_def_detect_checker);
	install_keyword("detect_pgpolicy", &def_detect_pgpolicy_handler, &snprint_def_detect_pgpolicy);
	install_keyword("detect_pgpolicy_use_tpg", &def_detect_pgpolicy_use_tpg_handler, &snprint_def_detect_pgpolicy_use_tpg);
	install_keyword("force_sync", &def_force_sync_handler, &snprint_def_force_sync);
	install_keyword("strict_timing", &def_strict_timing_handler, &snprint_def_strict_timing);
	install_keyword("deferred_remove", &def_deferred_remove_handler, &snprint_def_deferred_remove);
	install_keyword("partition_delimiter", &def_partition_delim_handler, &snprint_def_partition_delim);
	install_keyword("config_dir", &deprecated_config_dir_handler, &snprint_deprecated);
	install_keyword("delay_watch_checks", &def_delay_watch_checks_handler, &snprint_def_delay_watch_checks);
	install_keyword("delay_wait_checks", &def_delay_wait_checks_handler, &snprint_def_delay_wait_checks);
	install_keyword("san_path_err_threshold", &def_san_path_err_threshold_handler, &snprint_def_san_path_err_threshold);
	install_keyword("san_path_err_forget_rate", &def_san_path_err_forget_rate_handler, &snprint_def_san_path_err_forget_rate);
	install_keyword("san_path_err_recovery_time", &def_san_path_err_recovery_time_handler, &snprint_def_san_path_err_recovery_time);
	install_keyword("marginal_path_err_sample_time", &def_marginal_path_err_sample_time_handler, &snprint_def_marginal_path_err_sample_time);
	install_keyword("marginal_path_err_rate_threshold", &def_marginal_path_err_rate_threshold_handler, &snprint_def_marginal_path_err_rate_threshold);
	install_keyword("marginal_path_err_recheck_gap_time", &def_marginal_path_err_recheck_gap_time_handler, &snprint_def_marginal_path_err_recheck_gap_time);
	install_keyword("marginal_path_double_failed_time", &def_marginal_path_double_failed_time_handler, &snprint_def_marginal_path_double_failed_time);

	install_keyword("find_multipaths", &def_find_multipaths_handler, &snprint_def_find_multipaths);
	install_keyword("uxsock_timeout", &def_uxsock_timeout_handler, &snprint_def_uxsock_timeout);
	install_keyword("retrigger_tries", &def_retrigger_tries_handler, &snprint_def_retrigger_tries);
	install_keyword("retrigger_delay", &def_retrigger_delay_handler, &snprint_def_retrigger_delay);
	install_keyword("missing_uev_wait_timeout", &def_uev_wait_timeout_handler, &snprint_def_uev_wait_timeout);
	install_keyword("skip_kpartx", &def_skip_kpartx_handler, &snprint_def_skip_kpartx);
	install_keyword("disable_changed_wwids", &deprecated_disable_changed_wwids_handler, &snprint_deprecated);
	install_keyword("remove_retries", &def_remove_retries_handler, &snprint_def_remove_retries);
	install_keyword("max_sectors_kb", &def_max_sectors_kb_handler, &snprint_def_max_sectors_kb);
	install_keyword("ghost_delay", &def_ghost_delay_handler, &snprint_def_ghost_delay);
	install_keyword("auto_resize", &def_auto_resize_handler, &snprint_def_auto_resize);
	install_keyword("find_multipaths_timeout",
			&def_find_multipaths_timeout_handler,
			&snprint_def_find_multipaths_timeout);
	install_keyword("enable_foreign", &def_enable_foreign_handler,
			&snprint_def_enable_foreign);
	install_keyword("marginal_pathgroups", &def_marginal_pathgroups_handler, &snprint_def_marginal_pathgroups);
	install_keyword("recheck_wwid", &def_recheck_wwid_handler, &snprint_def_recheck_wwid);

	install_keyword_root("blacklist", &blacklist_handler);
	install_keyword_multi("devnode", &ble_blist_devnode_handler, &snprint_ble_simple);
	install_keyword_multi("wwid", &ble_blist_wwid_handler, &snprint_ble_simple);
	install_keyword_multi("property", &ble_blist_property_handler, &snprint_ble_simple);
	install_keyword_multi("protocol", &ble_blist_protocol_handler, &snprint_ble_simple);
	install_keyword_multi("device", &ble_device_handler, NULL);
	install_sublevel();
	install_keyword("vendor", &ble_blist_device_vendor_handler, &snprint_bled_vendor);
	install_keyword("product", &ble_blist_device_product_handler, &snprint_bled_product);
	install_sublevel_end();
	install_keyword_root("blacklist_exceptions", &blacklist_exceptions_handler);
	install_keyword_multi("devnode", &ble_elist_devnode_handler, &snprint_ble_simple);
	install_keyword_multi("wwid", &ble_elist_wwid_handler, &snprint_ble_simple);
	install_keyword_multi("property", &ble_elist_property_handler, &snprint_ble_simple);
	install_keyword_multi("protocol", &ble_elist_protocol_handler, &snprint_ble_simple);
	install_keyword_multi("device", &ble_except_device_handler, NULL);
	install_sublevel();
	install_keyword("vendor", &ble_elist_device_vendor_handler, &snprint_bled_vendor);
	install_keyword("product", &ble_elist_device_product_handler, &snprint_bled_product);
	install_sublevel_end();

/*
 * If you add or remove a "device subsection" keyword also update
 * multipath/multipath.conf.5 and the TEMPLATE in libmultipath/hwtable.c
 */
	install_keyword_root("devices", &devices_handler);
	install_keyword_multi("device", &device_handler, NULL);
	install_sublevel();
	install_keyword("vendor", &hw_vendor_handler, &snprint_hw_vendor);
	install_keyword("product", &hw_product_handler, &snprint_hw_product);
	install_keyword("revision", &hw_revision_handler, &snprint_hw_revision);
	install_keyword("product_blacklist", &hw_bl_product_handler, &snprint_hw_bl_product);
	install_keyword("path_grouping_policy", &hw_pgpolicy_handler, &snprint_hw_pgpolicy);
	install_keyword("uid_attribute", &hw_uid_attribute_handler, &snprint_hw_uid_attribute);
	install_keyword("getuid_callout", &deprecated_getuid_callout_handler, &snprint_deprecated);
	install_keyword("path_selector", &hw_selector_handler, &snprint_hw_selector);
	install_keyword("path_checker", &hw_checker_name_handler, &snprint_hw_checker_name);
	install_keyword("checker", &hw_checker_name_handler, NULL);
	install_keyword("alias_prefix", &hw_alias_prefix_handler, &snprint_hw_alias_prefix);
	install_keyword("features", &hw_features_handler, &snprint_hw_features);
	install_keyword("hardware_handler", &hw_hwhandler_handler, &snprint_hw_hwhandler);
	install_keyword("prio", &hw_prio_name_handler, &snprint_hw_prio_name);
	install_keyword("prio_args", &hw_prio_args_handler, &snprint_hw_prio_args);
	install_keyword("failback", &hw_pgfailback_handler, &snprint_hw_pgfailback);
	install_keyword("rr_weight", &hw_rr_weight_handler, &snprint_hw_rr_weight);
	install_keyword("no_path_retry", &hw_no_path_retry_handler, &snprint_hw_no_path_retry);
	install_keyword("rr_min_io", &hw_minio_handler, &snprint_hw_minio);
	install_keyword("rr_min_io_rq", &hw_minio_rq_handler, &snprint_hw_minio_rq);
	install_keyword("pg_timeout", &deprecated_pg_timeout_handler, &snprint_deprecated);
	install_keyword("flush_on_last_del", &hw_flush_on_last_del_handler, &snprint_hw_flush_on_last_del);
	install_keyword("fast_io_fail_tmo", &hw_fast_io_fail_handler, &snprint_hw_fast_io_fail);
	install_keyword("dev_loss_tmo", &hw_dev_loss_handler, &snprint_hw_dev_loss);
	install_keyword("eh_deadline", &hw_eh_deadline_handler, &snprint_hw_eh_deadline);
	install_keyword("user_friendly_names", &hw_user_friendly_names_handler, &snprint_hw_user_friendly_names);
	install_keyword("retain_attached_hw_handler", &hw_retain_hwhandler_handler, &snprint_hw_retain_hwhandler);
	install_keyword("detect_prio", &hw_detect_prio_handler, &snprint_hw_detect_prio);
	install_keyword("detect_checker", &hw_detect_checker_handler, &snprint_hw_detect_checker);
	install_keyword("detect_pgpolicy", &hw_detect_pgpolicy_handler, &snprint_hw_detect_pgpolicy);
	install_keyword("detect_pgpolicy_use_tpg", &hw_detect_pgpolicy_use_tpg_handler, &snprint_hw_detect_pgpolicy_use_tpg);
	install_keyword("deferred_remove", &hw_deferred_remove_handler, &snprint_hw_deferred_remove);
	install_keyword("delay_watch_checks", &hw_delay_watch_checks_handler, &snprint_hw_delay_watch_checks);
	install_keyword("delay_wait_checks", &hw_delay_wait_checks_handler, &snprint_hw_delay_wait_checks);
	install_keyword("san_path_err_threshold", &hw_san_path_err_threshold_handler, &snprint_hw_san_path_err_threshold);
	install_keyword("san_path_err_forget_rate", &hw_san_path_err_forget_rate_handler, &snprint_hw_san_path_err_forget_rate);
	install_keyword("san_path_err_recovery_time", &hw_san_path_err_recovery_time_handler, &snprint_hw_san_path_err_recovery_time);
	install_keyword("marginal_path_err_sample_time", &hw_marginal_path_err_sample_time_handler, &snprint_hw_marginal_path_err_sample_time);
	install_keyword("marginal_path_err_rate_threshold", &hw_marginal_path_err_rate_threshold_handler, &snprint_hw_marginal_path_err_rate_threshold);
	install_keyword("marginal_path_err_recheck_gap_time", &hw_marginal_path_err_recheck_gap_time_handler, &snprint_hw_marginal_path_err_recheck_gap_time);
	install_keyword("marginal_path_double_failed_time", &hw_marginal_path_double_failed_time_handler, &snprint_hw_marginal_path_double_failed_time);
	install_keyword("skip_kpartx", &hw_skip_kpartx_handler, &snprint_hw_skip_kpartx);
	install_keyword("max_sectors_kb", &hw_max_sectors_kb_handler, &snprint_hw_max_sectors_kb);
	install_keyword("ghost_delay", &hw_ghost_delay_handler, &snprint_hw_ghost_delay);
	install_keyword("all_tg_pt", &hw_all_tg_pt_handler, &snprint_hw_all_tg_pt);
	install_keyword("vpd_vendor", &hw_vpd_vendor_handler, &snprint_hw_vpd_vendor);
	install_keyword("recheck_wwid", &hw_recheck_wwid_handler, &snprint_hw_recheck_wwid);
	install_sublevel_end();

	install_keyword_root("overrides", &overrides_handler);
	install_keyword("path_grouping_policy", &ovr_pgpolicy_handler, &snprint_ovr_pgpolicy);
	install_keyword("uid_attribute", &ovr_uid_attribute_handler, &snprint_ovr_uid_attribute);
	install_keyword("getuid_callout", &deprecated_getuid_callout_handler, &snprint_deprecated);
	install_keyword("path_selector", &ovr_selector_handler, &snprint_ovr_selector);
	install_keyword("path_checker", &ovr_checker_name_handler, &snprint_ovr_checker_name);
	install_keyword("checker", &ovr_checker_name_handler, NULL);
	install_keyword("alias_prefix", &ovr_alias_prefix_handler, &snprint_ovr_alias_prefix);
	install_keyword("features", &ovr_features_handler, &snprint_ovr_features);
	install_keyword("prio", &ovr_prio_name_handler, &snprint_ovr_prio_name);
	install_keyword("prio_args", &ovr_prio_args_handler, &snprint_ovr_prio_args);
	install_keyword("failback", &ovr_pgfailback_handler, &snprint_ovr_pgfailback);
	install_keyword("rr_weight", &ovr_rr_weight_handler, &snprint_ovr_rr_weight);
	install_keyword("no_path_retry", &ovr_no_path_retry_handler, &snprint_ovr_no_path_retry);
	install_keyword("rr_min_io", &ovr_minio_handler, &snprint_ovr_minio);
	install_keyword("rr_min_io_rq", &ovr_minio_rq_handler, &snprint_ovr_minio_rq);
	install_keyword("flush_on_last_del", &ovr_flush_on_last_del_handler, &snprint_ovr_flush_on_last_del);
	install_keyword("fast_io_fail_tmo", &ovr_fast_io_fail_handler, &snprint_ovr_fast_io_fail);
	install_keyword("dev_loss_tmo", &ovr_dev_loss_handler, &snprint_ovr_dev_loss);
	install_keyword("eh_deadline", &ovr_eh_deadline_handler, &snprint_ovr_eh_deadline);
	install_keyword("user_friendly_names", &ovr_user_friendly_names_handler, &snprint_ovr_user_friendly_names);
	install_keyword("retain_attached_hw_handler", &ovr_retain_hwhandler_handler, &snprint_ovr_retain_hwhandler);
	install_keyword("detect_prio", &ovr_detect_prio_handler, &snprint_ovr_detect_prio);
	install_keyword("detect_checker", &ovr_detect_checker_handler, &snprint_ovr_detect_checker);
	install_keyword("detect_pgpolicy", &ovr_detect_pgpolicy_handler, &snprint_ovr_detect_pgpolicy);
	install_keyword("detect_pgpolicy_use_tpg", &ovr_detect_pgpolicy_use_tpg_handler, &snprint_ovr_detect_pgpolicy_use_tpg);
	install_keyword("deferred_remove", &ovr_deferred_remove_handler, &snprint_ovr_deferred_remove);
	install_keyword("delay_watch_checks", &ovr_delay_watch_checks_handler, &snprint_ovr_delay_watch_checks);
	install_keyword("delay_wait_checks", &ovr_delay_wait_checks_handler, &snprint_ovr_delay_wait_checks);
	install_keyword("san_path_err_threshold", &ovr_san_path_err_threshold_handler, &snprint_ovr_san_path_err_threshold);
	install_keyword("san_path_err_forget_rate", &ovr_san_path_err_forget_rate_handler, &snprint_ovr_san_path_err_forget_rate);
	install_keyword("san_path_err_recovery_time", &ovr_san_path_err_recovery_time_handler, &snprint_ovr_san_path_err_recovery_time);
	install_keyword("marginal_path_err_sample_time", &ovr_marginal_path_err_sample_time_handler, &snprint_ovr_marginal_path_err_sample_time);
	install_keyword("marginal_path_err_rate_threshold", &ovr_marginal_path_err_rate_threshold_handler, &snprint_ovr_marginal_path_err_rate_threshold);
	install_keyword("marginal_path_err_recheck_gap_time", &ovr_marginal_path_err_recheck_gap_time_handler, &snprint_ovr_marginal_path_err_recheck_gap_time);
	install_keyword("marginal_path_double_failed_time", &ovr_marginal_path_double_failed_time_handler, &snprint_ovr_marginal_path_double_failed_time);

	install_keyword("skip_kpartx", &ovr_skip_kpartx_handler, &snprint_ovr_skip_kpartx);
	install_keyword("max_sectors_kb", &ovr_max_sectors_kb_handler, &snprint_ovr_max_sectors_kb);
	install_keyword("ghost_delay", &ovr_ghost_delay_handler, &snprint_ovr_ghost_delay);
	install_keyword("all_tg_pt", &ovr_all_tg_pt_handler, &snprint_ovr_all_tg_pt);
	install_keyword("recheck_wwid", &ovr_recheck_wwid_handler, &snprint_ovr_recheck_wwid);
	install_keyword_multi("protocol", &protocol_handler, NULL);
	install_sublevel();
	install_keyword("type", &pc_type_handler, &snprint_pc_type);
	install_keyword("fast_io_fail_tmo", &pc_fast_io_fail_handler, &snprint_pc_fast_io_fail);
	install_keyword("dev_loss_tmo", &pc_dev_loss_handler, &snprint_pc_dev_loss);
	install_keyword("eh_deadline", &pc_eh_deadline_handler, &snprint_pc_eh_deadline);
	install_sublevel_end();

	install_keyword_root("multipaths", &multipaths_handler);
	install_keyword_multi("multipath", &multipath_handler, NULL);
	install_sublevel();
	install_keyword("wwid", &mp_wwid_handler, &snprint_mp_wwid);
	install_keyword("alias", &mp_alias_handler, &snprint_mp_alias);
	install_keyword("path_grouping_policy", &mp_pgpolicy_handler, &snprint_mp_pgpolicy);
	install_keyword("path_selector", &mp_selector_handler, &snprint_mp_selector);
	install_keyword("prio", &mp_prio_name_handler, &snprint_mp_prio_name);
	install_keyword("prio_args", &mp_prio_args_handler, &snprint_mp_prio_args);
	install_keyword("failback", &mp_pgfailback_handler, &snprint_mp_pgfailback);
	install_keyword("rr_weight", &mp_rr_weight_handler, &snprint_mp_rr_weight);
	install_keyword("no_path_retry", &mp_no_path_retry_handler, &snprint_mp_no_path_retry);
	install_keyword("rr_min_io", &mp_minio_handler, &snprint_mp_minio);
	install_keyword("rr_min_io_rq", &mp_minio_rq_handler, &snprint_mp_minio_rq);
	install_keyword("pg_timeout", &deprecated_pg_timeout_handler, &snprint_deprecated);
	install_keyword("flush_on_last_del", &mp_flush_on_last_del_handler, &snprint_mp_flush_on_last_del);
	install_keyword("features", &mp_features_handler, &snprint_mp_features);
	install_keyword("mode", &mp_mode_handler, &snprint_mp_mode);
	install_keyword("uid", &mp_uid_handler, &snprint_mp_uid);
	install_keyword("gid", &mp_gid_handler, &snprint_mp_gid);
	install_keyword("reservation_key", &mp_reservation_key_handler, &snprint_mp_reservation_key);
	install_keyword("user_friendly_names", &mp_user_friendly_names_handler, &snprint_mp_user_friendly_names);
	install_keyword("deferred_remove", &mp_deferred_remove_handler, &snprint_mp_deferred_remove);
	install_keyword("delay_watch_checks", &mp_delay_watch_checks_handler, &snprint_mp_delay_watch_checks);
	install_keyword("delay_wait_checks", &mp_delay_wait_checks_handler, &snprint_mp_delay_wait_checks);
	install_keyword("san_path_err_threshold", &mp_san_path_err_threshold_handler, &snprint_mp_san_path_err_threshold);
	install_keyword("san_path_err_forget_rate", &mp_san_path_err_forget_rate_handler, &snprint_mp_san_path_err_forget_rate);
	install_keyword("san_path_err_recovery_time", &mp_san_path_err_recovery_time_handler, &snprint_mp_san_path_err_recovery_time);
	install_keyword("marginal_path_err_sample_time", &mp_marginal_path_err_sample_time_handler, &snprint_mp_marginal_path_err_sample_time);
	install_keyword("marginal_path_err_rate_threshold", &mp_marginal_path_err_rate_threshold_handler, &snprint_mp_marginal_path_err_rate_threshold);
	install_keyword("marginal_path_err_recheck_gap_time", &mp_marginal_path_err_recheck_gap_time_handler, &snprint_mp_marginal_path_err_recheck_gap_time);
	install_keyword("marginal_path_double_failed_time", &mp_marginal_path_double_failed_time_handler, &snprint_mp_marginal_path_double_failed_time);
	install_keyword("skip_kpartx", &mp_skip_kpartx_handler, &snprint_mp_skip_kpartx);
	install_keyword("max_sectors_kb", &mp_max_sectors_kb_handler, &snprint_mp_max_sectors_kb);
	install_keyword("ghost_delay", &mp_ghost_delay_handler, &snprint_mp_ghost_delay);
	install_sublevel_end();
}
