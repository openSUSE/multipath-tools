#ifndef PGPOLICIES_H_INCLUDED
#define PGPOLICIES_H_INCLUDED

#define POLICY_NAME_SIZE 32

/* Storage controllers capabilities */
enum iopolicies {
	IOPOLICY_UNDEF,
	FAILOVER,
	MULTIBUS,
	GROUP_BY_SERIAL,
	GROUP_BY_PRIO,
	GROUP_BY_NODE_NAME,
	GROUP_BY_TPG,
};

int get_pgpolicy_id(char *);
const char *get_pgpolicy_name (int);
int group_paths(struct multipath *, int);
/*
 * policies
 */
int one_path_per_group(struct multipath *, vector);
int one_group(struct multipath *, vector);
int group_by_serial(struct multipath *, vector);
int group_by_prio(struct multipath *, vector);
int group_by_node_name(struct multipath *, vector);
int group_by_tpg(struct multipath *, vector);

#endif
