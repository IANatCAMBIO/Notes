/* ===========================================================================
 * check.h — the test harness: a failure counter and a run-loop pump
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
 *         status 2 when it is unset or is not under a dev/ folder — a test
 *         must never run against a real data directory.
 */
static inline const char *
check_data_dir(void)
{
    const char *dir = getenv(ON_DATA_DIR_ENV);
    if (dir == NULL || strstr(dir, "/dev/") == NULL) {
        fprintf(stderr, "refusing to run: %s must name a folder under dev/ "
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

#endif /* ON_TEST_CHECK_H */
