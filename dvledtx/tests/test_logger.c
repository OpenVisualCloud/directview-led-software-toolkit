/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 *
 * Unit tests for src/util/logger.c using cmocka.
 *
 * Covers:
 *   logger_init()              — null-config rejection, file-logging init
 *   logger_init_default()      — default configuration
 *   logger_cleanup()           — state reset after cleanup
 *   logger_set_level()         — round-trip through get
 *   logger_get_level()         — returns current level
 *   logger_is_level_enabled()  — uninitialized / level-threshold semantics
 *   logger_log()               — no-crash under valid inputs; level filtering
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glob.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "util/logger.h"

/* --------------------------------------------------------------------------
 * Helper: write a temp file path without creating the file.
 * Returns heap-allocated path; caller must unlink+free.
 * -------------------------------------------------------------------------- */
static char *make_tmppath(void)
{
    char *path = strdup("/tmp/dvledtx_logger_test_XXXXXX");
    if (!path) return NULL;
    int fd = mkstemp(path);
    if (fd < 0) { free(path); return NULL; }
    close(fd);
    /* Remove it so logger_init can create it fresh */
    unlink(path);
    return path;
}

/* Tear-down: always call cleanup so the internal global state is reset */
static int teardown(void **state)
{
    (void)state;
    logger_cleanup();
    return 0;
}

/* ==========================================================================
 * logger_init
 * ========================================================================== */

static void test_init_null_config_fails(void **state)
{
    (void)state;
    assert_int_equal(logger_init(NULL), -1);
}

static void test_init_console_only_succeeds(void **state)
{
    (void)state;
    logger_config_t cfg = {
        .level            = LOG_LEVEL_INFO,
        .enable_console   = true,
        .enable_file      = false,
        .enable_timestamp = false,
        .enable_colors    = false,
        .log_file         = NULL,
    };
    assert_int_equal(logger_init(&cfg), 0);
}

static void test_init_file_logging_creates_file(void **state)
{
    (void)state;
    char *path = make_tmppath();
    assert_non_null(path);

    logger_config_t cfg = {
        .level            = LOG_LEVEL_DEBUG,
        .enable_console   = false,
        .enable_file      = true,
        .enable_timestamp = false,
        .enable_colors    = false,
        .log_file         = path,
    };
    int ret = logger_init(&cfg);
    assert_int_equal(ret, 0);

    /* Emit a message to ensure the file is flushed */
    logger_log(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, "test file logging");

    /* File should now exist */
    assert_int_equal(access(path, F_OK), 0);

    logger_cleanup();
    unlink(path);
    free(path);
}

static void test_init_file_logging_bad_path_fails(void **state)
{
    (void)state;
    logger_config_t cfg = {
        .level            = LOG_LEVEL_INFO,
        .enable_console   = false,
        .enable_file      = true,
        .enable_timestamp = false,
        .enable_colors    = false,
        .log_file         = "/no/such/directory/dvledtx_test.log",
    };
    assert_int_equal(logger_init(&cfg), -1);
}

static void test_reinit_succeeds(void **state)
{
    (void)state;
    logger_config_t cfg = {
        .level          = LOG_LEVEL_WARN,
        .enable_console = true,
        .enable_file    = false,
        .log_file       = NULL,
    };
    assert_int_equal(logger_init(&cfg), 0);
    /* Second init (e.g. after config reload) must also succeed */
    cfg.level = LOG_LEVEL_DEBUG;
    assert_int_equal(logger_init(&cfg), 0);
}

/* ==========================================================================
 * logger_init_default
 * ========================================================================== */

static void test_init_default_succeeds(void **state)
{
    (void)state;
    assert_int_equal(logger_init_default(), 0);
}

static void test_init_default_sets_info_level(void **state)
{
    (void)state;
    assert_int_equal(logger_init_default(), 0);
    assert_int_equal((int)logger_get_level(), (int)LOG_LEVEL_INFO);
}

/* ==========================================================================
 * logger_cleanup
 * ========================================================================== */

static void test_cleanup_disables_logging(void **state)
{
    (void)state;
    assert_int_equal(logger_init_default(), 0);
    assert_true(logger_is_level_enabled(LOG_LEVEL_ERROR));

    logger_cleanup();
    assert_false(logger_is_level_enabled(LOG_LEVEL_ERROR));
}

static void test_cleanup_idempotent(void **state)
{
    (void)state;
    assert_int_equal(logger_init_default(), 0);
    logger_cleanup();
    /* Second cleanup on an already-cleaned-up logger must not crash */
    logger_cleanup();
}

/* ==========================================================================
 * logger_set_level / logger_get_level
 * ========================================================================== */

