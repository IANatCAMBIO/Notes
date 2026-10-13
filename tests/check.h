/* ===========================================================================
 * check.h — the test harness: a failure counter, a run-loop pump and a
 * date helper
 *
 * Each tests/<name>_test.c is a plain main() linked against the core.  It
 * calls check() for every expectation and returns check_result() — non-zero
 * if any failed — so `make test` stops at the first failing program.
 *
 * Every test runs with NOTES_DATA_DIR pointing at its own folder under
 * dev/test (the Makefile sets it): check_data_dir() refuses to run
 * otherwise, so a test started by hand can never touch a real data
 * directory.
 * =========================================================================== */

#ifndef ON_TEST_CHECK_H
#define ON_TEST_CHECK_H

#include <CoreFoundation/CoreFoundation.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "identity.h"

static int check_failures;           /* expectations that did not hold       */

/*
 * check — record one expectation.
 *
 * Inputs:
 *   ok   — whether it held
 *   what — what was expected, for the log
 *
 * Output: none; prints "ok   <what>" or "FAIL <what>".
 */
static inline void
check(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        check_failures++;
    }
}

/*
 * check_result — the program's exit status.
 *
 * Output: 0 when every expectation held, 1 otherwise.
 */
static inline int
check_result(void)
{
    if (check_failures > 0) {
        printf("%d check(s) failed\n", check_failures);
    }
    return check_failures > 0 ? 1 : 0;
}

/*
 * check_data_dir — the test's data directory, from the environment.
 *
 * Output: the directory `make test` set in NOTES_DATA_DIR.  Exits with
 *         status 2 when it is unset, is not an absolute path under a
 *         dev/test/ folder, or climbs out of one with ".." — a test must
 *         never run against a real data directory.
 */
static inline const char *
check_data_dir(void)
{
    const char *dir = getenv(ON_DATA_DIR_ENV);
    if (dir == NULL || dir[0] != '/' || strstr(dir, "/dev/test/") == NULL ||
        strstr(dir, "..") != NULL) {
        fprintf(stderr, "refusing to run: %s must name a folder under dev/test/ "
                        "(run the tests with `make test`)\n",
                ON_DATA_DIR_ENV);
        exit(2);
    }
    return dir;
}

/*
 * check_pump — run the main run loop, which drains the main dispatch
 * queue, until *done becomes true or the time runs out.  This is how a
 * test waits for anything that completes through on_main_post or a timer.
 *
 * Inputs:
 *   done    — the flag a callback sets; NULL to simply run for `seconds`
 *   seconds — the longest to wait
 *
 * Output: true when *done was set in time.
 */
static inline bool
check_pump(const bool *done, double seconds)
{
    CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + seconds;
    while ((done == NULL || !*done) && CFAbsoluteTimeGetCurrent() < end) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
    }
    return done != NULL && *done;
}

/*
 * check_midnight — a calendar date as the local-midnight UNIX time an
 * action item's due date is stored as.
 *
 * Inputs:
 *   year, month, day — the date (month 1-12)
 *
 * Output: the timestamp.
 */
static inline long long
check_midnight(int year, int month, int day)
{
    struct tm tm = { 0 };
    tm.tm_year  = year - 1900;
    tm.tm_mon   = month - 1;
    tm.tm_mday  = day;
    tm.tm_isdst = -1;
    return (long long)mktime(&tm);
}

#endif /* ON_TEST_CHECK_H */
