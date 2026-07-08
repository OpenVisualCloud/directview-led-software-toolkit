/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 *
 * Unit tests for config_reader.c using cmocka.
 *
 * Covers:
 *   parse_tx_config()        — JSON parsing and field extraction
 *   validate_tx_config()     — semantic / bounds validation
 *   peek_config_log_file()   — lightweight log-file peek
 *
 * Build:  meson test  (or  ninja -C build test)
 * Run:    ./build/test_config_reader
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/config_reader.h"
#include "app_context.h"

/* --------------------------------------------------------------------------
 * Compile-time fixture path injected by meson.build via -DFIXTURE_DIR="..."
 * -------------------------------------------------------------------------- */
#ifndef FIXTURE_DIR
#  error "FIXTURE_DIR must be defined by the build system (-DFIXTURE_DIR=...)"
#endif

#define FIXTURE_3SESSIONS  FIXTURE_DIR "/tx_fullhd_multi_session.json"
#define FIXTURE_1SESSION   FIXTURE_DIR "/tx_fullhd_single_session.json"

/* --------------------------------------------------------------------------
 * Helper: write content to a new temp file, return heap-allocated path.
 * Caller must unlink(path) and free(path) after use.
 * -------------------------------------------------------------------------- */
static char *write_tmpfile(const char *content)
{
    char *path = strdup("/tmp/dvledtx_test_XXXXXX");
    if (!path) return NULL;
    int fd = mkstemp(path);
    if (fd < 0) { free(path); return NULL; }
    size_t len = strlen(content);
    if (write(fd, content, len) != (ssize_t)len) {
        close(fd); unlink(path); free(path); return NULL;
    }
    close(fd);
    return path;
}

/* --------------------------------------------------------------------------
 * Helper: populate a fully-valid dvledtx_config (no tx_url so the
 * file-existence check inside validate_tx_config is skipped).
 * -------------------------------------------------------------------------- */
static void fill_valid_config(struct dvledtx_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    /* Allocate dynamic arrays */
    cfg->nic_cap = 1;
    cfg->interface_name = calloc(1, sizeof(*cfg->interface_name));
    cfg->interface_sip  = calloc(1, sizeof(*cfg->interface_sip));
    cfg->interface_dip  = calloc(1, sizeof(*cfg->interface_dip));
    cfg->nic_count = 1;

    strncpy(cfg->interface_name[0], "0000:06:00.0",   sizeof(cfg->interface_name[0]) - 1);
    strncpy(cfg->interface_sip[0],  "192.168.50.29",  sizeof(cfg->interface_sip[0])  - 1);
    strncpy(cfg->interface_dip[0],  "239.168.85.20",  sizeof(cfg->interface_dip[0])  - 1);
    cfg->width  = 1920;
    cfg->height = 1080;
    cfg->fps    = 30;
    strncpy(cfg->fmt, "yuv422p10le", sizeof(cfg->fmt) - 1);
    /* tx_url intentionally left empty — skips file-open check in validate */
    cfg->session_cap = 1;
    cfg->sessions = calloc(1, sizeof(*cfg->sessions));
    cfg->session_count = 1;
    cfg->sessions[0].udp_port     = 20000;
    cfg->sessions[0].payload_type = 96;
    cfg->sessions[0].crop_x = 0;
    cfg->sessions[0].crop_y = 0;
    cfg->sessions[0].crop_w = 1920;
    cfg->sessions[0].crop_h = 1080;
}

/* ==========================================================================
 * parse_tx_config tests
 * ========================================================================== */

static void test_parse_returns_minus1_for_null_path(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    /* fopen(NULL) returns NULL on Linux/glibc; logger is a no-op when
     * uninitialized, so this is safe to call. */
    assert_int_equal(parse_tx_config(NULL, &cfg), -1);
}

static void test_parse_returns_minus1_for_missing_file(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    assert_int_equal(
        parse_tx_config("/tmp/dvledtx_no_such_file_xyz.json", &cfg), -1);
}

static void test_parse_3sessions_session_count(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    assert_int_equal(parse_tx_config(FIXTURE_3SESSIONS, &cfg), 0);
    assert_int_equal(cfg.session_count, 3);
}

static void test_parse_1session_session_count(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    assert_int_equal(parse_tx_config(FIXTURE_1SESSION, &cfg), 0);
    assert_int_equal(cfg.session_count, 1);
}

static void test_parse_3sessions_interface_fields(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    assert_int_equal(parse_tx_config(FIXTURE_3SESSIONS, &cfg), 0);
    assert_string_equal(cfg.interface_name[0], "0000:03:10.1");
    assert_string_equal(cfg.interface_sip[0],  "192.168.50.29");
    assert_string_equal(cfg.interface_dip[0],  "239.168.85.21");
}

static void test_parse_3sessions_video_params(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    assert_int_equal(parse_tx_config(FIXTURE_3SESSIONS, &cfg), 0);
    assert_int_equal((int)cfg.width,  1920);
    assert_int_equal((int)cfg.height, 1080);
    assert_int_equal(cfg.fps,         30);
    assert_string_equal(cfg.fmt, "yuv422p10le");
}

static void test_parse_3sessions_log_file(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    assert_int_equal(parse_tx_config(FIXTURE_3SESSIONS, &cfg), 0);
    assert_string_equal(cfg.log_file, "dvledtx.log");
}

static void test_parse_3sessions_session0_crop(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    assert_int_equal(parse_tx_config(FIXTURE_3SESSIONS, &cfg), 0);

    const struct tx_session_config *s = &cfg.sessions[0];
    assert_int_equal(s->udp_port,     20000);
    assert_int_equal(s->payload_type, 96);
    assert_int_equal(s->crop_x, 0);
    assert_int_equal(s->crop_y, 0);
    assert_int_equal(s->crop_w, 640);
    assert_int_equal(s->crop_h, 1080);
}

