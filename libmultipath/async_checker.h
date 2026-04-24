// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 SUSE LLC
#ifndef ASYNC_CHECKER_H_INCLUDED
#define ASYNC_CHECKER_H_INCLUDED

struct runner_data;
struct checker;
typedef int (*async_checker_func)(struct runner_data *);

struct runner_data {
	int fd;
	dev_t devt;
	async_checker_func afunc;
	unsigned int timeout;
	int state;
	short msgid;
	char __attribute__((aligned(sizeof(void *)))) checker_ctx[];
};

int async_check_init(struct checker *c);
void async_check_free(struct checker *c);
bool async_check_need_wait(struct checker *c);
int async_check_pending(struct checker *c);
int async_check_check(struct checker *c);

#define CHECKER_MAX_CONTEXT_SIZE 1024

/* For testing handling of async checker timeouts */
#ifdef ASYNC_TEST_MAJOR
static void async_deep_sleep(const struct runner_data *rdata);
#else
#define async_deep_sleep(x) do {} while (0)
#endif

#endif
