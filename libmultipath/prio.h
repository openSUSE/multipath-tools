#ifndef PRIO_H_INCLUDED
#define PRIO_H_INCLUDED

/*
 * knowing about path struct gives flexibility to prioritizers
 */
#include "checkers.h"
#include "vector.h"

/* forward declaration to avoid circular dependency */
struct path;

#include "list.h"
#include "defaults.h"

/*
 * Known prioritizers for use in hwtable.c
 */
#define PRIO_ALUA		"alua"
#define PRIO_CONST		"const"
#define PRIO_DATACORE		"datacore"
#define PRIO_EMC		"emc"
#define PRIO_HDS		"hds"
#define PRIO_HP_SW		"hp_sw"
#define PRIO_IET		"iet"
#define PRIO_ONTAP		"ontap"
#define PRIO_RANDOM		"random"
#define PRIO_RDAC		"rdac"
#define PRIO_WEIGHTED_PATH	"weightedpath"
#define PRIO_SYSFS		"sysfs"
#define PRIO_PATH_LATENCY	"path_latency"
#define PRIO_ANA		"ana"

/*
 * Value used to mark the fact prio was not defined
 */
#define PRIO_UNDEF -1

/*
 * strings lengths
 */
#define LIB_PRIO_NAMELEN 255
#define PRIO_NAME_LEN 16
#define PRIO_ARGS_LEN 255

struct prio {
	void *handle;
	int refcount;
	struct list_head node;
	char name[PRIO_NAME_LEN];
	char args[PRIO_ARGS_LEN];
	int (*getprio)(struct path *, char *);
};

unsigned int get_prio_timeout_ms(const struct path *);
int init_prio(void);
void cleanup_prio (void);
struct prio * add_prio (const char *);
int prio_getprio (struct prio *, struct path *);
void prio_get (struct prio *, const char *, const char *);
void prio_put (struct prio *);
int prio_selected (const struct prio *);
const char * prio_name (const struct prio *);
const char * prio_args (const struct prio *);
int prio_set_args (struct prio *, const char *);

/* The only function exported by prioritizer dynamic libraries (.so) */
int getprio(struct path *, char *);

#endif /* PRIO_H_INCLUDED */
