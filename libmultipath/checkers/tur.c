/*
 * Some code borrowed from sg-utils.
 *
 * Copyright (c) 2004 Christophe Varoqui
 */
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <errno.h>
#include <sys/time.h>

#include "checkers.h"
#include "debug.h"
#include "sg_include.h"
#include "runner.h"

#define TUR_CMD_LEN 6
#define HEAVY_CHECK_COUNT       10
#define MAX_NR_TIMEOUTS 1

enum {
	MSG_TUR_RUNNING = CHECKER_FIRST_MSGID,
	MSG_TUR_TIMEOUT,
	MSG_TUR_FAILED,
	MSG_TUR_TRANSITIONING,
};

#define IDX_(x) (MSG_ ## x - CHECKER_FIRST_MSGID)
const char *libcheck_msgtable[] = {
	[IDX_(TUR_RUNNING)] = " still running",
	[IDX_(TUR_TIMEOUT)] = " timed out",
	[IDX_(TUR_FAILED)] = " failed to initialize",
	[IDX_(TUR_TRANSITIONING)] = " reports path is transitioning",
	NULL,
};

struct tur_data {
	int fd;
	dev_t devt;
	unsigned int timeout;
	int state;
	short msgid;
};

struct tur_checker_context {
	struct checker_context chkr;
	int last_runner_state;
	unsigned int nr_timeouts;
	struct runner_context *rtx;
	struct tur_data tdata;
};

int libcheck_init(struct checker *c)
{
	struct tur_checker_context *tcc;
	struct stat sb;

	tcc = calloc(1, sizeof(*tcc));
	tcc->tdata.state = PATH_UNCHECKED;
	tcc->tdata.fd = -1;
	if (fstat(c->fd, &sb) == 0)
		tcc->tdata.devt = sb.st_rdev;
	tcc->chkr.cls = c->cls;
	c->context = tcc;
	return 0;
}

void libcheck_free (struct checker * c)
{
	struct tur_checker_context *tcc = c->context;

	if (!tcc)
		return;
	c->context = NULL;
	if (tcc->rtx)
		release_runner(tcc->rtx);
	free(tcc);
}

