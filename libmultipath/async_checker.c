// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 SUSE LLC

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdbool.h>
#include "async_checker.h"
#include "checkers.h"
#include "debug.h"
#include "runner.h"

#define MAX_NR_TIMEOUTS 1

struct async_checker_context {
	int last_runner_state;
	unsigned int nr_timeouts;
	struct runner_context *rtx;
	struct runner_data rdata;
};

#define rdata_size(acc) (sizeof(acc->rdata))

int async_check_init(struct checker *c)
{
	struct async_checker_context *acc;
	struct stat sb;

	acc = calloc(1, sizeof(*acc));
	if (!acc)
		return -1;
	acc->rdata.state = PATH_UNCHECKED;
	acc->rdata.fd = -1;
	if (fstat(c->fd, &sb) == 0)
		acc->rdata.devt = sb.st_rdev;
	c->context = acc;
	return 0;
}

void async_check_free(struct checker *c)
{
	struct async_checker_context *acc = c->context;

	if (!acc)
		return;
	c->context = NULL;
	if (acc->rtx)
		release_runner(acc->rtx);
	free(acc);
}

static void runner_callback(void *arg)
{
	struct runner_data *rdata = arg;
	int state;

	condlog(4, "%d:%d : async checker starting up", major(rdata->devt),
		minor(rdata->devt));

	async_deep_sleep(rdata);
	state = rdata->afunc(rdata);
	rdata->state = state;
	pthread_testcancel();
	condlog(4, "%d:%d : async checker finished, state %s", major(rdata->devt),
		minor(rdata->devt), checker_state_name(state));
}

static int check_runner_state(struct async_checker_context *acc)
{
	struct runner_context *rtx = acc->rtx;
	int rc;

	rc = check_runner(rtx, &acc->rdata, rdata_size(acc));
	switch (rc) {
	case RUNNER_DEAD:
		acc->rdata.state = PATH_TIMEOUT;
		acc->rdata.msgid = CHECKER_MSGID_TIMEOUT;
		/* fallthrough */
	case RUNNER_DONE:
		release_runner(acc->rtx);
		acc->rtx = NULL;
		acc->last_runner_state = rc;
		acc->nr_timeouts = 0;
		condlog(rc == RUNNER_DONE ? 4 : 3,
			"%d:%d : async checker finished, state %s, runner state %s",
			major(acc->rdata.devt), minor(acc->rdata.devt),
			checker_state_name(acc->rdata.state),
			runner_state_name(rc));
		break;
	case RUNNER_CANCELLED:
		acc->last_runner_state = rc;
		acc->rdata.state = PATH_TIMEOUT;
		acc->rdata.msgid = CHECKER_MSGID_TIMEOUT;
		if (acc->nr_timeouts < MAX_NR_TIMEOUTS) {
			condlog(3, "%d:%d : async checker timed out, releasing it",
				major(acc->rdata.devt), minor(acc->rdata.devt));
			acc->nr_timeouts++;
			release_runner(acc->rtx);
			acc->rtx = NULL;
		} else if (acc->nr_timeouts == MAX_NR_TIMEOUTS) {
			acc->nr_timeouts++;
			condlog(3, "%d:%d : async checker timed out, waiting for it",
				major(acc->rdata.devt), minor(acc->rdata.devt));
		}
		break;
	default:
		condlog(4, "%d:%d : async checker still running",
			major(acc->rdata.devt), minor(acc->rdata.devt));
		acc->rdata.msgid = CHECKER_MSGID_RUNNING;
		break;
	}
	return rc;
}

bool async_check_need_wait(struct checker *c)
{
	struct async_checker_context *acc = c->context;

	return acc && acc->rtx;
}

int async_check_pending(struct checker *c)
{
	struct async_checker_context *acc = c->context;
	/* The if path checker isn't running, just return the exiting value. */
	if (!acc || !acc->rtx)
		return c->path_state;

	/* This may nullify ct->rtx */
	check_runner_state(acc);
	c->msgid = acc->rdata.msgid;
	return acc->rdata.state;
}

int async_check_check(struct checker *c)
{
	struct async_checker_context *acc = c->context;

	if (!acc)
		return PATH_UNCHECKED;

	if (checker_is_sync(c))
		return acc->rdata.afunc(&acc->rdata);

	/* Handle the case that the checker just completed */
	if (acc->rtx) {
		check_runner_state(acc);
		c->msgid = acc->rdata.msgid;
		return acc->rdata.state;
	}

	/* create new checker thread */
	acc->rdata.fd = c->fd;
	acc->rdata.timeout = c->timeout;

	acc->rdata.state = PATH_PENDING;
	acc->rdata.msgid = CHECKER_MSGID_RUNNING;
	acc->rdata.afunc = c->cls->async_func;
	condlog(4, "%d:%d : starting checker", major(acc->rdata.devt),
		minor(acc->rdata.devt));
	acc->rtx = get_runner(runner_callback, &acc->rdata, rdata_size(acc),
			      1000000 * c->timeout);

	if (acc->rtx) {
		c->msgid = acc->rdata.msgid;
		return acc->rdata.state;
	} else {
		condlog(3, "%d:%d : failed to start async thread, using sync mode",
			major(acc->rdata.devt), minor(acc->rdata.devt));
		return acc->rdata.afunc(&acc->rdata);
	}
}

/*
 * Test code for "zombie tur thread" handling.
 * Compile e.g. with CFLAGS=-DASYNC_TEST_MAJOR=8
 * Additional parameters can be configure with the macros below.
 *
 * Everty nth started thread will hang in non-cancellable state
 * for given number of seconds, for device given by major/minor.
 */
#ifdef ASYNC_TEST_MAJOR
#ifndef ASYNC_TEST_MINOR
#define ASYNC_TEST_MINOR 0
#endif
#ifndef ASYNC_SLEEP_INTERVAL
#define ASYNC_SLEEP_INTERVAL 3
#endif
#ifndef ASYNC_SLEEP_SECS
#define ASYNC_SLEEP_SECS 60
#endif

static void async_deep_sleep(const struct runner_data *rdata)
{
	static int sleep_cnt;
	const struct timespec ts = {.tv_sec = ASYNC_SLEEP_SECS, .tv_nsec = 0};
	int oldstate;

	if (rdata->devt != makedev(ASYNC_TEST_MAJOR, ASYNC_TEST_MINOR) ||
	    ++sleep_cnt % ASYNC_SLEEP_INTERVAL == 0)
		return;

	condlog(3, "async thread going to sleep for %ld seconds", ts.tv_sec);
	if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate) != 0)
		condlog(0, "pthread_setcancelstate: %m");
	if (nanosleep(&ts, NULL) != 0)
		condlog(0, "nanosleep: %m");
	condlog(3, "async zombie thread woke up");
	if (pthread_setcancelstate(oldstate, NULL) != 0)
		condlog(0, "pthread_setcancelstate (2): %m");
	pthread_testcancel();
}
#endif /* ASYNC_TEST_MAJOR */
