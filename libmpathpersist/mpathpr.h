#ifndef MPATHPR_H
#define MPATHPR_H

/*
 * This header file contains symbols that are only used by
 * libmpathpersist internally.
 */

int update_prflag(char *mapname, int set);
int update_prkey_flags(char *mapname, uint64_t prkey, uint8_t sa_flags);
#define update_prkey(mapname, prkey) update_prkey_flags(mapname, prkey, 0)

#endif