static void test_parse_3sessions_session1_crop(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    assert_int_equal(parse_tx_config(FIXTURE_3SESSIONS, &cfg), 0);

    const struct tx_session_config *s = &cfg.sessions[1];
    assert_int_equal(s->udp_port,     20002);
    assert_int_equal(s->payload_type, 96);
    assert_int_equal(s->crop_x, 640);
    assert_int_equal(s->crop_y, 0);
    assert_int_equal(s->crop_w, 640);
    assert_int_equal(s->crop_h, 1080);
}

static void test_parse_3sessions_session2_crop(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    assert_int_equal(parse_tx_config(FIXTURE_3SESSIONS, &cfg), 0);

    const struct tx_session_config *s = &cfg.sessions[2];
    assert_int_equal(s->udp_port,     20004);
    assert_int_equal(s->payload_type, 96);
    assert_int_equal(s->crop_x, 1280);
    assert_int_equal(s->crop_y, 0);
    assert_int_equal(s->crop_w, 640);
    assert_int_equal(s->crop_h, 1080);
}

static void test_parse_returns_minus1_when_sessions_key_absent(void **state)
{
    (void)state;
    char *path = write_tmpfile(
        "{"
        "  \"interfaces\": [{\"name\":\"eth0\",\"sip\":\"1.2.3.4\",\"dip\":\"5.6.7.8\"}],"
        "  \"video\": {\"width\":1920,\"height\":1080,\"fps\":25,\"fmt\":\"yuv422p10le\"}"
        "}");
    assert_non_null(path);

    struct dvledtx_config cfg;
    int ret = parse_tx_config(path, &cfg);
    unlink(path);
    free(path);
    assert_int_equal(ret, -1);
}

static void test_parse_returns_minus1_when_sessions_array_empty(void **state)
{
    (void)state;
    char *path = write_tmpfile(
        "{"
        "  \"interfaces\": [{\"name\":\"eth0\",\"sip\":\"1.2.3.4\",\"dip\":\"5.6.7.8\"}],"
        "  \"video\": {\"width\":1920,\"height\":1080,\"fps\":25,\"fmt\":\"yuv422p10le\"},"
        "  \"tx_sessions\": []"
        "}");
    assert_non_null(path);

    struct dvledtx_config cfg;
    int ret = parse_tx_config(path, &cfg);
    unlink(path);
    free(path);
    assert_int_equal(ret, -1);
}

static void test_parse_returns_zero_fields_when_video_missing(void **state)
{
    (void)state;
    /* Config with no "video" block — fields remain zero from memset */
    char *path = write_tmpfile(
        "{"
        "  \"interfaces\": [{\"name\":\"eth0\",\"sip\":\"1.2.3.4\",\"dip\":\"5.6.7.8\"}],"
        "  \"tx_sessions\": [{\"udp_port\":20000,\"payload_type\":96,"
        "    \"crop\":{\"x\":0,\"y\":0,\"w\":1920,\"h\":1080}}]"
        "}");
    assert_non_null(path);

    struct dvledtx_config cfg;
    int ret = parse_tx_config(path, &cfg);
    unlink(path);
    free(path);
    assert_int_equal(ret, 0);
    assert_int_equal((int)cfg.width,  0);
    assert_int_equal((int)cfg.height, 0);
    assert_int_equal(cfg.fps, 0);
}

/* ==========================================================================
 * validate_tx_config tests
 * ========================================================================== */

static void test_validate_valid_config_passes(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    assert_int_equal(validate_tx_config(&cfg), 0);
    dvledtx_config_free(&cfg);
}

static void test_validate_missing_interface_name_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.interface_name[0][0] = '\0';
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_missing_dip_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.interface_dip[0][0] = '\0';
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_invalid_sip_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    strncpy(cfg.interface_sip[0], "not.an.ip.address", sizeof(cfg.interface_sip[0]) - 1);
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_invalid_dip_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    strncpy(cfg.interface_dip[0], "999.999.999.999", sizeof(cfg.interface_dip[0]) - 1);
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_zero_width_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.width = 0;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_zero_height_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.height = 0;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_odd_width_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.width = 1921;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_supported_fps_values_all_pass(void **state)
{
    (void)state;
    const int valid_fps[] = {25, 30, 50, 60};
    for (size_t i = 0; i < sizeof(valid_fps) / sizeof(valid_fps[0]); i++) {
        struct dvledtx_config cfg;
        fill_valid_config(&cfg);
        cfg.fps = valid_fps[i];
        assert_int_equal(validate_tx_config(&cfg), 0);
        dvledtx_config_free(&cfg);
    }
}

static void test_validate_unsupported_fps_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.fps = 24;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_unsupported_fmt_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    strncpy(cfg.fmt, "rgb24", sizeof(cfg.fmt) - 1);
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_all_supported_fmts_pass(void **state)
{
    (void)state;
    const char *fmts[] = {"yuv422p10le", "yuv420", "yuv444p10le", "gbrp10le",
                          "yuv422p12le", "yuv444p12le", "gbrp12le"};
    for (size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        struct dvledtx_config cfg;
        fill_valid_config(&cfg);
        strncpy(cfg.fmt, fmts[i], sizeof(cfg.fmt) - 1);
        assert_int_equal(validate_tx_config(&cfg), 0);
        dvledtx_config_free(&cfg);
    }
}

static void test_validate_zero_udp_port_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.sessions[0].udp_port = 0;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_payload_type_below_96_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.sessions[0].payload_type = 95;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_payload_type_above_127_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.sessions[0].payload_type = 128;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_payload_type_boundary_96_passes(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.sessions[0].payload_type = 96;
    assert_int_equal(validate_tx_config(&cfg), 0);
    dvledtx_config_free(&cfg);
}

static void test_validate_payload_type_boundary_127_passes(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.sessions[0].payload_type = 127;
    assert_int_equal(validate_tx_config(&cfg), 0);
    dvledtx_config_free(&cfg);
}