static int
tur_check(int fd, unsigned int timeout, short *msgid)
{
	struct sg_io_hdr io_hdr;
	unsigned char turCmdBlk[TUR_CMD_LEN] = { 0x00, 0, 0, 0, 0, 0 };
	unsigned char sense_buffer[32];
	int retry_tur = 5;

retry:
	memset(&io_hdr, 0, sizeof (struct sg_io_hdr));
	memset(&sense_buffer, 0, 32);
	io_hdr.interface_id = 'S';
	io_hdr.cmd_len = sizeof (turCmdBlk);
	io_hdr.mx_sb_len = sizeof (sense_buffer);
	io_hdr.dxfer_direction = SG_DXFER_NONE;
	io_hdr.cmdp = turCmdBlk;
	io_hdr.sbp = sense_buffer;
	io_hdr.timeout = timeout * 1000;
	io_hdr.pack_id = 0;
	if (ioctl(fd, SG_IO, &io_hdr) < 0) {
		if (errno == ENOTTY) {
			*msgid = CHECKER_MSGID_UNSUPPORTED;
			return PATH_WILD;
		}
		*msgid = CHECKER_MSGID_DOWN;
		return PATH_DOWN;
	}
	if ((io_hdr.status & 0x7e) == 0x18) {
		/*
		 * SCSI-3 arrays might return
		 * reservation conflict on TUR
		 */
		*msgid = CHECKER_MSGID_UP;
		return PATH_UP;
	}
	if (io_hdr.info & SG_INFO_OK_MASK) {
		int key = 0, asc, ascq;

		switch (io_hdr.host_status) {
		case DID_OK:
		case DID_NO_CONNECT:
		case DID_BAD_TARGET:
		case DID_ABORT:
		case DID_TRANSPORT_FAILFAST:
			break;
		default:
			/* Driver error, retry */
			if (--retry_tur)
				goto retry;
			break;
		}
		if (io_hdr.sb_len_wr > 3) {
			if (io_hdr.sbp[0] == 0x72 || io_hdr.sbp[0] == 0x73) {
				key = io_hdr.sbp[1] & 0x0f;
				asc = io_hdr.sbp[2];
				ascq = io_hdr.sbp[3];
			} else if (io_hdr.sb_len_wr > 13 &&
				   ((io_hdr.sbp[0] & 0x7f) == 0x70 ||
				    (io_hdr.sbp[0] & 0x7f) == 0x71)) {
				key = io_hdr.sbp[2] & 0x0f;
				asc = io_hdr.sbp[12];
				ascq = io_hdr.sbp[13];
			}
		}
		if (key == 0x6) {
			/* Unit Attention, retry */
			if (--retry_tur)
				goto retry;
		}
		else if (key == 0x2) {
			/* Not Ready */
			/* Note: Other ALUA states are either UP or DOWN */
			if (asc == 0x04 && ascq == 0x0b) {
				/*
				 * LOGICAL UNIT NOT ACCESSIBLE,
				 * TARGET PORT IN STANDBY STATE
				 */
				*msgid = CHECKER_MSGID_GHOST;
				return PATH_GHOST;
			} else if (asc == 0x04 && ascq == 0x0a) {
				/*
				 * LOGICAL UNIT NOT ACCESSIBLE,
				 * ASYMMETRIC ACCESS STATE TRANSITION
				 */
				*msgid = MSG_TUR_TRANSITIONING;
				return PATH_PENDING;
			}
		} else if (key == 0x5) {
			/* Illegal request */
			if (asc == 0x25 && ascq == 0x00) {
				/*
				 * LUN NOT SUPPORTED: unmapped at target.
				 * Signals pp->disconnected, becomes PATH_DOWN.
				 */
				*msgid = CHECKER_MSGID_DISCONNECTED;
				return PATH_DISCONNECTED;
			}
		}
		*msgid = CHECKER_MSGID_DOWN;
		return PATH_DOWN;
	}
	*msgid = CHECKER_MSGID_UP;
	return PATH_UP;
}

/*
 * Test code for "zombie tur thread" handling.
 * Compile e.g. with CFLAGS=-DTUR_TEST_MAJOR=8
 * Additional parameters can be configure with the macros below.
 *
 * Everty nth started TUR thread will hang in non-cancellable state
 * for given number of seconds, for device given by major/minor.
 */
#ifdef TUR_TEST_MAJOR

#ifndef TUR_TEST_MINOR
#define TUR_TEST_MINOR 0
#endif
#ifndef TUR_SLEEP_INTERVAL
#define TUR_SLEEP_INTERVAL 3
#endif
#ifndef TUR_SLEEP_SECS
#define TUR_SLEEP_SECS 60
#endif

static void tur_deep_sleep(const struct tur_data *tdata)
{
	static int sleep_cnt;
	const struct timespec ts = { .tv_sec = TUR_SLEEP_SECS, .tv_nsec = 0 };
	int oldstate;

	if (tdata->devt != makedev(TUR_TEST_MAJOR, TUR_TEST_MINOR) ||
	    ++sleep_cnt % TUR_SLEEP_INTERVAL != 0)
		return;

	condlog(1, "tur thread going to sleep for %ld seconds", ts.tv_sec);
	if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate) != 0)
		condlog(0, "pthread_setcancelstate: %m");
	if (nanosleep(&ts, NULL) != 0)
		condlog(0, "nanosleep: %m");
	condlog(1, "tur zombie thread woke up");
	if (pthread_setcancelstate(oldstate, NULL) != 0)
		condlog(0, "pthread_setcancelstate (2): %m");
	pthread_testcancel();
}
#else
#define tur_deep_sleep(x) do {} while (0)
#endif /* TUR_TEST_MAJOR */

