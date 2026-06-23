// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Test reliability of the runner implementation.
 *
 * This tests simulates "path checkers" being started through the
 * runner code. It creates threads that run for a variable amount of time,
 * optionally ignoring cancellation signals. The runners have a fixed
 * timeout (TIMEOUT_USEC, "-t" option), after which they are considered
 * "hanging" and will be cancelled. The actual runtime of the runner is random.
 * It varies between (TIMEOUT_USEC - NOISE USEC) and
 * (TIMEOUT_USEC + NOISE_BIAS * NOISE_USEC). These noise parameters are
 * set with the "-n" and "-b" option, respectively.
 * This allows simulating frequent races between cancellation and regular
 * completion of threads. Note that because timers in different threads
 * aren't started simultaneously, NOISE_USEC = 0 doesn't mean that the
 * runners will complete exactly at the point in time when there timer
 * expires.
 * The runners simulate waiting checkers by simply sleeping. Optionally,
 * they can ignore cancellation while sleeping (IGNORE_CANCEL, -i) or
 * divide the sleep time in multiple "steps" (-s), between which they can
 * be cancelled.
 * Like multipathd, the main thread "polls" the status of the runners in
 * regular time intervals that are set with POLL_USEC (-p). Because of
 * this polling behavior, it can happen that a runner finishes after
 * its timeout has expired. The runner code (and this test) treats this case
 * as successful completion.
 * If a runner completes, the result is checked against the expected value.
 * The number of threads that have not finished (either successfully or
 * cancelled, plus the number of wrong results of completed runners is
 * the error count.
 * The N_RUNNERS (-N) option determines how many simultaneous threads are
 * started.
 * The test runs until all runners have either completed or expired, or
 * until a maximum wait time is reached, which is calculated from the
 * test parameters (max_wait in run_test()). The REPEAT (-r) parameter
 * determines the number of times the entire test is repeated.
 * The KILL_TIMEOUT (-k) parameter is for simulating a shutdown of the
 * main program (think multipathd). When this timeout expires, all pending
 * runners are cancelled and the program terminates.
 *
 * A "realistic" simulation of multipathd path checkers would use options
 * roughly like this:
 *
 *   runner-test -N 1000 -p 1000 -t 30000 -n 29990 -b 5 -s 1 -i -r 20 -k 300000
 *
 * (note that time options are in ms, whereas the code uses us), but this
 * takes a very long time to run.
 *
 * Scaled down, it becomes:
 *
 *   runner-test -N 1000 -p 100 -t 3000 -n 2999 -b 5 -s 1 -i -r 20 -k 30000
 *
 * A less realistic run with high likelihood of completionc / cancellation races:
 *
 *   runner-test -N 1000 -p 10 -t 3000 -n 1 -b 1 -s 1 -i -r 20
 *
 * Here, all runners finish in a +-1ms timer interval around the timeout.
 * Even with -n 0 (no noise), with a sufficient number of runners, some runners
 * will time out.
 */

#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <stdbool.h>
#include <sched.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/wait.h>
#include "debug.h"
#include "time-util.h"
#include "runner.h"
#include "runner.h"
#include "globals.c"

#define MILLION 1000000
#define BILLION 1000000000

/* For testing startup races, see libmpathutil/runner.c */
#ifndef RUNNER_START_DELAY_US
#define RUNNER_START_DELAY_US 0
#endif

static int N_RUNNERS = 100;
/* sleep time between runner status polling */
static long POLL_USEC = 10000;
/* timeout for runner */
static long TIMEOUT_USEC = 100000;
/* random noise to subtract / add to the sleep time */
static long NOISE_USEC = 10000;
/*
 * Factor to increase noise towards longer sleep times (timeouts).
 * The actual sleep time will be in the interval
 * [ TIMEOUT_USEC - NOISE_USEC, TIMEOUT_USEC + NOISE_USEC * NOISE_BIAS ]
 */
static int NOISE_BIAS = 5;
/* number of sleep intervals the runner uses */
static int SLEEP_STEPS = 1;
/* time after which to kill all runners */
static long KILL_TIMEOUT = 0;
/* number of repeated runs */
static int REPEAT = 10;
/* whether to ignore cancellation signals */
static bool IGNORE_CANCEL = false;

/* gap in the paylod to similate larger size */
#define PAYLOAD_GAP 128

struct payload {
	long wait_nsec;
	int steps;
	bool ignore_cancel;
	int start;
	char pad[PAYLOAD_GAP];
	int end;
};

static void wait_and_add_1(void *arg)
{
	struct payload *t1 = arg;
	struct timespec wait;
	int i, cancelstate;

	wait.tv_sec = t1->wait_nsec / BILLION;
	wait.tv_nsec = t1->wait_nsec % BILLION;
	normalize_timespec(&wait);
	for (i = 0; i < t1->steps; i++) {
		if (t1->ignore_cancel)
			pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancelstate);
		if (nanosleep(&wait, NULL) != 0 && errno != EINTR)
			condlog(3, "%s: nanosleep: %s", __func__, strerror(errno));
		if (t1->ignore_cancel) {
			pthread_setcancelstate(cancelstate, NULL);
		}
		pthread_testcancel();
	}
	t1->end = t1->start + 1;
}