static void test_validate_crop_x_plus_w_exceeds_width_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.sessions[0].crop_x = 100;
    cfg.sessions[0].crop_w = 1920; /* 100 + 1920 = 2020 > 1920 */
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_crop_y_plus_h_exceeds_height_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.sessions[0].crop_y = 100;
    cfg.sessions[0].crop_h = 1080; /* 100 + 1080 = 1180 > 1080 */
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_odd_crop_w_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.sessions[0].crop_w = 641;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_zero_session_count_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.session_count = 0;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_3sessions_tiled_layout_passes(void **state)
{
    (void)state;
    /* Mirrors tx_fullhd_multi_session.json: three 640-wide horizontal tiles */
    struct dvledtx_config cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.nic_cap = 1;
    cfg.interface_name = calloc(1, sizeof(*cfg.interface_name));
    cfg.interface_sip  = calloc(1, sizeof(*cfg.interface_sip));
    cfg.interface_dip  = calloc(1, sizeof(*cfg.interface_dip));
    cfg.nic_count = 1;

    strncpy(cfg.interface_name[0], "0000:06:00.0",   sizeof(cfg.interface_name[0]) - 1);
    strncpy(cfg.interface_sip[0],  "192.168.50.29",  sizeof(cfg.interface_sip[0])  - 1);
    strncpy(cfg.interface_dip[0],  "239.168.85.20",  sizeof(cfg.interface_dip[0])  - 1);
    cfg.width  = 1920;
    cfg.height = 1080;
    cfg.fps    = 30;
    strncpy(cfg.fmt, "yuv420", sizeof(cfg.fmt) - 1);

    cfg.session_cap = 3;
    cfg.sessions = calloc(3, sizeof(*cfg.sessions));
    cfg.session_count = 3;

    const uint16_t ports[] = {20000, 20002, 20004};
    const int      xs[]    = {0,     640,   1280};
    for (int i = 0; i < 3; i++) {
        cfg.sessions[i].udp_port     = ports[i];
        cfg.sessions[i].payload_type = 96;
        cfg.sessions[i].crop_x = xs[i];
        cfg.sessions[i].crop_y = 0;
        cfg.sessions[i].crop_w = 640;
        cfg.sessions[i].crop_h = 1080;
    }
    assert_int_equal(validate_tx_config(&cfg), 0);
    dvledtx_config_free(&cfg);
}

/* ==========================================================================
 * peek_config_log_file tests
 * ========================================================================== */

static void test_peek_log_file_returns_value_from_3sessions(void **state)
{
    (void)state;
    char buf[256] = {0};
    assert_int_equal(peek_config_log_file(FIXTURE_3SESSIONS, buf, sizeof(buf)), 0);
    assert_string_equal(buf, "dvledtx.log");
}

static void test_peek_log_file_returns_minus1_when_field_absent(void **state)
{
    (void)state;
    char *path = write_tmpfile(
        "{\"interfaces\":[],\"video\":{},\"tx_sessions\":[]}");
    assert_non_null(path);

    char buf[256] = {0};
    int ret = peek_config_log_file(path, buf, sizeof(buf));
    unlink(path);
    free(path);
    assert_int_equal(ret, -1);
}

static void test_peek_log_file_returns_minus1_for_nonexistent_file(void **state)
{
    (void)state;
    char buf[256] = {0};
    assert_int_equal(
        peek_config_log_file("/tmp/dvledtx_no_such_file_xyz.json", buf, sizeof(buf)), -1);
}

static void test_peek_log_file_fills_buffer_correctly(void **state)
{
    (void)state;
    char *path = write_tmpfile("{\"log_file\": \"my_custom.log\"}");
    assert_non_null(path);

    char buf[256] = {0};
    int ret = peek_config_log_file(path, buf, sizeof(buf));
    unlink(path);
    free(path);
    assert_int_equal(ret, 0);
    assert_string_equal(buf, "my_custom.log");
}

/* ==========================================================================
 * parse_tx_config — default/fallback paths for missing/invalid session fields
 * ========================================================================== */

static void test_parse_session_missing_udp_port_fails(void **state)
{
    (void)state;
    char *path = write_tmpfile(
        "{"
        "  \"interfaces\": [{\"name\":\"eth0\",\"sip\":\"1.2.3.4\",\"dip\":\"5.6.7.8\"}],"
        "  \"video\": {\"width\":1920,\"height\":1080,\"fps\":25,\"fmt\":\"yuv422p10le\"},"
        "  \"tx_sessions\": [{\"payload_type\":96,"
        "    \"crop\":{\"x\":0,\"y\":0,\"w\":1920,\"h\":1080}}]"
        "}");
    assert_non_null(path);
    struct dvledtx_config cfg;
    int ret = parse_tx_config(path, &cfg);
    unlink(path); free(path);
    assert_int_equal(ret, -1);
}

static void test_parse_session_udp_port_exceeds_65535_fails(void **state)
{
    (void)state;
    char *path = write_tmpfile(
        "{"
        "  \"interfaces\": [{\"name\":\"eth0\",\"sip\":\"1.2.3.4\",\"dip\":\"5.6.7.8\"}],"
        "  \"video\": {\"width\":1920,\"height\":1080,\"fps\":25,\"fmt\":\"yuv422p10le\"},"
        "  \"tx_sessions\": [{\"udp_port\":70000,\"payload_type\":96,"
        "    \"crop\":{\"x\":0,\"y\":0,\"w\":1920,\"h\":1080}}]"
        "}");
    assert_non_null(path);
    struct dvledtx_config cfg;
    int ret = parse_tx_config(path, &cfg);
    unlink(path); free(path);
    assert_int_equal(ret, -1);
}

static void test_parse_session_missing_payload_type_defaults_to_96(void **state)
{
    (void)state;
    char *path = write_tmpfile(
        "{"
        "  \"interfaces\": [{\"name\":\"eth0\",\"sip\":\"1.2.3.4\",\"dip\":\"5.6.7.8\"}],"
        "  \"video\": {\"width\":1920,\"height\":1080,\"fps\":25,\"fmt\":\"yuv422p10le\"},"
        "  \"tx_sessions\": [{\"udp_port\":20000,"
        "    \"crop\":{\"x\":0,\"y\":0,\"w\":1920,\"h\":1080}}]"
        "}");
    assert_non_null(path);
    struct dvledtx_config cfg;
    int ret = parse_tx_config(path, &cfg);
    unlink(path); free(path);
    assert_int_equal(ret, 0);
    assert_int_equal(cfg.sessions[0].payload_type, 96);
}

