// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2018 SUSE Linux GmbH
 */

#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdlib.h>
#include "cmocka-compat.h"
#include "structs.h"
#include "discovery.h"
#include "util.h"

#include "globals.c"

// #include "globals.c"
static char wwid_buf[WWID_SIZE];

int __wrap_check_wwids_file(const char *wwid, int write)
{
	return mock_type(int);
}

static void test_wwid_null(void **state)
{
	size_t res = maybe_unmangle_wwid(wwid_buf, NULL);

	assert_int_equal(res, 0);
	assert_string_equal(wwid_buf, "");
}

static void test_wwid_zero(void **state)
{
	size_t res = maybe_unmangle_wwid(wwid_buf, "");

	assert_int_equal(res, 0);
	assert_string_equal(wwid_buf, "");
}

/* contains no mangled chars */
static void test_wwid_no_unmangle_01(void **state)
{
	const char *const wwid = "simple";
	size_t res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(wwid));
	assert_string_equal(wwid_buf, wwid);
}

/* contains invalid character, not unmangled */
static void test_wwid_no_unmangle_02(void **state)
{
	const char *const wwid = "simple wwid";
	size_t res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(wwid));
	assert_string_equal(wwid_buf, wwid);
}

/* contains unicode character, not unmangled */
static void test_wwid_no_unmangle_03(void **state)
{
	const char *const wwid = "utf8→wwid";
	size_t res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(wwid));
	assert_string_equal(wwid_buf, wwid);
}

/* contains invalid character, not unmangled */
static void test_wwid_no_unmangle_04(void **state)
{
	const char *const wwid = "simple;wwid";
	size_t res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(wwid));
	assert_string_equal(wwid_buf, wwid);
}

/* contains invalid character, not unmangled */
static void test_wwid_no_unmangle_05(void **state)
{
	const char *const wwid = "simple\\wwid";
	size_t res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(wwid));
	assert_string_equal(wwid_buf, wwid);
}

/* contains unicode character, not unmangled */
static void test_wwid_no_unmangle_06(void **state)
{
	const char *const wwid = "utf8→wwid\x20space";
	size_t res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(wwid));
	assert_string_equal(wwid_buf, wwid);
}

/* contains invalid character, not unmangled */
static void test_wwid_no_unmangle_07(void **state)
{
	const char *const wwid = "invalid\\x20;wwid";
	size_t res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(wwid));
	assert_string_equal(wwid_buf, wwid);
}

/* contains invalid character, not unmangled */
static void test_wwid_no_unmangle_08(void **state)
{
	const char *const wwid = "invalid,wwid\x20space";
	size_t res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(wwid));
	assert_string_equal(wwid_buf, wwid);
}

/* contains unicode character, not unmangled */
static void test_wwid_no_unmangle_09(void **state)
{
	const char *const wwid = "utf8\\x20→wwid";
	size_t res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(wwid));
	assert_string_equal(wwid_buf, wwid);
}

/* contains invalid mangled char */
static void test_wwid_no_unmangle_10(void **state)
{
	const char *const wwid = "invalid\\x1wwid";
	size_t res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(wwid));
	assert_string_equal(wwid_buf, wwid);
}

/* test that WWID_SIZE doesn't overflow */
static void test_wwid_overlong_01(void **state)
{
	char wwid[WWID_SIZE + 2];
	size_t res;
	memset(wwid, 'A', sizeof(wwid) - 1);

	wwid[sizeof(wwid) - 1] = '\0';
	res = maybe_unmangle_wwid(wwid_buf, wwid);
	assert_int_equal(res, strlen(wwid));
	wwid[WWID_SIZE - 1] = '\0';
	assert_string_equal(wwid_buf, wwid);
}

/* mangle sequence at the end, fits after mangling */
static void test_wwid_overlong_02(void **state)
{
	char wwid[WWID_SIZE];
	size_t res;
	memset(wwid, 'A', sizeof(wwid) - 1);

	wwid[sizeof(wwid) - 1] = '\0';
	strlcpy(wwid + sizeof(wwid) - 5, "\\x20", 5);
	memcpy(wwid, "\\x20", 4);

	will_return_int(__wrap_check_wwids_file, 0);
	res = maybe_unmangle_wwid(wwid_buf, wwid);

	wwid[3] = ' ';
	strlcpy(wwid + sizeof(wwid) - 5, " ", 5);
	assert_int_equal(res, strlen(wwid + 3));
	assert_string_equal(wwid_buf, wwid + 3);
}

/* truncated mangle sequence at the end, will no be unmangled */
static void test_wwid_overlong_03(void **state)
{
	char wwid[WWID_SIZE + 5];
	size_t res;
	memset(wwid, 'A', sizeof(wwid) - 1);

	wwid[sizeof(wwid) - 1] = '\0';
	strlcpy(wwid + sizeof(wwid) - 4, "\\x20", 4);
	memcpy(wwid, "\\x20", 4);

	res = maybe_unmangle_wwid(wwid_buf, wwid);
	assert_int_equal(res, strlen(wwid));
	wwid[WWID_SIZE - 1] = '\0';
	assert_string_equal(wwid_buf, wwid);
}

/* overlong string fits after unmangling */
static void test_wwid_overlong_04(void **state)
{
	char wwid[WWID_SIZE + 3];
	size_t res;
	memset(wwid, 'A', sizeof(wwid) - 1);

	wwid[sizeof(wwid) - 1] = '\0';
	strlcpy(wwid, "\\x20", sizeof(wwid));

	will_return_int(__wrap_check_wwids_file, 1);
	will_return_int(__wrap_check_wwids_file, 1);
	res = maybe_unmangle_wwid(wwid_buf, wwid);

	wwid[3] = ' ';
	assert_int_equal(res, strlen(wwid + 3));
	assert_string_equal(wwid_buf, wwid + 3);
}

