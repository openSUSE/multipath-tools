// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 SUSE LLC
#include <assert.h>
#include <time.h>
#include <pthread.h>
#include <urcu/uatomic.h>
#include "util.h"
#include "debug.h"
#include "time-util.h"
#include "runner.h"

#define STACK_SIZE (4 * 1024)
#define MILLION 1000000

const char *runner_state_name(int state)
{
	// clang-format off
	static const char * const state_name_[] = {
		[RUNNER_IDLE] = "idle",
		[RUNNER_RUNNING] = "running",
		[RUNNER_DONE] = "done",
		[RUNNER_CANCELLED] = "cancelled",
		[RUNNER_DEAD] = "dead"
	};
	// clang-format on

	if (state < RUNNER_IDLE || state > RUNNER_DEAD)
		return "unknown";
	return state_name_[state];
}

struct runner_context {
	int refcount;
	int status;
	struct timespec deadline;
	pthread_t thr;
	void (*func)(void *data);
	/* User data will be copied into this area */
	char __attribute__((aligned(sizeof(void *)))) data[];
};

static void release_context(struct runner_context *rctx)
{
	int n;

	n = uatomic_sub_return(&rctx->refcount, 1);
	assert(n >= 0);

	if (n == 0)
		free(rctx);
}

static void cleanup_context(struct runner_context **prctx)
{
	struct runner_context *rctx = *prctx;
	int st;

	if (!rctx)
		return;

	st = uatomic_cmpxchg(&rctx->status, RUNNER_RUNNING, RUNNER_DONE);
	if (st != RUNNER_RUNNING) {
		uatomic_cmpxchg(&rctx->status, st, RUNNER_DEAD);
		condlog(st == RUNNER_CANCELLED ? 3 : 2,
			"%s: runner %p finished in state '%s'", __func__, rctx,
			runner_state_name(st));
	}
	release_context(rctx);
}

static void *runner_thread(void *arg)
{
	int st;
	/*
	 * The cleanup function makes sure memory is freed if the thread is
	 * cancelled (-fexceptions).
	 */
	struct runner_context *rctx __attribute__((cleanup(cleanup_context))) = arg;

#ifdef RUNNER_START_DELAY_US
	/*
	 * Compile e.g. with RUNNER_START_DELAY_US=1000 to test races between
	 * thread start and thread cancellation.
	 */
	do {
		struct timespec slp = {.tv_sec = 0,
				       .tv_nsec = 1000 * RUNNER_START_DELAY_US};

		nanosleep(&slp, NULL);
	} while (0);
#endif

	st = uatomic_cmpxchg(&rctx->status, RUNNER_IDLE, RUNNER_RUNNING);
	if (st != RUNNER_IDLE)
		return NULL;

	(*rctx->func)(rctx->data);
	return NULL;
}

static int cancel_runner(struct runner_context *rctx)
{
	int st, st_new;
	int level = 4, retry = 1;

repeat:
	st = uatomic_cmpxchg(&rctx->status, RUNNER_RUNNING, RUNNER_CANCELLED);
	st_new = st;
	switch (st) {
	case RUNNER_IDLE:
		/*
		 * Race with thread startup.
		 *
		 * If after the following cmpxchg st is still IDLE, the cmpxchg
		 * in runner_thread() will return CANCELLED, and the context
		 * will be relased there. Otherwise, the thread has switched
		 * to RUNNING in the meantime, and we will be able to cancel
		 * it regularly if we retry.
		 */
		if (retry--) {
			st = uatomic_cmpxchg(&rctx->status, RUNNER_IDLE,
					     RUNNER_CANCELLED);
			if (st == RUNNER_IDLE)
				st_new = RUNNER_CANCELLED;
			else
				goto repeat;
		}
		break;
	case RUNNER_RUNNING:
		pthread_cancel(rctx->thr);
		st_new = RUNNER_CANCELLED;
		/* fallthrough */
	case RUNNER_CANCELLED:
		break;
	case RUNNER_DONE:
		st_new = RUNNER_DEAD;
		/* fallthrough */
	case RUNNER_DEAD:
		level = 3;
		break;
	}
	condlog(level, "%s: runner %p cancelled in state '%s'", __func__, rctx,
		runner_state_name(st));
	return st_new;
}

void release_runner(struct runner_context *rctx)
{
	cancel_runner(rctx);
	release_context(rctx);
}

int check_runner(struct runner_context *rctx, void *data, unsigned int size)
{
	int st = uatomic_read(&rctx->status);

	switch (st) {
	case RUNNER_DONE:
		if (data)
			/* hand back the data to the caller */
			memcpy(data, rctx->data, size);
		/* fallthrough */
	case RUNNER_DEAD:
	case RUNNER_CANCELLED:
		return st;
	case RUNNER_IDLE:
	case RUNNER_RUNNING:
		if (rctx->deadline.tv_sec != 0 || rctx->deadline.tv_nsec != 0) {
			struct timespec now;

			get_monotonic_time(&now);
			if (timespeccmp(&rctx->deadline, &now) <= 0)
				return cancel_runner(rctx);
		}
		/* don't bother the caller with RUNNER_IDLE */
		return RUNNER_RUNNING;
	default:
		condlog(1, "%s: runner in impossible state '%s'", __func__,
			runner_state_name(st));
		assert(false);
		return st;
	}
}

struct runner_context *get_runner(runner_func func, void *data,
				  unsigned int size, unsigned long timeout_usec)
{
	static const struct timespec time_zero = {.tv_sec = 0};
	struct runner_context *rctx;
	pthread_attr_t attr;
	int rc;

	if (!func || !data || size <= 0) {
		condlog(0, "%s: illegal arguments", __func__);
		return NULL;
	}

	rctx = malloc(sizeof(*rctx) + size);
	if (!rctx)
		return NULL;

	rctx->func = func;
	/*
	 * We have to set the refcount to 2 here. The runner thread may be
	 * cancelled before it even had the chance to increase the refcount,
	 * which could result in a use-after-free in cleanup_context().
	 */
	uatomic_set(&rctx->refcount, 2);
	uatomic_set(&rctx->status, RUNNER_IDLE);
	memcpy(rctx->data, data, size);

	if (timeout_usec) {
		get_monotonic_time(&rctx->deadline);
		rctx->deadline.tv_sec += timeout_usec / MILLION;
		rctx->deadline.tv_nsec += (timeout_usec % MILLION) * 1000;
	} else
		rctx->deadline = time_zero;

	setup_thread_attr(&attr, STACK_SIZE, 1);
	rc = pthread_create(&rctx->thr, &attr, runner_thread, rctx);
	pthread_attr_destroy(&attr);

	if (rc) {
		condlog(1, "%s: pthread_create(): %s", __func__, strerror(rc));
		uatomic_dec(&rctx->refcount);
		release_context(rctx);
		return NULL;
	}
	return rctx;
}