static bool payload_error(const struct payload *p)
{
	return p->end != p->start + 1;
}

static int check_payload(struct runner_context *rctx, bool *error)
{
	struct timespec now;
	struct payload t1;
	int st = check_runner(rctx, &t1, sizeof(t1));

	if (st == RUNNER_RUNNING || st == RUNNER_IDLE)
		return st;

	clock_gettime(CLOCK_MONOTONIC, &now);
	if (st == RUNNER_DONE) {
		int level = 4;

		if (error) {
			*error = payload_error(&t1);
			if (*error)
				level = 2;
		}
		condlog(level,
			"runner %p finished in state 'done' at %lld.%06lld, start %d end %d",
			rctx, (long long)now.tv_sec,
			(long long)now.tv_nsec / 1000, t1.start, t1.end);
	} else
		condlog(4, "runner %p finished in state '%s' at %lld.%06lld",
			rctx, runner_state_name(st), (long long)now.tv_sec,
			(long long)now.tv_nsec / 1000);
	return st;
}

static struct runner_context *
start_runner(long usecs, int start, long noise_range_usec, long bias,
	     int steps, bool ignore_cancel)
{

	struct payload t1;
	struct runner_context *rctx;
	long noise;

	if (noise_range_usec > 0)
		noise = random() % ((bias + 1) * noise_range_usec) -
			noise_range_usec;
	else
		noise = 0;
	t1.start = start;
	t1.end = 0;
	t1.wait_nsec = (usecs + noise) * 1000 / steps;
	t1.wait_nsec = t1.wait_nsec > 0 ? t1.wait_nsec : 0;
	t1.steps = steps;
	t1.ignore_cancel = ignore_cancel;

	rctx = get_runner(wait_and_add_1, &t1, sizeof(t1), usecs);

	if (rctx) {
		struct timespec tmo, finish;

		clock_gettime(CLOCK_MONOTONIC, &tmo);
		tmo.tv_sec += usecs / MILLION;
		tmo.tv_nsec += (usecs % MILLION) * 1000;
		finish = tmo;
		normalize_timespec(&tmo);
		finish.tv_sec += noise / MILLION;
		finish.tv_nsec += (noise % MILLION) * 1000;
		normalize_timespec(&finish);
		condlog(4, "started runner start %d timeout %lld.%06lld, finish %lld.%06lld (noise %ld), steps %d, %signoring cancellation",
			start, (long long)tmo.tv_sec,
			(long long)tmo.tv_nsec / 1000, (long long)finish.tv_sec,
			(long long)finish.tv_nsec / 1000, noise, steps,
			ignore_cancel ? "" : "not ");
		return rctx;
	} else {
		condlog(0, "failed to start runner for start %d", start);
		return NULL;
	}
}

static struct runner_context **context;
static volatile bool must_stop = false;
void int_handler(int signal)
{
	must_stop = true;
}

static void terminate_all(void)
{
	int i, count;
	struct runner_context *rctx;

	for (count = 0, i = 0; i < N_RUNNERS; i++)
		if (context[i]) {
			rctx = context[i];
			context[i] = NULL;
			release_runner(rctx);
			count++;
		}
	condlog(3, "%s: %d runners released", __func__, count);
	/* give runners a chance to clean up */
	sched_yield();
}

static bool test_sleep(const struct timespec *wait)
{
	sigset_t set;

	sigfillset(&set);
	sigdelset(&set, SIGTERM);
	sigdelset(&set, SIGINT);
	sigdelset(&set, SIGQUIT);

	pselect(0, NULL, NULL, NULL, wait, &set);

	if (!must_stop)
		return false;

	terminate_all();
	return true;
}