static void test_wwid_unmangle_01(void **state)
{
	const char *const wwid = "wwid\\x20with\\x20spaces";
	const char *const expected = "wwid with spaces";
	size_t res;

	will_return_int(__wrap_check_wwids_file, 1);
	will_return_int(__wrap_check_wwids_file, 1);
	res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(expected));
	assert_string_equal(wwid_buf, expected);
}

static void test_wwid_unmangle_02(void **state)
{
	const char *const wwid = "wwid\\x2cwith\\x2ccommas";
	const char *const expected = "wwid,with,commas";
	size_t res;

	will_return_int(__wrap_check_wwids_file, 1);
	will_return_int(__wrap_check_wwids_file, 1);
	res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(expected));
	assert_string_equal(wwid_buf, expected);
}

static void test_wwid_unmangle_03(void **state)
{
	const char *const wwid = "\\x20\\x20wwid\\x20\\x20\\x20with\\x20spaces\\x20";
	const char *const expected = "  wwid   with spaces ";
	size_t res;

	will_return_int(__wrap_check_wwids_file, 1);
	will_return_int(__wrap_check_wwids_file, 1);
	res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(expected));
	assert_string_equal(wwid_buf, expected);
}

static void test_wwid_unmangle_04(void **state)
{
	const char *const wwid = "wwid\\x20with\\x5cbackslash";
	const char *const expected = "wwid with\\backslash";
	size_t res;

	will_return_int(__wrap_check_wwids_file, 1);
	will_return_int(__wrap_check_wwids_file, 1);
	res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(expected));
	assert_string_equal(wwid_buf, expected);
}

static void test_wwid_unmangle_05(void **state)
{
	const char *const wwid = "wwid\\x20with\\x5cx20mangled\\x20space";
	const char *const expected = "wwid with\\x20mangled space";
	size_t res;

	will_return_int(__wrap_check_wwids_file, 1);
	will_return_int(__wrap_check_wwids_file, 1);
	res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(expected));
	assert_string_equal(wwid_buf, expected);
}

static void test_wwid_unmangle_06(void **state)
{
	const char *const wwid = "wwid\\x20with\\x5cx5cmangled\\x20backslash";
	const char *const expected = "wwid with\\x5cmangled backslash";
	size_t res;

	will_return_int(__wrap_check_wwids_file, 1);
	will_return_int(__wrap_check_wwids_file, 1);
	res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(expected));
	assert_string_equal(wwid_buf, expected);
}

static void test_wwid_unmangle_07(void **state)
{
	const char *const wwid = "wwid\\x20with\\x09unprintable";
	const char *const expected = "wwid with_unprintable";
	size_t res;

	will_return_int(__wrap_check_wwids_file, 1);
	will_return_int(__wrap_check_wwids_file, 1);
	res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(expected));
	assert_string_equal(wwid_buf, expected);
}

static void test_wwid_unmangled_exists(void **state)
{
	const char *const wwid = "wwid\\x20with\\x20spaces";
	const char *const expected = "wwid with spaces";
	size_t res;

	will_return_int(__wrap_check_wwids_file, 0);
	res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(expected));
	assert_string_equal(wwid_buf, expected);
}

static void test_wwid_orig_exists(void **state)
{
	const char *const wwid = "wwid\\x20with\\x20spaces";
	size_t res;

	will_return_int(__wrap_check_wwids_file, 1);
	will_return_int(__wrap_check_wwids_file, 0);
	res = maybe_unmangle_wwid(wwid_buf, wwid);

	assert_int_equal(res, strlen(wwid));
	assert_string_equal(wwid_buf, wwid);
}

static int test_uevent_wwid_mangle(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_wwid_null),
		cmocka_unit_test(test_wwid_zero),
		cmocka_unit_test(test_wwid_no_unmangle_01),
		cmocka_unit_test(test_wwid_no_unmangle_02),
		cmocka_unit_test(test_wwid_no_unmangle_03),
		cmocka_unit_test(test_wwid_no_unmangle_04),
		cmocka_unit_test(test_wwid_no_unmangle_05),
		cmocka_unit_test(test_wwid_no_unmangle_06),
		cmocka_unit_test(test_wwid_no_unmangle_07),
		cmocka_unit_test(test_wwid_no_unmangle_08),
		cmocka_unit_test(test_wwid_no_unmangle_09),
		cmocka_unit_test(test_wwid_no_unmangle_10),
		cmocka_unit_test(test_wwid_overlong_01),
		cmocka_unit_test(test_wwid_overlong_02),
		cmocka_unit_test(test_wwid_overlong_03),
		cmocka_unit_test(test_wwid_overlong_04),
		cmocka_unit_test(test_wwid_unmangle_01),
		cmocka_unit_test(test_wwid_unmangle_02),
		cmocka_unit_test(test_wwid_unmangle_03),
		cmocka_unit_test(test_wwid_unmangle_04),
		cmocka_unit_test(test_wwid_unmangle_05),
		cmocka_unit_test(test_wwid_unmangle_06),
		cmocka_unit_test(test_wwid_unmangle_07),
		cmocka_unit_test(test_wwid_unmangled_exists),
		cmocka_unit_test(test_wwid_orig_exists),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}

int main(void)
{
	int ret = 0;

	init_test_verbosity(-1);
	ret += test_uevent_wwid_mangle();
	return ret;
}