static void test_set_get_level_round_trip(void **state)
{
    (void)state;
    assert_int_equal(logger_init_default(), 0);

    const log_level_t levels[] = {
        LOG_LEVEL_ERROR,
        LOG_LEVEL_WARN,
        LOG_LEVEL_INFO,
        LOG_LEVEL_DEBUG,
    };
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        logger_set_level(levels[i]);
        assert_int_equal((int)logger_get_level(), (int)levels[i]);
    }
}

/* ==========================================================================
 * logger_is_level_enabled
 * ========================================================================== */

static void test_is_level_enabled_false_before_init(void **state)
{
    (void)state;
    /* logger was just cleaned up by teardown from a previous test —
     * or this is the very first test.  Either way: not initialized. */
    logger_cleanup(); /* ensure clean state */
    assert_false(logger_is_level_enabled(LOG_LEVEL_ERROR));
}

static void test_is_level_enabled_at_info_level(void **state)
{
    (void)state;
    assert_int_equal(logger_init_default(), 0); /* sets INFO */

    assert_true (logger_is_level_enabled(LOG_LEVEL_ERROR));
    assert_true (logger_is_level_enabled(LOG_LEVEL_WARN));
    assert_true (logger_is_level_enabled(LOG_LEVEL_INFO));
    assert_false(logger_is_level_enabled(LOG_LEVEL_DEBUG));
}

static void test_is_level_enabled_at_debug_level(void **state)
{
    (void)state;
    assert_int_equal(logger_init_default(), 0);
    logger_set_level(LOG_LEVEL_DEBUG);

    assert_true(logger_is_level_enabled(LOG_LEVEL_ERROR));
    assert_true(logger_is_level_enabled(LOG_LEVEL_WARN));
    assert_true(logger_is_level_enabled(LOG_LEVEL_INFO));
    assert_true(logger_is_level_enabled(LOG_LEVEL_DEBUG));
}

static void test_is_level_enabled_at_error_level(void **state)
{
    (void)state;
    assert_int_equal(logger_init_default(), 0);
    logger_set_level(LOG_LEVEL_ERROR);

    assert_true (logger_is_level_enabled(LOG_LEVEL_ERROR));
    assert_false(logger_is_level_enabled(LOG_LEVEL_WARN));
    assert_false(logger_is_level_enabled(LOG_LEVEL_INFO));
    assert_false(logger_is_level_enabled(LOG_LEVEL_DEBUG));
}

/* ==========================================================================
 * logger_log
 * ========================================================================== */

static void test_log_does_not_crash(void **state)
{
    (void)state;
    assert_int_equal(logger_init_default(), 0);
    /* All four levels — must not crash */
    logger_log(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, "error %d", 1);
    logger_log(LOG_LEVEL_WARN,  __FILE__, __LINE__, __func__, "warn %s",  "msg");
    logger_log(LOG_LEVEL_INFO,  __FILE__, __LINE__, __func__, "info");
    logger_log(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "debug (should be filtered)");
}

static void test_log_skipped_when_level_filtered(void **state)
{
    (void)state;
    /* Init at ERROR-only — DEBUG/INFO/WARN must be silently skipped, not crash */
    logger_config_t cfg = {
        .level          = LOG_LEVEL_ERROR,
        .enable_console = true,
        .enable_file    = false,
        .log_file       = NULL,
    };
    assert_int_equal(logger_init(&cfg), 0);
    logger_log(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "filtered debug");
    logger_log(LOG_LEVEL_INFO,  __FILE__, __LINE__, __func__, "filtered info");
    logger_log(LOG_LEVEL_WARN,  __FILE__, __LINE__, __func__, "filtered warn");
    /* ERROR should still go through without crashing */
    logger_log(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, "not filtered");
}

