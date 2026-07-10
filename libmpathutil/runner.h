// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 SUSE LLC
#ifndef RUNNER_H_INCLUDED
#define RUNNER_H_INCLUDED

enum runner_status {
	/**
	 * Initial state. Runner thread has not started yet.
	 */
	RUNNER_IDLE,
	/**
	 * The runner thread is running in @runner_func (see below).
	 */
	RUNNER_RUNNING,
	/**
	 * @runner_func has terminated. This is the only only state
	 * in which @check_runner can obtain user data in the @data
	 * parameter.
	 */
	RUNNER_DONE,
	/**
	 * The runner thread has been cancelled (usually because of a timeout),
	 * but @runner_func is still running.
	 */
	RUNNER_CANCELLED,
	/**
	 * The runner thread has terminated without providing user data
	 * (usually after a timeout).
	 */
	RUNNER_DEAD,
};

typedef void (*runner_func)(void *data);
struct runner_context;

/**
 * runner_state_name(): helper for printing runner states
 *
 * @param state: a valid @runner_status value
 * @returns: a string describing the status
 */
const char *runner_state_name(int state);

/**
 * get_runner(): start a runner thread
 *
 * This function starts a runner thread that calls @func(@data).
 * The thread is created as detached thread.
 * @data will be copied to thread-private memory, which will be freed when
 * the runner terminates.
 * Output values can be retrieved with @check_runner().
 *
 * @param func: the worker function to invoke
 *        This parameter must not be NULL.
 * @param data: pointer the data to pass to the function (input and output)
 *        This parameter must not be NULL.
 * @param size: the size (in bytes) of the data passed to the function
 *        This parameter must be positive.
 * @param timeout_usec: timeout (in microseconds) after which to cancel the
 * runner. If it is 0, the runner will not time out.
 * @returns: a runner context that must be passed to the functions below.
 */
struct runner_context *get_runner(runner_func func, void *data,
				  unsigned int size, unsigned long timeout_usec);

/**
 * release_runner(): release a runner context
 *
 * This function should be called when the caller has no interest to
 * operate on the given @runner_context any more.
 * If necessary, the runner thread will be cancelled.
 * Upon return of this function, @rctx becomes stale and shouldn't accessed
 * any more.
 *
 * @param rctx: the context of the runner to be released.
 */
void release_runner(struct runner_context *rctx);

/**
 * check_runner(): query the state of a runner thread and obtain results
 *
 * Check the state of a runner previously started with @get_runner. If the
 * thread function has completed, @RUNNER_DONE will be returned, and the
 * user data will be copied into the @data argument. If @check_runner returns
 * anything else, @data contains no valid data. The @size argument
 * will typically be the same as the @size passed to @get_runner (meaning
 * that @data represents an object of the same type as the @data argument
 * passed to @get_runner previously).
 *
 * Side effect: If the runner has timed out, the thread will be cancelled.
 *
 * @param rctx: the context of the runner to be queried
 * @param data: memory pointer that will receive results of the worker
 * function. Can be NULL, in which case no data will be copied.
 * @param size: size of the memory pointed to by data. It must be no bigger
 *        than the size of the memory passed to @get_runner for this runner.
 *        It can be smaller, but no more than @size bytes will be copied.
 * @returns: @RUNNER_RUNNING, @RUNNER_DONE, @RUNNER_CANCELLED, or @RUNNER_DEAD.
 */
int check_runner(struct runner_context *rctx, void *data, unsigned int size);

#endif /* RUNNER_H_INCLUDED */
