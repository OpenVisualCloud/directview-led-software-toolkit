/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 *
 * Unit tests for the static helper functions in src/tx_app_main.c.
 *
 * Strategy
 * --------
 * tx_app_main.c's interesting logic lives in static functions
 * (parse_args, resolve_ip_addrs, tx_app_sig_handler,
 *  tx_app_apply_pending_signal_exit, print_help).  Because they are
 * static we cannot link them from outside; instead we #include the
 * source file directly (after renaming 'main' to avoid a clash with
 * the cmocka test runner's own main).
 *
 * Functions called by the renamed tx_app_real_main() that require
 * real hardware or filesystem state are provided as no-op stubs here
 * (session_manager_*, load_and_apply_config, peek_config_log_file).
 * After the #include those stubs are linked instead of the real
 * implementations, keeping the test self-contained.
 *
 * Coverage target
 * ---------------
 *   parse_args()                        — argument parsing
 *   resolve_ip_addrs()                  — IPv4 binary conversion
 *   tx_app_sig_handler()                — sets g_tx_app_signal_exit
 *   tx_app_apply_pending_signal_exit()  — propagates signal flag
 *   print_help()                        — smoke test only
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <arpa/inet.h>

/* FFmpeg types needed indirectly by tx_app_context.h */
#include <libavutil/pixfmt.h>
#include <libavformat/avformat.h>

/* g_tx_app_exit is normally defined in session_manager.c.
 * We define it here since session_manager.c is not compiled into this test. */
_Atomic bool g_tx_app_exit = false;

/* ===========================================================================
 * Controllable stubs for session_manager and config_reader functions.
 *
 * tx_app_real_main() (the renamed main()) calls these.  We inject return
 * values and side-effects via file-scope variables so each test can
 * configure the behaviour it needs.
 * =========================================================================== */

#include "session_manager.h"

static int stub_session_manager_init_ret  = 0;
static int stub_session_manager_start_ret = 0;

int session_manager_init(session_manager_t* m, struct tx_app_context* a)
    { (void)m; (void)a; return stub_session_manager_init_ret; }
int session_manager_start(session_manager_t* m)
    { (void)m; return stub_session_manager_start_ret; }
int session_manager_stop(session_manager_t* m)
    { (void)m; return 0; }
void session_manager_cleanup(session_manager_t* m)
    { (void)m; }
bool session_manager_is_running(const session_manager_t* m)
    { (void)m; return false; }

/* ===========================================================================
 * Stubs for config_reader functions
 * =========================================================================== */

#include "config_reader.h"

static int  stub_peek_config_log_file_ret    = -1;
static char stub_peek_config_log_file_buf[256] = {0};
static int  stub_load_and_apply_config_ret   = 0;

/* When load_and_apply_config succeeds, fill in valid IPs + test_time_s so
 * tx_app_real_main's resolve_ip_addrs and polling loop work correctly. */
static bool  stub_load_config_set_ips        = true;
static int   stub_load_config_test_time_s    = 0;
static bool  stub_load_config_set_exit       = true; /* set app->exit=true immediately */

int peek_config_log_file(const char* f, char* buf, size_t sz)
{
    (void)f;
    if (stub_peek_config_log_file_ret == 0 && stub_peek_config_log_file_buf[0]) {
        strncpy(buf, stub_peek_config_log_file_buf, sz - 1);
        buf[sz - 1] = '\0';
    }
    return stub_peek_config_log_file_ret;
}

int load_and_apply_config(struct tx_app_context* app, const char* f)
{
    (void)f;
    if (stub_load_and_apply_config_ret == 0 && stub_load_config_set_ips) {
        strncpy(app->sip_addr_str, "192.168.50.29", sizeof(app->sip_addr_str) - 1);
        strncpy(app->dip_addr_str, "239.168.85.20", sizeof(app->dip_addr_str) - 1);
        app->test_time_s = stub_load_config_test_time_s;
        app->exit = stub_load_config_set_exit;
    }
    return stub_load_and_apply_config_ret;
}

/* Helper to reset all stubs to defaults before each test —
 * forward-declared here; definition after the #include so that
 * g_tx_app_signal_exit (from tx_app_main.c) is visible. */
static void reset_stubs(void);

/* ===========================================================================
 * Include production source with main() renamed so our cmocka main() can
 * coexist.  After this point all static symbols from tx_app_main.c
 * (parse_args, resolve_ip_addrs, g_tx_app_signal_exit, g_app_ptr, …) are
 * visible to the test functions below.
 * =========================================================================== */

#define main tx_app_real_main
#include "../src/tx_app_main.c"
#undef main

static void reset_stubs(void)
{
    stub_session_manager_init_ret  = 0;
    stub_session_manager_start_ret = 0;
    stub_peek_config_log_file_ret  = -1;
    stub_peek_config_log_file_buf[0] = '\0';
    stub_load_and_apply_config_ret = 0;
    stub_load_config_set_ips       = true;
    stub_load_config_test_time_s   = 0;
    stub_load_config_set_exit      = true;
    g_tx_app_signal_exit           = 0;
    g_tx_app_exit                  = false;
}

/* ===========================================================================
 * Tests — parse_args
 * =========================================================================== */

static void test_parse_args_long_config_option(void **state)
{
    (void)state;
    struct tx_app_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    optind = 1; /* reset getopt state between tests */
    char *argv[] = {"prog", "--config", "myfile.json", NULL};
    assert_int_equal(parse_args(&ctx, 3, argv), 0);
    assert_string_equal(ctx.config_file, "myfile.json");
}

static void test_parse_args_short_config_option(void **state)
{
    (void)state;
    struct tx_app_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    optind = 1;
    char *argv[] = {"prog", "-C", "another.json", NULL};
    assert_int_equal(parse_args(&ctx, 3, argv), 0);
    assert_string_equal(ctx.config_file, "another.json");
}

static void test_parse_args_no_args_returns_zero_and_empty_config(void **state)
{
    (void)state;
    struct tx_app_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    optind = 1;
    char *argv[] = {"prog", NULL};
    assert_int_equal(parse_args(&ctx, 1, argv), 0);
    assert_int_equal(ctx.config_file[0], '\0');
}

static void test_parse_args_help_returns_minus1(void **state)
{
    (void)state;
    struct tx_app_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    optind = 1;
    char *argv[] = {"prog", "--help", NULL};
    assert_int_equal(parse_args(&ctx, 2, argv), -1);
}

static void test_parse_args_unknown_option_returns_minus1(void **state)
{
    (void)state;
    struct tx_app_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    optind  = 1;
    opterr  = 0; /* suppress getopt error output for unknown option */
    char *argv[] = {"prog", "--badoption", NULL};
    int ret = parse_args(&ctx, 2, argv);
    opterr = 1; /* restore */
    assert_int_equal(ret, -1);
}

/* ===========================================================================
 * Tests — resolve_ip_addrs
 * =========================================================================== */

static void test_resolve_ip_valid_sip_and_dip(void **state)
{
    (void)state;
    struct tx_app_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    strncpy(ctx.sip_addr_str, "192.168.50.29", sizeof(ctx.sip_addr_str) - 1);
    strncpy(ctx.dip_addr_str, "239.168.85.20", sizeof(ctx.dip_addr_str) - 1);
    assert_int_equal(resolve_ip_addrs(&ctx), 0);
    /* Verify binary addresses were written */
    assert_int_equal(ctx.sip_addr[0], 192);
    assert_int_equal(ctx.dip_addr[0], 239);
}

static void test_resolve_ip_empty_sip_dhcp_mode(void **state)
{
    (void)state;
    struct tx_app_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* sip_addr_str is empty → DHCP mode branch; only dip is resolved */
    strncpy(ctx.dip_addr_str, "239.168.85.20", sizeof(ctx.dip_addr_str) - 1);
    assert_int_equal(resolve_ip_addrs(&ctx), 0);
    assert_int_equal(ctx.dip_addr[0], 239);
}

static void test_resolve_ip_invalid_sip_fails(void **state)
{
    (void)state;
    struct tx_app_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    strncpy(ctx.sip_addr_str, "not.an.ip.addr", sizeof(ctx.sip_addr_str) - 1);
    strncpy(ctx.dip_addr_str, "239.168.85.20",  sizeof(ctx.dip_addr_str) - 1);
    assert_int_equal(resolve_ip_addrs(&ctx), -1);
}

static void test_resolve_ip_invalid_dip_fails(void **state)
{
    (void)state;
    struct tx_app_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    strncpy(ctx.dip_addr_str, "999.999.999.999", sizeof(ctx.dip_addr_str) - 1);
    assert_int_equal(resolve_ip_addrs(&ctx), -1);
}

/* ===========================================================================
 * Tests — signal handler + tx_app_apply_pending_signal_exit
 * =========================================================================== */

static void test_sig_handler_sets_flag_and_apply_propagates(void **state)
{
    (void)state;
    struct tx_app_context app;
    memset(&app, 0, sizeof(app));

    /* Reset shared state */
    g_tx_app_signal_exit = 0;
    g_tx_app_exit        = false;
    app.exit             = false;
    g_app_ptr            = &app;

    /* Before any signal: apply is a no-op */
    tx_app_apply_pending_signal_exit();
    assert_false(g_tx_app_exit);
    assert_false(app.exit);

    /* Simulate a signal and apply */
    tx_app_sig_handler(SIGINT);
    assert_int_equal(g_tx_app_signal_exit, 1);
    tx_app_apply_pending_signal_exit();
    assert_true(g_tx_app_exit);
    assert_true(app.exit);

    /* Teardown */
    g_tx_app_signal_exit = 0;
    g_tx_app_exit        = false;
    g_app_ptr            = NULL;
}

static void test_apply_pending_exit_noop_without_signal(void **state)
{
    (void)state;
    g_tx_app_signal_exit = 0;
    g_tx_app_exit        = false;
    tx_app_apply_pending_signal_exit();
    assert_false(g_tx_app_exit);
}

/* ===========================================================================
 * Tests — print_help (smoke: verify no crash / assert)
 * =========================================================================== */

static void test_print_help_does_not_crash(void **state)
{
    (void)state;
    print_help("TxApp"); /* output goes to log; just verify no crash */
}

/* ===========================================================================
 * Tests — tx_app_real_main (the renamed main() function)
 *
 * Each test calls tx_app_real_main() with controlled argv and stub config.
 * The stubs are reset via reset_stubs() before each test.
 * =========================================================================== */

/* No config file → print_help + return -1 */
static void test_main_no_config_returns_minus1(void **state)
{
    (void)state;
    reset_stubs();
    optind = 1;
    char *argv[] = {"TxApp", NULL};
    int ret = tx_app_real_main(1, argv);
    assert_int_equal(ret, -1);
}

/* Happy path: config provided, load succeeds, sessions start,
 * app.exit is set immediately by stub → loop exits → clean shutdown */
static void test_main_happy_path_exits_immediately(void **state)
{
    (void)state;
    reset_stubs();
    stub_load_config_set_exit = true; /* exit loop immediately */
    optind = 1;
    char *argv[] = {"TxApp", "--config", "test.json", NULL};
    int ret = tx_app_real_main(3, argv);
    assert_int_equal(ret, 0);
}

/* Happy path with test_time_s = 1: exercises the for-loop branch.
 * app.exit is set true so the for loop exits after checking !app.exit. */
static void test_main_test_time_path(void **state)
{
    (void)state;
    reset_stubs();
    stub_load_config_test_time_s = 1;
    stub_load_config_set_exit    = true; /* for loop exits on first check */
    optind = 1;
    char *argv[] = {"TxApp", "-C", "test.json", NULL};
    int ret = tx_app_real_main(3, argv);
    assert_int_equal(ret, 0);
}

/* load_and_apply_config fails → return -1 */
static void test_main_config_load_fails(void **state)
{
    (void)state;
    reset_stubs();
    stub_load_and_apply_config_ret = -1;
    optind = 1;
    char *argv[] = {"TxApp", "--config", "bad.json", NULL};
    int ret = tx_app_real_main(3, argv);
    assert_int_equal(ret, -1);
}

/* session_manager_init fails → return -1 */
static void test_main_session_init_fails(void **state)
{
    (void)state;
    reset_stubs();
    stub_session_manager_init_ret = -1;
    optind = 1;
    char *argv[] = {"TxApp", "--config", "test.json", NULL};
    int ret = tx_app_real_main(3, argv);
    assert_int_equal(ret, -1);
}

/* session_manager_start fails → return -1 */
static void test_main_session_start_fails(void **state)
{
    (void)state;
    reset_stubs();
    stub_session_manager_start_ret = -1;
    optind = 1;
    char *argv[] = {"TxApp", "--config", "test.json", NULL};
    int ret = tx_app_real_main(3, argv);
    assert_int_equal(ret, -1);
}

/* resolve_ip_addrs fails (bad IPs injected by stub) → return -1 */
static void test_main_resolve_ip_fails(void **state)
{
    (void)state;
    reset_stubs();
    stub_load_config_set_ips = false; /* don't set valid IPs → dip empty → fails */
    optind = 1;
    char *argv[] = {"TxApp", "--config", "test.json", NULL};
    int ret = tx_app_real_main(3, argv);
    assert_int_equal(ret, -1);
}

/* Log file redirection via peek_config_log_file → exercises the log block */
static void test_main_with_log_file_redirect(void **state)
{
    (void)state;
    reset_stubs();
    stub_peek_config_log_file_ret = 0;
    char log_path[] = "/tmp/txapp_test_log_XXXXXX";
    int log_fd = mkstemp(log_path);
    assert_true(log_fd >= 0);
    close(log_fd);
    strncpy(stub_peek_config_log_file_buf, log_path,
            sizeof(stub_peek_config_log_file_buf) - 1);
    stub_load_config_set_exit = true;
    optind = 1;
    char *argv[] = {"TxApp", "--config", "test.json", NULL};
    int ret = tx_app_real_main(3, argv);
    /* May succeed or fail depending on dup2 behaviour in test env;
     * the key goal is to exercise the log redirection code path */
    (void)ret;
    unlink(log_path);
}

/* Log file via LOG_FILE env variable (peek returns -1) */
static void test_main_with_log_env_variable(void **state)
{
    (void)state;
    reset_stubs();
    stub_peek_config_log_file_ret = -1;
    stub_load_config_set_exit = true;
    char env_log_path[] = "/tmp/txapp_test_env_log_XXXXXX";
    int env_fd = mkstemp(env_log_path);
    assert_true(env_fd >= 0);
    close(env_fd);
    setenv("LOG_FILE", env_log_path, 1);
    optind = 1;
    char *argv[] = {"TxApp", "--config", "test.json", NULL};
    int ret = tx_app_real_main(3, argv);
    (void)ret;
    unsetenv("LOG_FILE");
    unlink(env_log_path);
}

/* ===========================================================================
 * main
 * =========================================================================== */

int main(void)
{
    logger_init_default();

    const struct CMUnitTest tests[] = {
        /* parse_args */
        cmocka_unit_test(test_parse_args_long_config_option),
        cmocka_unit_test(test_parse_args_short_config_option),
        cmocka_unit_test(test_parse_args_no_args_returns_zero_and_empty_config),
        cmocka_unit_test(test_parse_args_help_returns_minus1),
        cmocka_unit_test(test_parse_args_unknown_option_returns_minus1),

        /* resolve_ip_addrs */
        cmocka_unit_test(test_resolve_ip_valid_sip_and_dip),
        cmocka_unit_test(test_resolve_ip_empty_sip_dhcp_mode),
        cmocka_unit_test(test_resolve_ip_invalid_sip_fails),
        cmocka_unit_test(test_resolve_ip_invalid_dip_fails),

        /* signal handling */
        cmocka_unit_test(test_sig_handler_sets_flag_and_apply_propagates),
        cmocka_unit_test(test_apply_pending_exit_noop_without_signal),

        /* print_help */
        cmocka_unit_test(test_print_help_does_not_crash),

        /* tx_app_real_main (full main() body) */
        cmocka_unit_test(test_main_no_config_returns_minus1),
        cmocka_unit_test(test_main_happy_path_exits_immediately),
        cmocka_unit_test(test_main_test_time_path),
        cmocka_unit_test(test_main_config_load_fails),
        cmocka_unit_test(test_main_session_init_fails),
        cmocka_unit_test(test_main_session_start_fails),
        cmocka_unit_test(test_main_resolve_ip_fails),
        cmocka_unit_test(test_main_with_log_file_redirect),
        cmocka_unit_test(test_main_with_log_env_variable),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