static void test_log_writes_to_file(void **state)
{
    (void)state;
    char *path = make_tmppath();
    assert_non_null(path);

    logger_config_t cfg = {
        .level            = LOG_LEVEL_DEBUG,
        .enable_console   = false,
        .enable_file      = true,
        .enable_timestamp = false,
        .enable_colors    = false,
        .log_file         = path,
    };
    assert_int_equal(logger_init(&cfg), 0);
    logger_log(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__,
               "hello from test_log_writes_to_file");
    logger_cleanup();

    /* Verify file is non-empty */
    FILE *f = fopen(path, "r");
    assert_non_null(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    assert_true(sz > 0);

    unlink(path);
    free(path);
}

static void test_log_with_timestamp_enabled(void **state)
{
    (void)state;
    assert_int_equal(logger_init_default(), 0);
    /* Timestamp is enabled by default — just verify no crash */
    logger_log(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, "timestamped message");
}

static void test_log_with_colors_disabled(void **state)
{
    (void)state;
    logger_config_t cfg = {
        .level            = LOG_LEVEL_DEBUG,
        .enable_console   = true,
        .enable_file      = false,
        .enable_timestamp = false,
        .enable_colors    = false,
        .log_file         = NULL,
    };
    assert_int_equal(logger_init(&cfg), 0);
    logger_log(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, "no-color error");
    logger_log(LOG_LEVEL_WARN,  __FILE__, __LINE__, __func__, "no-color warn");
    logger_log(LOG_LEVEL_INFO,  __FILE__, __LINE__, __func__, "no-color info");
    logger_log(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "no-color debug");
}

/* ==========================================================================
 * Log rotation — archive old log and create fresh file
 * ========================================================================== */

static void test_log_rotation_archives_old_log(void **state)
{
    (void)state;
    char *path = make_tmppath();
    assert_non_null(path);

    logger_config_t cfg = {
        .level            = LOG_LEVEL_DEBUG,
        .enable_console   = false,
        .enable_file      = true,
        .enable_timestamp = false,
        .enable_colors    = false,
        .log_file         = path,
    };
    assert_int_equal(logger_init(&cfg), 0);

    /* Set a tiny rotation threshold (512 bytes) so we can trigger it easily */
    logger_set_max_size(512);

    /* Write enough data to exceed the 512-byte threshold */
    for (int i = 0; i < 50; i++) {
        logger_log(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__,
                   "rotation test line %d — padding to fill the log quickly", i);
    }

    logger_cleanup();

    /* The current log file should exist and be small (post-rotation) */
    struct stat st;
    assert_int_equal(stat(path, &st), 0);

    /* An archived .gz file should exist matching the pattern <path>.*gz */
    char glob_pattern[600];
    snprintf(glob_pattern, sizeof(glob_pattern), "%s.*.gz", path);
    glob_t gl;
    int gret = glob(glob_pattern, 0, NULL, &gl);
    assert_int_equal(gret, 0);
    assert_true(gl.gl_pathc >= 1);

    /* Archived file should be non-empty */
    struct stat gz_st;
    assert_int_equal(stat(gl.gl_pathv[0], &gz_st), 0);
    assert_true(gz_st.st_size > 0);

    /* Cleanup: remove all generated files */
    for (size_t i = 0; i < gl.gl_pathc; i++)
        unlink(gl.gl_pathv[i]);
    globfree(&gl);
    unlink(path);
    free(path);

    /* Reset rotation size to default */
    logger_set_max_size(0);
}

/* ==========================================================================
 * main
 * ========================================================================== */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* --- logger_init --- */
        cmocka_unit_test_teardown(test_init_null_config_fails,          teardown),
        cmocka_unit_test_teardown(test_init_console_only_succeeds,      teardown),
        cmocka_unit_test_teardown(test_init_file_logging_creates_file,  teardown),
        cmocka_unit_test_teardown(test_init_file_logging_bad_path_fails,teardown),
        cmocka_unit_test_teardown(test_reinit_succeeds,                 teardown),

        /* --- logger_init_default --- */
        cmocka_unit_test_teardown(test_init_default_succeeds,           teardown),
        cmocka_unit_test_teardown(test_init_default_sets_info_level,    teardown),

        /* --- logger_cleanup --- */
        cmocka_unit_test_teardown(test_cleanup_disables_logging,        teardown),
        cmocka_unit_test_teardown(test_cleanup_idempotent,              teardown),

        /* --- logger_set_level / logger_get_level --- */
        cmocka_unit_test_teardown(test_set_get_level_round_trip,        teardown),

        /* --- logger_is_level_enabled --- */
        cmocka_unit_test_teardown(test_is_level_enabled_false_before_init, teardown),
        cmocka_unit_test_teardown(test_is_level_enabled_at_info_level,  teardown),
        cmocka_unit_test_teardown(test_is_level_enabled_at_debug_level, teardown),
        cmocka_unit_test_teardown(test_is_level_enabled_at_error_level, teardown),

        /* --- logger_log --- */
        cmocka_unit_test_teardown(test_log_does_not_crash,              teardown),
        cmocka_unit_test_teardown(test_log_skipped_when_level_filtered, teardown),
        cmocka_unit_test_teardown(test_log_writes_to_file,              teardown),
        cmocka_unit_test_teardown(test_log_with_timestamp_enabled,      teardown),
        cmocka_unit_test_teardown(test_log_with_colors_disabled,        teardown),

        /* --- log rotation --- */
        cmocka_unit_test_teardown(test_log_rotation_archives_old_log,   teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