void runner_callback(void *arg)
{
	struct tur_data *tdata = arg;
	int state;

	condlog(4, "%d:%d : tur checker starting up", major(tdata->devt),
		minor(tdata->devt));

	tur_deep_sleep(tdata);
	state = tur_check(tdata->fd, tdata->timeout, &tdata->msgid);
	tdata->state = state;
	pthread_testcancel();
	condlog(4, "%d:%d : tur checker finished, state %s", major(tdata->devt),
		minor(tdata->devt), checker_state_name(state));
}

static int check_runner_state(struct tur_checker_context *tcc)
{
	struct runner_context *rtx = tcc->rtx;
	int rc;

	rc = check_runner(rtx, &tcc->tdata, sizeof(tcc->tdata));
	switch (rc) {
	case RUNNER_DEAD:
		tcc->tdata.state = PATH_TIMEOUT;
		tcc->tdata.msgid = MSG_TUR_TIMEOUT;
		/* fallthrough */
	case RUNNER_DONE:
		release_runner(tcc->rtx);
		tcc->rtx = NULL;
		tcc->last_runner_state = rc;
		tcc->nr_timeouts = 0;
		condlog(rc == RUNNER_DONE ? 4 : 3,
			"%d:%d : tur checker finished, state %s, runner state %s",
			major(tcc->tdata.devt), minor(tcc->tdata.devt),
			checker_state_name(tcc->tdata.state),
			runner_state_name(rc));
		break;
	case RUNNER_CANCELLED:
		tcc->last_runner_state = rc;
		tcc->tdata.state = PATH_TIMEOUT;
		tcc->tdata.msgid = MSG_TUR_TIMEOUT;
		if (tcc->nr_timeouts < MAX_NR_TIMEOUTS) {
			condlog(3, "%d:%d : tur checker timed out, releasing it",
				major(tcc->tdata.devt), minor(tcc->tdata.devt));
			tcc->nr_timeouts++;
			release_runner(tcc->rtx);
			tcc->rtx = NULL;
		} else if (tcc->nr_timeouts == MAX_NR_TIMEOUTS) {
			tcc->nr_timeouts++;
			condlog(3, "%d:%d : tur checker timed out, waiting for it",
				major(tcc->tdata.devt), minor(tcc->tdata.devt));
		}
		break;
	default:
		condlog(4, "%d:%d : tur checker still running",
			major(tcc->tdata.devt), minor(tcc->tdata.devt));
		tcc->tdata.msgid = MSG_TUR_RUNNING;
		break;
	}
	return rc;
}

bool libcheck_need_wait(struct checker *c)
{
	struct tur_checker_context *ct = c->context;

	return ct && ct->rtx;
}

int libcheck_pending(struct checker *c)
{
	struct tur_checker_context *ct = c->context;
	/* The if path checker isn't running, just return the exiting value. */
	if (!ct || !ct->rtx)
		return c->path_state;

	/* This may nullify ct->rtx */
	check_runner_state(ct);
	c->msgid = ct->tdata.msgid;
	return ct->tdata.state;
}

int libcheck_check(struct checker * c)
{
	struct tur_checker_context *ct = c->context;

	if (!ct)
		return PATH_UNCHECKED;

	if (checker_is_sync(c))
		return tur_check(c->fd, c->timeout, &c->msgid);

	/* Handle the case that the checker just completed */
	if (ct->rtx) {
		check_runner_state(ct);
		c->msgid = ct->tdata.msgid;
		return ct->tdata.state;
	}

	/* create new checker thread */
	ct->tdata.fd = c->fd;
	ct->tdata.timeout = c->timeout;

	ct->tdata.state = PATH_PENDING;
	ct->tdata.msgid = MSG_TUR_RUNNING;
	condlog(3, "%d:%d : starting checker", major(ct->tdata.devt),
		minor(ct->tdata.devt));
	ct->rtx = get_runner(runner_callback, &ct->tdata, sizeof(ct->tdata),
			     1000000 * c->timeout);

	if (ct->rtx) {
		c->msgid = ct->tdata.msgid;
		return ct->tdata.state;
	} else {
		condlog(3, "%d:%d : failed to start tur thread, using sync mode",
			major(ct->tdata.devt), minor(ct->tdata.devt));
		return tur_check(c->fd, c->timeout, &c->msgid);
	}
}
