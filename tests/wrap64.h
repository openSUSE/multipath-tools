#ifndef _WRAP64_H
#define _WRAP64_H 1
#include "util.h"

/*
 * Make cmocka work with _FILE_OFFSET_BITS == 64
 *
 * If -D_FILE_OFFSET_BITS=64 is set, glibc headers replace some functions with
 * their 64bit equivalents. "open()" is replaced by "open64()", etc. cmocka
 * wrappers for these functions must have correct name __wrap_open() doesn't
 * work if the function called is not open() but open64(). Consequently, unit
 * tests using such wrappers will fail.
 * Use some CPP trickery to insert the correct name. The Makefile rule that
 * creates the .wrap file must parse the C preprocessor output to generate the
 * correct -Wl,wrap= option.
 */

/* Without this indirection, WRAP_FUNC(x) would be expanded to __wrap_WRAP_NAME(x) */
#define CONCAT2_(x, y) x ## y
#define CONCAT2(x, y) CONCAT2_(x, y)

#if defined(__GLIBC__) && _FILE_OFFSET_BITS == 64
#define WRAP_NAME(x) x ## 64
#else
#define WRAP_NAME(x) x
#endif
#define WRAP_FUNC(x) CONCAT2(__wrap_, WRAP_NAME(x))
#define REAL_FUNC(x) CONCAT2(__real_, WRAP_NAME(x))

/*
 * fcntl() needs special treatment; fcntl64() has been introduced in 2.28.
 * https://savannah.gnu.org/forum/forum.php?forum_id=9205
 */
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 28)
#define WRAP_FCNTL_NAME WRAP_NAME(fcntl)
#else
#define WRAP_FCNTL_NAME fcntl
#endif
#define WRAP_FCNTL CONCAT2(__wrap_, WRAP_FCNTL_NAME)
#define REAL_FCNTL CONCAT2(__real_, WRAP_FCNTL_NAME)

/*
 * will_return() is itself a macro that uses CPP "stringizing". We need a
 * macro indirection to make sure the *value* of WRAP_FUNC() is stringized
 * (see https://gcc.gnu.org/onlinedocs/cpp/Stringizing.html).
 */
#define wrap_will_return(x, y) will_return(x, y)
#endif