static int run_test(int n)
{
	int i, running, done, errors;
	const struct timespec wait = {.tv_sec = 0, .tv_nsec = 1000 * POLL_USEC};
	struct timespec stop, now;
	long max_wait = TIMEOUT_USEC + NOISE_BIAS * NOISE_USEC + 100000 +
			RUNNER_START_DELAY_US;
	bool killed = false;
	struct runner_context *rctx;

	for (i = 0; i < N_RUNNERS; i++)
		context[i] = start_runner(TIMEOUT_USEC, i, NOISE_USEC, NOISE_BIAS,
					  SLEEP_STEPS, IGNORE_CANCEL);

	clock_gettime(CLOCK_MONOTONIC, &stop);
	stop.tv_sec += max_wait / MILLION;
	stop.tv_nsec += (max_wait % MILLION) * 1000;
	normalize_timespec(&stop);
	running = N_RUNNERS;
	done = 0;
	errors = 0;
	do {
		bool err = false, last = false;

		killed = test_sleep(&wait);
		if (killed)
			condlog(3, "%s: terminating on signal...", __func__);

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (timespeccmp(&stop, &now) <= 0)
			last = true;

		for (running = 0, i = 0; i < N_RUNNERS; i++) {
			int st;

			if (!context[i])
				continue;
			st = check_payload(context[i], &err);
			switch (st) {
			case RUNNER_DONE:
				if (err)
					errors++;
				done++;
				/* fallthrough */
			case RUNNER_DEAD:
				rctx = context[i];
				context[i] = NULL;
				release_runner(rctx);
				break;
			case RUNNER_CANCELLED:
				if (last) {
					rctx = context[i];
					context[i] = NULL;
					condlog(3, "%s: releasing %p",
						__func__, rctx);
					release_runner(rctx);
				} else
					running++;
				break;
			default:
				running++;
				if (last)
					condlog(1, "%s: found thread in state %s, rctx=%p",
						__func__, runner_state_name(st),
						context[i]);
				break;
			}
		}
		if (killed || last)
			break;
	} while (running);

	condlog(2, "%10d%10d%10d%10d%10d", n, N_RUNNERS, N_RUNNERS - running,
		done, errors);

	if (killed) {
		condlog(2, "%s: termination signal received", __func__);
		exit(0);
	}

	if (running > 0) {
		condlog(1, "ERROR: %d runners haven't finished", running);
		terminate_all();
	}
	return running + errors;
}

static void free_rctxs(struct runner_context ***rctxs)
{
	if (*rctxs)
		free(*rctxs);
}

static int setup_signal_handler(int sig, void (*handler)(int))
{
	sigset_t set;
	sigfillset(&set);
	struct sigaction sga = {.sa_handler = NULL};

	sga.sa_handler = handler;
	sga.sa_mask = set;
	if (sigaction(sig, &sga, NULL) != 0) {
		condlog(1, "%s: failed to install signal handler for %d: %s",
			__func__, sig, strerror(errno));
		return -1;
	}
	return 0;
}

int run_tests(void)
{
	int errors = 0, i;
	struct runner_context **rctxs __attribute__((cleanup(free_rctxs))) = NULL;

	if (setup_signal_handler(SIGINT, int_handler) != 0)
		return -1;

	rctxs = calloc(N_RUNNERS, sizeof(*context));
	if (rctxs == NULL)
		/* arbitrary number to indicate OOM error */
		return 7000;
	context = rctxs;
	for (i = 0; i < REPEAT; i++) {
		errors += run_test(i + 1);
	}
	return errors ? 1 : 0;
}

/* We need to register a dummy handler to avoid system call restarting in
 * pselect() below */
static void dummy_handler(int sig) {}

static int fork_test(void)
{
	sigset_t set;
	pid_t child;
	int wstatus;
	struct timespec wait_to_kill = {.tv_sec = 0};

	/* Block all signals. termination signals will be enabled in test_sleep() */
	sigfillset(&set);
	pthread_sigmask(SIG_SETMASK, &set, NULL);

	child = fork();

	if (child < 0) {
		condlog(0, "error in fork(), %s", strerror(errno));
		return -1;
	} else if (child == 0) {
		/* child */
		int rc = run_tests();
		exit(rc ? 1 : 0);
	}

	setup_signal_handler(SIGCHLD, dummy_handler);

	/* parent */
	if (KILL_TIMEOUT > 0) {
		sigset_t set;

		condlog(3, "%s: == Child %d will be killed with SIGINT after %ld us",
			__func__, child, KILL_TIMEOUT);
		wait_to_kill.tv_sec = KILL_TIMEOUT / MILLION;
		wait_to_kill.tv_nsec = (KILL_TIMEOUT % MILLION) * 1000;

		/*
		 * Unblock SIGCHLD in case thild terminates
		 * (child will receive SIGINT)
		 */
		sigfillset(&set);
		sigdelset(&set, SIGCHLD);
		if (pselect(0, NULL, NULL, NULL, &wait_to_kill, &set) != 0) {
			if (errno == EINTR)
				condlog(2, "main: child terminated");
			else
				condlog(1, "main: error in pselect: %s",
					strerror(errno));
		} else
			kill(child, SIGINT);
	}

	if (waitpid(child, &wstatus, 0) <= 0) {
		condlog(1, "%s: failed to wait for child %d", __func__, child);
		return -1;
	}
	if (WIFEXITED(wstatus)) {
		condlog(3, "%s: child %d return code %d", __func__, child,
			WEXITSTATUS(wstatus));
		return WEXITSTATUS(wstatus);
	} else if (WIFSIGNALED(wstatus)) {
		condlog(2, "%s: child %d killed by signal code %d", __func__,
			child, WTERMSIG(wstatus));
		return -1;
	} else {
		condlog(1, "%s: unexpected status of child %d", __func__, child);
		return -1;
	}
}

