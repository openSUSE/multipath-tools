#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <pthread.h>
#include <urcu/uatomic.h>
#include "cmocka-compat.h"
#include "util.h"
#include "globals.c"

struct s_4 {
	uint32_t val;
};

struct s_8 {
	char pad[4];
	uint32_t val;
};

struct s_64 {
	char pad[60];
	uint32_t val;
};

// clang-format off
#define make_destruct(n, size)	     \
static void destruct_ ## n ## _ ## size(void *ptr) \
{						  \
	struct s_##size *s = ptr;		  \
						  \
	assert_ptr_not_equal(ptr, NULL);	  \
	assert_int_equal(s->val, n);		  \
}

#define make_fn(size) \
static void *thread_##size(void *arg)		\
{						\
	struct s_##size *s = arg;		\
						\
	get_shared_ptr(s);			\
	uatomic_add(&s->val, 1);		\
	put_shared_ptr(s);			\
	return NULL;				\
}

make_fn(4)
make_fn(8)
make_fn(64)

#define make_test(n, size)					\
make_destruct(n, size)						\
								\
static void test_ ## n ## _ ## size(void **state)		\
{								\
	pthread_attr_t attr;					\
	pthread_t threads[n];					\
	struct s_##size *s;					\
	int i, rc;						\
								\
	rc = pthread_attr_init(&attr);				\
	assert_int_equal(rc, 0);				\
	rc = pthread_attr_setstacksize(&attr,			\
			PTHREAD_STACK_MIN + 4096);		\
	assert_int_equal(rc, 0);				\
	s = alloc_shared_ptr(size, destruct_ ## n## _ ## size);	\
	assert_ptr_not_equal(s, NULL);				\
	s->val = 0;						\
	for (i = 0; i < n; i++) {				\
		rc = pthread_create(&threads[i], &attr,		\
				    thread_##size, s);		\
		assert_int_equal(rc, 0);			\
	}							\
	pthread_attr_destroy(&attr);				\
	for (i = 0; i < n; i++) {				\
		rc = pthread_join(threads[i], NULL);		\
		assert_int_equal(rc, 0);			\
	}							\
	assert_int_equal(uatomic_read(&s->val), n);		\
	put_shared_ptr(s);					\
}

make_test(0, 4)
make_test(100, 4)
make_test(1000, 4)
make_test(0, 8)
make_test(100, 8)
make_test(1000, 8)
make_test(0, 64)
make_test(100, 64)
make_test(1000, 64)
	// clang-format on

	int test_shared_ptr(void)
{
	// clang-format off
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_0_4),
		cmocka_unit_test(test_100_4),
		cmocka_unit_test(test_1000_4),
		cmocka_unit_test(test_0_8),
		cmocka_unit_test(test_100_8),
		cmocka_unit_test(test_1000_8),
		cmocka_unit_test(test_0_64),
		cmocka_unit_test(test_100_64),
		cmocka_unit_test(test_1000_64),
	};
	// clang-format on
	return cmocka_run_group_tests(tests, NULL, NULL);
}

int main(void)
{
	int ret = 0;

	init_test_verbosity(-1);
	ret += test_shared_ptr();
	return ret;
}
