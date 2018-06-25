#ifndef _DEFAULTS_H
#define _DEFAULTS_H
#include <limits.h>
#include <string.h>

/*
 * If you add or modify a value also update multipath/multipath.conf.5
 * and the TEMPLATE in libmultipath/hwtable.c
 */
#define DEFAULT_UID_ATTRIBUTE	"ID_SERIAL"
#define DEFAULT_NVME_UID_ATTRIBUTE	"ID_WWN"
#define DEFAULT_DASD_UID_ATTRIBUTE	"ID_UID"
#define DEFAULT_UDEVDIR		"/dev"
#define DEFAULT_MULTIPATHDIR	"/" LIB_STRING "/multipath"
#define DEFAULT_SELECTOR	"service-time 0"
#define DEFAULT_ALIAS_PREFIX	"mpath"
#define DEFAULT_FEATURES	"0"
#define DEFAULT_HWHANDLER	"0"
#define DEFAULT_MINIO		1000
#define DEFAULT_MINIO_RQ	1
#define DEFAULT_PGPOLICY	FAILOVER
#define DEFAULT_FAILBACK	-FAILBACK_MANUAL
#define DEFAULT_RR_WEIGHT	RR_WEIGHT_NONE
#define DEFAULT_NO_PATH_RETRY	NO_PATH_RETRY_UNDEF
#define DEFAULT_VERBOSITY	2
#define DEFAULT_REASSIGN_MAPS	0
#define DEFAULT_FIND_MULTIPATHS	FIND_MULTIPATHS_GREEDY
#define DEFAULT_FAST_IO_FAIL	5
#define DEFAULT_DEV_LOSS_TMO	600
#define DEFAULT_RETAIN_HWHANDLER RETAIN_HWHANDLER_ON
#define DEFAULT_DETECT_PRIO	DETECT_PRIO_ON
#define DEFAULT_DETECT_CHECKER	DETECT_CHECKER_ON
#define DEFAULT_DEFERRED_REMOVE	DEFERRED_REMOVE_OFF
#define DEFAULT_DELAY_CHECKS	NU_NO
#define DEFAULT_ERR_CHECKS	NU_NO
/* half of minimum value for marginal_path_err_sample_time */
#define IOTIMEOUT_SEC		60
#define DEFAULT_UEVENT_STACKSIZE 256
#define DEFAULT_RETRIGGER_DELAY	10
#define DEFAULT_RETRIGGER_TRIES	3
#define DEFAULT_UEV_WAIT_TIMEOUT 30
#define DEFAULT_PRIO		PRIO_CONST
#define DEFAULT_PRIO_ARGS	""
#define DEFAULT_CHECKER		TUR
#define DEFAULT_FLUSH		FLUSH_DISABLED
#define DEFAULT_USER_FRIENDLY_NAMES USER_FRIENDLY_NAMES_OFF
#define DEFAULT_FORCE_SYNC	0
#define UNSET_PARTITION_DELIM "/UNSET/"
#define DEFAULT_PARTITION_DELIM	"-part"
#define DEFAULT_SKIP_KPARTX SKIP_KPARTX_OFF
#define DEFAULT_DISABLE_CHANGED_WWIDS 1
#define DEFAULT_MAX_SECTORS_KB MAX_SECTORS_KB_UNDEF
#define DEFAULT_GHOST_DELAY GHOST_DELAY_OFF
#define DEFAULT_FIND_MULTIPATHS_TIMEOUT -10
#define DEFAULT_UNKNOWN_FIND_MULTIPATHS_TIMEOUT 1
#define DEFAULT_ALL_TG_PT ALL_TG_PT_OFF
#define DEFAULT_RECHECK_WWID RECHECK_WWID_OFF
/* Enable no foreign libraries by default */
#define DEFAULT_ENABLE_FOREIGN "NONE"

#define CHECKINT_UNDEF		UINT_MAX
#define DEFAULT_CHECKINT	5

#define DEV_LOSS_TMO_UNSET	0U
#define MAX_DEV_LOSS_TMO	UINT_MAX
#define DEFAULT_PIDFILE		"/" RUN_DIR "/multipathd.pid"
#define DEFAULT_SOCKET		"/org/kernel/linux/storage/multipathd"
#define DEFAULT_CONFIGFILE	"/etc/multipath.conf"
#define DEFAULT_BINDINGS_FILE	"/etc/multipath/bindings"
#define DEFAULT_WWIDS_FILE	"/etc/multipath/wwids"
#define DEFAULT_PRKEYS_FILE    "/etc/multipath/prkeys"
#define DEFAULT_CONFIG_DIR	"/etc/multipath/conf.d"
#define MULTIPATH_SHM_BASE	"/dev/shm/multipath/"


static inline char *set_default(char *str)
{
	return strdup(str);
}
extern const char *const default_partition_delim;
#endif /* _DEFAULTS_H */