static void test_parse_session_no_crop_object_fails(void **state)
{
    (void)state;
    char *path = write_tmpfile(
        "{"
        "  \"interfaces\": [{\"name\":\"eth0\",\"sip\":\"1.2.3.4\",\"dip\":\"5.6.7.8\"}],"
        "  \"video\": {\"width\":1920,\"height\":1080,\"fps\":25,\"fmt\":\"yuv422p10le\"},"
        "  \"tx_sessions\": [{\"udp_port\":20000,\"payload_type\":96}]"
        "}");
    assert_non_null(path);
    struct dvledtx_config cfg;
    int ret = parse_tx_config(path, &cfg);
    unlink(path); free(path);
    assert_int_equal(ret, -1);
}

/* ==========================================================================
 * validate_tx_config — additional failure / edge-case paths
 * ========================================================================== */

static void test_validate_resolution_exceeds_max_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.width = 4000; /* > 3840 limit */
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_2k_resolution_passes(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.width  = 2560;
    cfg.height = 1440;
    cfg.sessions[0].crop_w = 2560;
    cfg.sessions[0].crop_h = 1440;
    assert_int_equal(validate_tx_config(&cfg), 0);
    dvledtx_config_free(&cfg);
}

static void test_validate_4k_resolution_passes(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.width  = 3840;
    cfg.height = 2160;
    cfg.sessions[0].crop_w = 3840;
    cfg.sessions[0].crop_h = 2160;
    assert_int_equal(validate_tx_config(&cfg), 0);
    dvledtx_config_free(&cfg);
}

static void test_validate_4k_height_exceeds_max_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.width  = 3840;
    cfg.height = 2162; /* > 2160 limit */
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

/* ==========================================================================
 * validate_tx_config — scaling tests
 * ========================================================================== */

static void test_validate_scale_upscale_1080p_to_4k_passes(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.width  = 1920;
    cfg.height = 1080;
    cfg.scale_width  = 3840;
    cfg.scale_height = 2160;
    cfg.sessions[0].crop_w = 3840;
    cfg.sessions[0].crop_h = 2160;
    assert_int_equal(validate_tx_config(&cfg), 0);
    dvledtx_config_free(&cfg);
}

static void test_validate_scale_downscale_4k_to_1080p_passes(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.width  = 3840;
    cfg.height = 2160;
    cfg.scale_width  = 1920;
    cfg.scale_height = 1080;
    cfg.sessions[0].crop_w = 1920;
    cfg.sessions[0].crop_h = 1080;
    assert_int_equal(validate_tx_config(&cfg), 0);
    dvledtx_config_free(&cfg);
}

static void test_validate_scale_width_only_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.scale_width  = 3840;
    cfg.scale_height = 0;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_scale_height_only_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.scale_width  = 0;
    cfg.scale_height = 2160;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_scale_exceeds_max_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.scale_width  = 4096;
    cfg.scale_height = 2160;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_scale_odd_width_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.scale_width  = 1921;
    cfg.scale_height = 1080;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_scale_odd_height_yuv420_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    strncpy(cfg.fmt, "yuv420", sizeof(cfg.fmt) - 1);
    cfg.scale_width  = 1920;
    cfg.scale_height = 1081; /* yuv420 requires even height */
    cfg.sessions[0].crop_w = 1920;
    cfg.sessions[0].crop_h = 1080;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_scale_crop_exceeds_scaled_dims_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.width  = 3840;
    cfg.height = 2160;
    cfg.scale_width  = 1920;
    cfg.scale_height = 1080;
    /* crop is 3840x2160 but effective dims are 1920x1080 */
    cfg.sessions[0].crop_w = 3840;
    cfg.sessions[0].crop_h = 2160;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_scale_crop_within_scaled_dims_passes(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.width  = 3840;
    cfg.height = 2160;
    cfg.scale_width  = 2560;
    cfg.scale_height = 1440;
    cfg.sessions[0].crop_w = 2560;
    cfg.sessions[0].crop_h = 1440;
    assert_int_equal(validate_tx_config(&cfg), 0);
    dvledtx_config_free(&cfg);
}

static void test_validate_no_scale_passes(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.scale_width  = 0;
    cfg.scale_height = 0;
    assert_int_equal(validate_tx_config(&cfg), 0);
    dvledtx_config_free(&cfg);
}

static void test_validate_duplicate_udp_ports_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.nic_cap = 1;
    cfg.interface_name = calloc(1, sizeof(*cfg.interface_name));
    cfg.interface_sip  = calloc(1, sizeof(*cfg.interface_sip));
    cfg.interface_dip  = calloc(1, sizeof(*cfg.interface_dip));
    cfg.nic_count = 1;

    strncpy(cfg.interface_name[0], "0000:06:00.0", sizeof(cfg.interface_name[0]) - 1);
    strncpy(cfg.interface_sip[0],  "192.168.1.1",  sizeof(cfg.interface_sip[0])  - 1);
    strncpy(cfg.interface_dip[0],  "239.0.0.1",    sizeof(cfg.interface_dip[0])  - 1);
    cfg.width = 1920; cfg.height = 1080; cfg.fps = 25;
    strncpy(cfg.fmt, "yuv422p10le", sizeof(cfg.fmt) - 1);

    cfg.session_cap = 2;
    cfg.sessions = calloc(2, sizeof(*cfg.sessions));
    cfg.session_count = 2;
    cfg.sessions[0].udp_port = 20000; cfg.sessions[0].payload_type = 96;
    cfg.sessions[0].crop_x = 0;   cfg.sessions[0].crop_y = 0;
    cfg.sessions[0].crop_w = 960; cfg.sessions[0].crop_h = 1080;
    cfg.sessions[1].udp_port = 20000; /* same port — duplicate */
    cfg.sessions[1].payload_type = 97;
    cfg.sessions[1].crop_x = 960; cfg.sessions[1].crop_y = 0;
    cfg.sessions[1].crop_w = 960; cfg.sessions[1].crop_h = 1080;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_crop_x_misaligned_for_yuv422_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    /* yuv422p10le has x_align=2; crop_x=1 is not a multiple of 2 */
    cfg.sessions[0].crop_x = 1;
    cfg.sessions[0].crop_w = 1918; /* crop_x + crop_w = 1919 < 1920; crop_w even */
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_tx_url_nonexistent_file_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    strncpy(cfg.tx_url, "/tmp/dvledtx_no_such_video_xyz.mp4", sizeof(cfg.tx_url) - 1);
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_tx_url_existing_file_passes(void **state)
{
    (void)state;
    char *path = write_tmpfile("dummy video placeholder");
    assert_non_null(path);
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    strncpy(cfg.tx_url, path, sizeof(cfg.tx_url) - 1);
    int ret = validate_tx_config(&cfg);
    unlink(path); free(path);
    dvledtx_config_free(&cfg);
    assert_int_equal(ret, 0);
}

