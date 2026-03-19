#! /bin/sh
: "${MPATHTEST_VERBOSITY:=2}"

export LD_LIBRARY_PATH=../libmultipath:../libmpathutil:../libmpathcmd
export MPATHTEST_VERBOSITY

RUNNER=./runner-test
if [ "$VALGRIND" ]; then
    command -v valgrind >/dev/null && \
	RUNNER="valgrind --leak-check=full --error-exitcode=128 --max-threads=5000 --suppressions=./runner-test.supp ./runner-test"
fi

# LSAN is not supported on ppc64le
case $(uname -m) in
    x86_64|aarch64)
	export ASAN_OPTIONS="detect_leaks=1:detect_odr_violation=0"
	export LSAN_OPTIONS="report_objects=1"
	;;
esac

LONG=
while [ $# -gt 0 ]; do
    case $1 in
	-l) LONG=1;;
    esac
    shift
done

if [ "$LONG" ]; then
    TIME1=30000
    TIME2=23000
else
    TIME1=7500
    TIME2=4700
fi

# Test scenarios
# 1. timeout 1 us - test runner creation / cancellation races
# 2. timeout 1 ms - test runner creation / cancellation races with -DRUNNER_START_DELAY_US=1000
# 3. "realistic" test, scaled down by a factor 10 in time
# 4./5. Tests with high likelihood of completion / cancellation race
set -- \
    "-N 100 -p 1 -t 0 -n 2 -b 1 -s 1 -i -r 20" \
    "-N 100 -p 1 -t 1 -n 2 -b 1 -s 1 -i -r 20" \
    "-N 1000 -p 100 -t 3000 -n 2999 -b 5 -s 1 -i -r 20 -k $TIME1" \
    "-N 1000 -p 10 -t 3000 -n 1 -b 1 -s 1 -i -r 20 -k $TIME2" \
    "-N 100 -p 1 -t 3000 -n 0 -s 1 -i -r 5"

errors=0
for args in "$@"; do
    echo "=== $RUNNER $args"
    # shellcheck disable=SC2086
    $RUNNER $args || errors=$((errors+1))
done

echo "$0: ERRORS: $errors"
[ $errors -eq 0 ]