static long parse_number(const char *arg, long factor, long deflt)
{
	char *ep;
	long v;

	if (*arg) {
		v = strtol(arg, &ep, 10);
		if (!*ep && v >= 0)
			return factor * v;
	}
	condlog(1, "invalid argument: %s, using %ld", arg, deflt);
	return deflt;
}

static int usage(const char *cmd, int opt)
{
#define USAGE_FMT \
	"Usage: %s [options]\n" \
		"	-N runners:  number of parallel runners\n" \
		"	-p msecs:    time to sleep between status polls in main thread\n" \
		"	-t msecs:    timeout for runners\n" \
		"	-n msecs:    random noise for runner sleep time\n" \
		"	-b bias:     noise increase factor towards sleeping longer\n" \
		"	-s n:        number of steps to divide sleep time into\n" \
		"	-k msecs:    timeout after which to kill all runners (0: don't kill)\n" \
		"	-r n:        number of times to repeat test\n" \
		"	-i:          runners ignore cancellation while sleeping\n" \
		"	-v n:        set verbosity level (default 2)\n" \
		"	-h:          print this help"
	condlog(0, USAGE_FMT, cmd);
	return opt == 'h' ? 0 : 1;
}

int main(int argc, char *argv[])
{
	int opt;
	int total = 0;
	const char *optstring = "+:N:p:t:n:b:s:k:r:v:ih";

	init_test_verbosity(2);

	while ((opt = getopt(argc, argv, optstring)) != -1) {
		switch (opt) {
		case 'N':
			N_RUNNERS = parse_number(optarg, 1L, N_RUNNERS);
			break;
		case 'p':
			POLL_USEC = parse_number(optarg, 1000L, POLL_USEC);
			break;
		case 't':
			TIMEOUT_USEC = parse_number(optarg, 1000L, TIMEOUT_USEC);
			break;
		case 'n':
			NOISE_USEC = parse_number(optarg, 1000L, NOISE_USEC);
			break;
		case 'b':
			NOISE_BIAS = parse_number(optarg, 1L, NOISE_BIAS);
			break;
		case 's':
			SLEEP_STEPS = parse_number(optarg, 1L, SLEEP_STEPS);
			break;
		case 'k':
			KILL_TIMEOUT = parse_number(optarg, 1000L, KILL_TIMEOUT);
			break;
		case 'r':
			REPEAT = parse_number(optarg, 1L, REPEAT);
			break;
		case 'i':
			IGNORE_CANCEL = true;
			break;
		case 'v':
			libmp_verbosity = parse_number(optarg, 1L, libmp_verbosity);
			break;
		case 'h':
		case ':':
		case '?':
			return usage(argv[0], opt);
			break;
		}
	}

	if (optind != argc)
		return usage(argv[0], '?');

	/*
	 * For the sake of this test, infinite timeout makes no sense.
	 * Interpret "-t 0" as "minimal timeout", and use 1us.
	 */
	if (TIMEOUT_USEC == 0)
		TIMEOUT_USEC = 1;

	condlog(2, "Runner: timeout=%ld us, noise interval=[-%ld:%ld] us, steps=%d",
		TIMEOUT_USEC, TIMEOUT_USEC - NOISE_USEC,
		TIMEOUT_USEC + NOISE_BIAS * NOISE_USEC, SLEEP_STEPS);
	condlog(2, "Other : poll interval=%ld us, ignore cancellation=%s, runners=%d, repeat=%d, kill timeout=%ld us",
		POLL_USEC, IGNORE_CANCEL ? "YES" : "NO", N_RUNNERS, REPEAT,
		KILL_TIMEOUT);
	condlog(2, "%10s%10s%10s%10s%10s", "run", "total", "finished",
		"completed", "errors");

	total = fork_test();
	if (total == -1)
		return 130;
	condlog(2, "== TOTAL NUMBER OF FAILED RUNS: %d", total);
	return total ? 1 : 0;
}