/* ==========================================================================
 * Security validation tests (STRIDE mitigations)
 * ========================================================================== */

/* D-3: DIP must be in multicast range 224.0.0.0/4 */
static void test_validate_non_multicast_dip_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    strncpy(cfg.interface_dip[0], "192.168.1.100", sizeof(cfg.interface_dip[0]) - 1);
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

/* S-2: PCI BDF must match DDDD:DD:DD.D hex pattern */
static void test_validate_invalid_bdf_format_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    strncpy(cfg.interface_name[0], "eth0", sizeof(cfg.interface_name[0]) - 1);
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_valid_bdf_passes(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    strncpy(cfg.interface_name[0], "0000:af:00.1", sizeof(cfg.interface_name[0]) - 1);
    assert_int_equal(validate_tx_config(&cfg), 0);
    dvledtx_config_free(&cfg);
}

/* D-3: UDP port must be >= 1024 (above privileged range) */
static void test_validate_privileged_udp_port_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.sessions[0].udp_port = 80;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

/* ==========================================================================
 * JSON string escape sequences (extract_json_string handling)
 * Exercised via peek_config_log_file which extracts the "log_file" string.
 * ========================================================================== */

/* \\\" inside a JSON string must decode to a literal double-quote */
static void test_peek_log_file_handles_escaped_quote(void **state)
{
    (void)state;
    char *path = write_tmpfile("{\"log_file\": \"my\\\"file.log\"}");
    assert_non_null(path);
    char buf[256] = {0};
    int ret = peek_config_log_file(path, buf, sizeof(buf));
    unlink(path); free(path);
    assert_int_equal(ret, 0);
    assert_string_equal(buf, "my\"file.log");
}

/* \\\\ must decode to a single literal backslash */
static void test_peek_log_file_handles_escaped_backslash(void **state)
{
    (void)state;
    char *path = write_tmpfile("{\"log_file\": \"a\\\\b.log\"}");
    assert_non_null(path);
    char buf[256] = {0};
    int ret = peek_config_log_file(path, buf, sizeof(buf));
    unlink(path); free(path);
    assert_int_equal(ret, 0);
    assert_string_equal(buf, "a\\b.log");
}

/* \\/ must decode to a literal forward slash (used for path separators) */
static void test_peek_log_file_handles_escaped_forward_slash(void **state)
{
    (void)state;
    char *path = write_tmpfile("{\"log_file\": \"\\/tmp\\/x.log\"}");
    assert_non_null(path);
    char buf[256] = {0};
    int ret = peek_config_log_file(path, buf, sizeof(buf));
    unlink(path); free(path);
    assert_int_equal(ret, 0);
    assert_string_equal(buf, "/tmp/x.log");
}

/* \\n must decode to a literal newline character */
static void test_peek_log_file_handles_escaped_newline(void **state)
{
    (void)state;
    char *path = write_tmpfile("{\"log_file\": \"line1\\nline2\"}");
    assert_non_null(path);
    char buf[256] = {0};
    int ret = peek_config_log_file(path, buf, sizeof(buf));
    unlink(path); free(path);
    assert_int_equal(ret, 0);
    assert_string_equal(buf, "line1\nline2");
}

/* \\t must decode to a literal tab character */
static void test_peek_log_file_handles_escaped_tab(void **state)
{
    (void)state;
    char *path = write_tmpfile("{\"log_file\": \"a\\tb\"}");
    assert_non_null(path);
    char buf[256] = {0};
    int ret = peek_config_log_file(path, buf, sizeof(buf));
    unlink(path); free(path);
    assert_int_equal(ret, 0);
    assert_string_equal(buf, "a\tb");
}

/* Unknown escape sequence (e.g. \\z) keeps the literal escaped character */
static void test_peek_log_file_unknown_escape_kept_literal(void **state)
{
    (void)state;
    char *path = write_tmpfile("{\"log_file\": \"x\\zy.log\"}");
    assert_non_null(path);
    char buf[256] = {0};
    int ret = peek_config_log_file(path, buf, sizeof(buf));
    unlink(path); free(path);
    assert_int_equal(ret, 0);
    assert_string_equal(buf, "xzy.log");
}

/* Bare control characters (e.g. raw 0x01) inside the JSON string are stripped */
static void test_peek_log_file_strips_bare_control_chars(void **state)
{
    (void)state;
    /* Embed an SOH (0x01) byte between 'a' and 'b' */
    const char json[] = "{\"log_file\": \"a\x01""b.log\"}";
    char *path = strdup("/tmp/dvledtx_ctrl_XXXXXX");
    assert_non_null(path);
    int fd = mkstemp(path);
    assert_true(fd >= 0);
    assert_int_equal(write(fd, json, sizeof(json) - 1), (ssize_t)(sizeof(json) - 1));
    close(fd);

    char buf[256] = {0};
    int ret = peek_config_log_file(path, buf, sizeof(buf));
    unlink(path); free(path);
    assert_int_equal(ret, 0);
    assert_string_equal(buf, "ab.log");
}

/* ==========================================================================
 * peek_config_log_file — NULL guards
 * ========================================================================== */

static void test_peek_log_file_null_path_returns_minus1(void **state)
{
    (void)state;
    char buf[64] = {0};
    assert_int_equal(peek_config_log_file(NULL, buf, sizeof(buf)), -1);
}

static void test_peek_log_file_null_buffer_returns_minus1(void **state)
{
    (void)state;
    assert_int_equal(peek_config_log_file(FIXTURE_3SESSIONS, NULL, 64), -1);
}

/* ==========================================================================
 * Crop bounds — overflow safety with very large crop values
 * (Tests the unsigned-cast addition introduced for STRIDE mitigation.)
 * ========================================================================== */

static void test_validate_crop_x_plus_w_unsigned_overflow_safe(void **state)
{
    (void)state;
    /* With signed int math, INT_MAX + 1 wraps to INT_MIN and would silently
     * pass the comparison.  The unsigned cast must catch this. */
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.sessions[0].crop_x = 2147483647; /* INT_MAX */
    cfg.sessions[0].crop_w = 2;          /* even, so passes alignment */
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

static void test_validate_crop_y_plus_h_unsigned_overflow_safe(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.sessions[0].crop_y = 2147483647; /* INT_MAX */
    cfg.sessions[0].crop_h = 2;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

/* D-3: 224.x.x.x is in the multicast range — passes (with a warning) */
static void test_validate_low_multicast_dip_passes_with_warning(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    strncpy(cfg.interface_dip[0], "224.0.0.50", sizeof(cfg.interface_dip[0]) - 1);
    /* In multicast range but outside the 239.0.0.0/8 administratively-scoped
     * range — must still validate (a warning is logged). */
    assert_int_equal(validate_tx_config(&cfg), 0);
    dvledtx_config_free(&cfg);
}

/* D-3: 240.x.x.x is OUTSIDE the multicast range (224.0.0.0/4) — must fail */
static void test_validate_above_multicast_dip_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    strncpy(cfg.interface_dip[0], "240.0.0.1", sizeof(cfg.interface_dip[0]) - 1);
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

/* D-3: UDP port at the boundary (1024) must pass */
static void test_validate_udp_port_boundary_1024_passes(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.sessions[0].udp_port = 1024;
    assert_int_equal(validate_tx_config(&cfg), 0);
    dvledtx_config_free(&cfg);
}

/* D-3: UDP port 1023 (just below boundary) must fail */
static void test_validate_udp_port_boundary_1023_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    cfg.sessions[0].udp_port = 1023;
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

/* S-2: BDF with too few digits in domain part must fail */
static void test_validate_short_bdf_format_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    /* Missing leading "00" in domain — DDD:DD:DD.D instead of DDDD:DD:DD.D */
    strncpy(cfg.interface_name[0], "000:06:00.0", sizeof(cfg.interface_name[0]) - 1);
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

/* S-2: BDF with non-hex chars must fail */
static void test_validate_nonhex_bdf_format_fails(void **state)
{
    (void)state;
    struct dvledtx_config cfg;
    fill_valid_config(&cfg);
    strncpy(cfg.interface_name[0], "ZZZZ:06:00.0", sizeof(cfg.interface_name[0]) - 1);
    assert_int_equal(validate_tx_config(&cfg), -1);
    dvledtx_config_free(&cfg);
}

/* ==========================================================================
 * parse_tx_config — additional negative-value crop fields must be rejected
 * (crop fields are now mandatory; missing/invalid values cause parse to fail)
 * ========================================================================== */

static void test_parse_session_negative_crop_x_fails(void **state)
{
    (void)state;
    char *path = write_tmpfile(
        "{"
        "  \"interfaces\": [{\"name\":\"eth0\",\"sip\":\"1.2.3.4\",\"dip\":\"5.6.7.8\"}],"
        "  \"video\": {\"width\":1920,\"height\":1080,\"fps\":25,\"fmt\":\"yuv422p10le\"},"
        "  \"tx_sessions\": [{\"udp_port\":20000,\"payload_type\":96,"
        "    \"crop\":{\"y\":0,\"w\":1920,\"h\":1080}}]"
        "}");
    assert_non_null(path);
    struct dvledtx_config cfg;
    int ret = parse_tx_config(path, &cfg);
    unlink(path); free(path);
    /* Missing crop "x" — extract_json_int returns -1, parse must fail */
    assert_int_equal(ret, -1);
}

static void test_parse_session_zero_crop_w_fails(void **state)
{
    (void)state;
    char *path = write_tmpfile(
        "{"
        "  \"interfaces\": [{\"name\":\"eth0\",\"sip\":\"1.2.3.4\",\"dip\":\"5.6.7.8\"}],"
        "  \"video\": {\"width\":1920,\"height\":1080,\"fps\":25,\"fmt\":\"yuv422p10le\"},"
        "  \"tx_sessions\": [{\"udp_port\":20000,\"payload_type\":96,"
        "    \"crop\":{\"x\":0,\"y\":0,\"w\":0,\"h\":1080}}]"
        "}");
    assert_non_null(path);
    struct dvledtx_config cfg;
    int ret = parse_tx_config(path, &cfg);
    unlink(path); free(path);
    assert_int_equal(ret, -1);
}

/* ==========================================================================
 * load_and_apply_config
 * ========================================================================== */

static void test_load_and_apply_config_null_app_returns_error(void **state)
{
    (void)state;
    assert_int_equal(load_and_apply_config(NULL, "any_file.json"), -1);
}

static void test_load_and_apply_config_null_file_returns_zero(void **state)
{
    (void)state;
    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    assert_int_equal(load_and_apply_config(&app, NULL), 0);
}

static void test_load_and_apply_config_empty_file_returns_zero(void **state)
{
    (void)state;
    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    assert_int_equal(load_and_apply_config(&app, ""), 0);
}

static void test_load_and_apply_config_nonexistent_file_returns_minus1(void **state)
{
    (void)state;
    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    assert_int_equal(
        load_and_apply_config(&app, "/tmp/dvledtx_no_such_config_xyz.json"), -1);
}

/* Minimal valid one-session JSON (no tx_url, file-open check skipped) */
#define ONE_SESSION_JSON(fmt_str) \
    "{" \
    "  \"interfaces\": [{\"name\":\"0000:06:00.0\"," \
    "    \"sip\":\"192.168.50.29\",\"dip\":\"239.168.85.20\"}]," \
    "  \"video\": {\"width\":1920,\"height\":1080}," \
    "  \"tx_video\": {\"fps\":30,\"fmt\":\"" fmt_str "\"}," \
    "  \"tx_sessions\": [{\"udp_port\":20000,\"payload_type\":96," \
    "    \"crop\":{\"x\":0,\"y\":0,\"w\":1920,\"h\":1080}}]" \
    "}"

static void test_load_and_apply_config_populates_app_context(void **state)
{
    (void)state;
    char *path = write_tmpfile(ONE_SESSION_JSON("yuv422p10le"));
    assert_non_null(path);
    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    int ret = load_and_apply_config(&app, path);
    unlink(path); free(path);
    assert_int_equal(ret, 0);
    assert_string_equal(app.nics[0].port, "0000:06:00.0");
    assert_int_equal((int)app.width,  1920);
    assert_int_equal((int)app.height, 1080);
    assert_int_equal(app.fps, 30);
    assert_int_equal(app.fmt, AV_PIX_FMT_YUV422P10LE);
    assert_int_equal(app.st20p_sessions, 1);
    assert_int_equal(app.session_net[0].udp_port, 20000);
    assert_int_equal(app.session_net[0].payload_type, 96);
    dvledtx_context_free(&app);
}

static void test_load_and_apply_config_fmt_yuv444p(void **state)
{
    (void)state;
    char *path = write_tmpfile(ONE_SESSION_JSON("yuv444p10le"));
    assert_non_null(path);
    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    int ret = load_and_apply_config(&app, path);
    unlink(path); free(path);
    assert_int_equal(ret, 0);
    assert_int_equal(app.fmt, AV_PIX_FMT_YUV444P10LE);
    dvledtx_context_free(&app);
}

static void test_load_and_apply_config_fmt_gbrp10le(void **state)
{
    (void)state;
    char *path = write_tmpfile(ONE_SESSION_JSON("gbrp10le"));
    assert_non_null(path);
    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    int ret = load_and_apply_config(&app, path);
    unlink(path); free(path);
    assert_int_equal(ret, 0);
    assert_int_equal(app.fmt, AV_PIX_FMT_GBRP10LE);
    dvledtx_context_free(&app);
}

static void test_load_and_apply_config_fmt_yuv420(void **state)
{
    (void)state;
    char *path = write_tmpfile(ONE_SESSION_JSON("yuv420"));
    assert_non_null(path);
    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    int ret = load_and_apply_config(&app, path);
    unlink(path); free(path);
    assert_int_equal(ret, 0);
    assert_int_equal(app.fmt, AV_PIX_FMT_YUV420P);
    dvledtx_context_free(&app);
}

static void test_load_and_apply_config_unknown_fmt_fails(void **state)
{
    (void)state;
    char *path = write_tmpfile(ONE_SESSION_JSON("rgb24"));
    assert_non_null(path);
    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    int ret = load_and_apply_config(&app, path);
    unlink(path); free(path);
    assert_int_equal(ret, -1);
}

static void test_load_and_apply_config_copies_log_file(void **state)
{
    (void)state;
    char *path = write_tmpfile(
        "{"
        "  \"log_file\": \"myapp.log\","
        "  \"interfaces\": [{\"name\":\"0000:06:00.0\",\"sip\":\"192.168.50.29\",\"dip\":\"239.168.85.20\"}],"
        "  \"video\": {\"width\":1920,\"height\":1080},"
        "  \"tx_video\": {\"fps\":25,\"fmt\":\"yuv422p10le\"},"
        "  \"tx_sessions\": [{\"udp_port\":20000,\"payload_type\":96,"
        "    \"crop\":{\"x\":0,\"y\":0,\"w\":1920,\"h\":1080}}]"
        "}");
    assert_non_null(path);
    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    int ret = load_and_apply_config(&app, path);
    unlink(path); free(path);
    assert_int_equal(ret, 0);
    assert_string_equal(app.log_file, "myapp.log");
    dvledtx_context_free(&app);
}

/* ==========================================================================
 * main
 * ========================================================================== */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* --- parse_tx_config --- */
        cmocka_unit_test(test_parse_returns_minus1_for_null_path),
        cmocka_unit_test(test_parse_returns_minus1_for_missing_file),
        cmocka_unit_test(test_parse_3sessions_session_count),
        cmocka_unit_test(test_parse_1session_session_count),
        cmocka_unit_test(test_parse_3sessions_interface_fields),
        cmocka_unit_test(test_parse_3sessions_video_params),
        cmocka_unit_test(test_parse_3sessions_log_file),
        cmocka_unit_test(test_parse_3sessions_session0_crop),
        cmocka_unit_test(test_parse_3sessions_session1_crop),
        cmocka_unit_test(test_parse_3sessions_session2_crop),
        cmocka_unit_test(test_parse_returns_minus1_when_sessions_key_absent),
        cmocka_unit_test(test_parse_returns_minus1_when_sessions_array_empty),
        cmocka_unit_test(test_parse_returns_zero_fields_when_video_missing),
        cmocka_unit_test(test_parse_session_missing_udp_port_fails),
        cmocka_unit_test(test_parse_session_udp_port_exceeds_65535_fails),
        cmocka_unit_test(test_parse_session_missing_payload_type_defaults_to_96),
        cmocka_unit_test(test_parse_session_no_crop_object_fails),
        cmocka_unit_test(test_parse_session_negative_crop_x_fails),
        cmocka_unit_test(test_parse_session_zero_crop_w_fails),

        /* --- validate_tx_config --- */
        cmocka_unit_test(test_validate_valid_config_passes),
        cmocka_unit_test(test_validate_missing_interface_name_fails),
        cmocka_unit_test(test_validate_missing_dip_fails),
        cmocka_unit_test(test_validate_invalid_sip_fails),
        cmocka_unit_test(test_validate_invalid_dip_fails),
        cmocka_unit_test(test_validate_zero_width_fails),
        cmocka_unit_test(test_validate_zero_height_fails),
        cmocka_unit_test(test_validate_odd_width_fails),
        cmocka_unit_test(test_validate_supported_fps_values_all_pass),
        cmocka_unit_test(test_validate_unsupported_fps_fails),
        cmocka_unit_test(test_validate_unsupported_fmt_fails),
        cmocka_unit_test(test_validate_all_supported_fmts_pass),
        cmocka_unit_test(test_validate_zero_udp_port_fails),
        cmocka_unit_test(test_validate_payload_type_below_96_fails),
        cmocka_unit_test(test_validate_payload_type_above_127_fails),
        cmocka_unit_test(test_validate_payload_type_boundary_96_passes),
        cmocka_unit_test(test_validate_payload_type_boundary_127_passes),
        cmocka_unit_test(test_validate_crop_x_plus_w_exceeds_width_fails),
        cmocka_unit_test(test_validate_crop_y_plus_h_exceeds_height_fails),
        cmocka_unit_test(test_validate_odd_crop_w_fails),
        cmocka_unit_test(test_validate_zero_session_count_fails),
        cmocka_unit_test(test_validate_3sessions_tiled_layout_passes),
        cmocka_unit_test(test_validate_resolution_exceeds_max_fails),
        cmocka_unit_test(test_validate_2k_resolution_passes),
        cmocka_unit_test(test_validate_4k_resolution_passes),
        cmocka_unit_test(test_validate_4k_height_exceeds_max_fails),
        cmocka_unit_test(test_validate_scale_upscale_1080p_to_4k_passes),
        cmocka_unit_test(test_validate_scale_downscale_4k_to_1080p_passes),
        cmocka_unit_test(test_validate_scale_width_only_fails),
        cmocka_unit_test(test_validate_scale_height_only_fails),
        cmocka_unit_test(test_validate_scale_exceeds_max_fails),
        cmocka_unit_test(test_validate_scale_odd_width_fails),
        cmocka_unit_test(test_validate_scale_odd_height_yuv420_fails),
        cmocka_unit_test(test_validate_scale_crop_exceeds_scaled_dims_fails),
        cmocka_unit_test(test_validate_scale_crop_within_scaled_dims_passes),
        cmocka_unit_test(test_validate_no_scale_passes),
        cmocka_unit_test(test_validate_duplicate_udp_ports_fails),
        cmocka_unit_test(test_validate_crop_x_misaligned_for_yuv422_fails),
        cmocka_unit_test(test_validate_tx_url_nonexistent_file_fails),
        cmocka_unit_test(test_validate_tx_url_existing_file_passes),

        /* --- STRIDE security validation --- */
        cmocka_unit_test(test_validate_non_multicast_dip_fails),
        cmocka_unit_test(test_validate_invalid_bdf_format_fails),
        cmocka_unit_test(test_validate_valid_bdf_passes),
        cmocka_unit_test(test_validate_privileged_udp_port_fails),
        cmocka_unit_test(test_validate_low_multicast_dip_passes_with_warning),
        cmocka_unit_test(test_validate_above_multicast_dip_fails),
        cmocka_unit_test(test_validate_udp_port_boundary_1024_passes),
        cmocka_unit_test(test_validate_udp_port_boundary_1023_fails),
        cmocka_unit_test(test_validate_short_bdf_format_fails),
        cmocka_unit_test(test_validate_nonhex_bdf_format_fails),
        cmocka_unit_test(test_validate_crop_x_plus_w_unsigned_overflow_safe),
        cmocka_unit_test(test_validate_crop_y_plus_h_unsigned_overflow_safe),

        /* --- peek_config_log_file --- */
        cmocka_unit_test(test_peek_log_file_returns_value_from_3sessions),
        cmocka_unit_test(test_peek_log_file_returns_minus1_when_field_absent),
        cmocka_unit_test(test_peek_log_file_returns_minus1_for_nonexistent_file),
        cmocka_unit_test(test_peek_log_file_fills_buffer_correctly),
        cmocka_unit_test(test_peek_log_file_null_path_returns_minus1),
        cmocka_unit_test(test_peek_log_file_null_buffer_returns_minus1),
        cmocka_unit_test(test_peek_log_file_handles_escaped_quote),
        cmocka_unit_test(test_peek_log_file_handles_escaped_backslash),
        cmocka_unit_test(test_peek_log_file_handles_escaped_forward_slash),
        cmocka_unit_test(test_peek_log_file_handles_escaped_newline),
        cmocka_unit_test(test_peek_log_file_handles_escaped_tab),
        cmocka_unit_test(test_peek_log_file_unknown_escape_kept_literal),
        cmocka_unit_test(test_peek_log_file_strips_bare_control_chars),

        /* --- load_and_apply_config --- */
        cmocka_unit_test(test_load_and_apply_config_null_app_returns_error),
        cmocka_unit_test(test_load_and_apply_config_null_file_returns_zero),
        cmocka_unit_test(test_load_and_apply_config_empty_file_returns_zero),
        cmocka_unit_test(test_load_and_apply_config_nonexistent_file_returns_minus1),
        cmocka_unit_test(test_load_and_apply_config_populates_app_context),
        cmocka_unit_test(test_load_and_apply_config_fmt_yuv444p),
        cmocka_unit_test(test_load_and_apply_config_fmt_gbrp10le),
        cmocka_unit_test(test_load_and_apply_config_fmt_yuv420),
        cmocka_unit_test(test_load_and_apply_config_unknown_fmt_fails),
        cmocka_unit_test(test_load_and_apply_config_copies_log_file),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
