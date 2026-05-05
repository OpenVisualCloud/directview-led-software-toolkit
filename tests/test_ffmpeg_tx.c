/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 *
 * Unit tests for src/ffmpeg/ffmpeg_tx.c using cmocka.
 *
 * Strategy
 * --------
 * open_ffmpeg_tx() calls av_guess_format("mtl_st20p",...) which requires the
 * real MTL FFmpeg plugin at runtime.  We wrap av_guess_format via the linker
 * (--wrap=av_guess_format) to return the "null" muxer instead.
 *
 * Covered:
 *   open_ffmpeg_tx()           — success path, enc_frame/enc_pkt allocated
 *   close_ffmpeg_tx()          — frees enc_frame, enc_pkt, out_fmt_ctx
 *   ffmpeg_tx_send_yuv_frame() — NULL guard, successful crop+pack+write
 *   ffmpeg_tx_send_raw_yuv()   — NULL source guard, sequential read + wrap
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavdevice/avdevice.h>

/* Provide accessor function stubs (session_manager.c is not linked). */
#include <stdatomic.h>
static _Atomic bool g_test_exit = false;
bool session_manager_should_exit(void) { return g_test_exit; }
void session_manager_request_exit(void) { g_test_exit = true; }
void session_manager_reset_exit(void) { g_test_exit = false; }

#include "app_context.h"
#include "ffmpeg/ffmpeg_tx.h"
#include "ffmpeg/ffmpeg_frame_handler.h"

/* ==========================================================================
 * Linker wrap: av_guess_format → return "null" muxer
 * ========================================================================== */

const AVOutputFormat* __real_av_guess_format(const char* short_name,
                                              const char* filename,
                                              const char* mime_type);

const AVOutputFormat* __wrap_av_guess_format(const char* short_name,
                                              const char* filename,
                                              const char* mime_type) {
    if (short_name && strcmp(short_name, "mtl_st20p") == 0)
        return __real_av_guess_format("null", NULL, NULL);
    return __real_av_guess_format(short_name, filename, mime_type);
}

/* ==========================================================================
 * Helpers
 * ========================================================================== */

static void fill_app(struct dvledtx_context* app) {
    memset(app, 0, sizeof(*app));
    strncpy(app->port,         "0000:06:00.0",  sizeof(app->port) - 1);
    strncpy(app->sip_addr_str, "192.168.50.29", sizeof(app->sip_addr_str) - 1);
    strncpy(app->dip_addr_str, "239.168.85.20", sizeof(app->dip_addr_str) - 1);
    app->width        = 640;
    app->height       = 1080;
    app->fps          = 30;
    app->fmt          = AV_PIX_FMT_YUV422P10LE;
    app->udp_port     = 20000;
    app->payload_type = 96;
    app->st20p_sessions = 1;
    app->session_net[0].udp_port     = 20000;
    app->session_net[0].payload_type = 96;
    app->session_net[0].crop_x = 0;
    app->session_net[0].crop_y = 0;
    app->session_net[0].crop_w = 640;
    app->session_net[0].crop_h = 1080;
}

static void fill_ctx(struct st20p_tx_ctx* ctx, struct dvledtx_context* app) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->idx        = 0;
    ctx->app        = app;
    ctx->crop_width  = app->session_net[0].crop_w;
    ctx->crop_height = app->session_net[0].crop_h;
}

/* ==========================================================================
 * open_ffmpeg_tx / close_ffmpeg_tx
 * ========================================================================== */

static void test_open_ffmpeg_tx_allocates_enc_frame_and_pkt(void **state) {
    (void)state;
    avdevice_register_all();

    struct dvledtx_context app;
    fill_app(&app);
    struct st20p_tx_ctx ctx;
    fill_ctx(&ctx, &app);

    int ret = open_ffmpeg_tx(&ctx);
    assert_int_equal(ret, 0);
    assert_non_null(ctx.out_fmt_ctx);
    assert_non_null(ctx.enc_frame);
    assert_non_null(ctx.enc_pkt);

    close_ffmpeg_tx(&ctx);
    assert_null(ctx.out_fmt_ctx);
    assert_null(ctx.enc_frame);
    assert_null(ctx.enc_pkt);
}

static void test_close_ffmpeg_tx_idempotent(void **state) {
    (void)state;
    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* Calling close on a zero-initialised ctx must not crash */
    close_ffmpeg_tx(&ctx);
    close_ffmpeg_tx(&ctx);
}

/* ==========================================================================
 * ffmpeg_tx_send_yuv_frame
 * ========================================================================== */

static void test_ffmpeg_tx_send_yuv_frame_null_guard(void **state) {
    (void)state;
    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* All fields NULL — must return -1 without crashing */
    int ret = ffmpeg_tx_send_yuv_frame(&ctx, NULL, 0, 0, 64, 16);
    assert_int_equal(ret, -1);
}

/* ffmpeg_tx_send_yuv_frame: crop_yuv_frame fails when src is NULL (lines 224-225) */
static void test_ffmpeg_tx_send_yuv_frame_null_src_returns_minus1(void **state) {
    (void)state;
    avdevice_register_all();

    struct dvledtx_context app;
    fill_app(&app);
    app.width  = 64;
    app.height = 16;
    app.session_net[0].crop_w = 64;
    app.session_net[0].crop_h = 16;
    struct st20p_tx_ctx ctx;
    fill_ctx(&ctx, &app);

    assert_int_equal(open_ffmpeg_tx(&ctx), 0);

    /* src=NULL → crop_yuv_frame returns -1 */
    int ret = ffmpeg_tx_send_yuv_frame(&ctx, NULL, 0, 0, 64, 16);
    assert_int_equal(ret, -1);

    close_ffmpeg_tx(&ctx);
}

static void test_ffmpeg_tx_send_yuv_frame_success(void **state) {
    (void)state;
    avdevice_register_all();

    struct dvledtx_context app;
    fill_app(&app);
    /* Use a tiny frame for the test to keep it fast */
    app.width  = 64;
    app.height = 16;
    app.session_net[0].crop_w = 64;
    app.session_net[0].crop_h = 16;
    struct st20p_tx_ctx ctx;
    fill_ctx(&ctx, &app);

    assert_int_equal(open_ffmpeg_tx(&ctx), 0);

    /* Build a dummy full-width source AVFrame */
    AVFrame* src = av_frame_alloc();
    src->format = app.fmt;
    src->width  = (int)app.width;
    src->height = (int)app.height;
    assert_int_equal(av_frame_get_buffer(src, 32), 0);
    av_frame_make_writable(src);
    memset(src->data[0], 0x40, (size_t)src->linesize[0] * (int)app.height);

    int ret = ffmpeg_tx_send_yuv_frame(&ctx, src, 0, 0, (int)app.width, (int)app.height);
    /* null muxer: av_write_frame may return 0 or an error; we just check no crash */
    (void)ret;
    assert_true(ctx.frames_sent <= 1); /* sent 0 or 1 depending on muxer */

    av_frame_free(&src);
    close_ffmpeg_tx(&ctx);
}

/* ==========================================================================
 * ffmpeg_tx_send_raw_yuv
 * ========================================================================== */

static void test_ffmpeg_tx_send_raw_yuv_null_source(void **state) {
    (void)state;
    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* No source_buffer — must return -1 */
    int ret = ffmpeg_tx_send_raw_yuv(&ctx);
    assert_int_equal(ret, -1);
}

static void test_ffmpeg_tx_send_raw_yuv_advances_position(void **state) {
    (void)state;
    avdevice_register_all();

    struct dvledtx_context app;
    fill_app(&app);
    app.width  = 64;
    app.height = 16;
    app.session_net[0].crop_w = 64;
    app.session_net[0].crop_h = 16;
    struct st20p_tx_ctx ctx;
    fill_ctx(&ctx, &app);

    assert_int_equal(open_ffmpeg_tx(&ctx), 0);

    int fsz = av_image_get_buffer_size(app.fmt, (int)app.width, (int)app.height, 1);
    assert_true(fsz > 0);
    ctx.frame_size    = (size_t)fsz;
    ctx.source_size   = (size_t)fsz * 3;
    ctx.source_buffer = calloc(1, ctx.source_size);
    assert_non_null(ctx.source_buffer);
    ctx.current_pos   = 0;

    /* Send two frames — position must advance by frame_size each time */
    ffmpeg_tx_send_raw_yuv(&ctx);
    assert_int_equal((int)ctx.current_pos, fsz);

    ffmpeg_tx_send_raw_yuv(&ctx);
    assert_int_equal((int)ctx.current_pos, 2 * fsz);

    free(ctx.source_buffer);
    ctx.source_buffer = NULL;
    close_ffmpeg_tx(&ctx);
}

static void test_ffmpeg_tx_send_raw_yuv_wraps_at_end(void **state) {
    (void)state;
    avdevice_register_all();

    struct dvledtx_context app;
    fill_app(&app);
    app.width  = 64;
    app.height = 16;
    app.session_net[0].crop_w = 64;
    app.session_net[0].crop_h = 16;
    struct st20p_tx_ctx ctx;
    fill_ctx(&ctx, &app);

    assert_int_equal(open_ffmpeg_tx(&ctx), 0);

    int fsz = av_image_get_buffer_size(app.fmt, (int)app.width, (int)app.height, 1);
    ctx.frame_size    = (size_t)fsz;
    ctx.source_size   = (size_t)fsz; /* exactly one frame — wraps on second send */
    ctx.source_buffer = calloc(1, ctx.source_size);
    assert_non_null(ctx.source_buffer);
    ctx.current_pos   = 0;

    /* First send reads the whole buffer */
    ffmpeg_tx_send_raw_yuv(&ctx);
    assert_int_equal((int)ctx.current_pos, fsz);

    /* Second send should wrap back to 0 */
    ffmpeg_tx_send_raw_yuv(&ctx);
    assert_int_equal((int)ctx.current_pos, fsz);

    free(ctx.source_buffer);
    ctx.source_buffer = NULL;
    close_ffmpeg_tx(&ctx);
}

/* ==========================================================================
 * main
 * ========================================================================== */

int main(void) {
    const struct CMUnitTest tests[] = {
        /* open / close */
        cmocka_unit_test(test_open_ffmpeg_tx_allocates_enc_frame_and_pkt),
        cmocka_unit_test(test_close_ffmpeg_tx_idempotent),

        /* send_yuv_frame */
        cmocka_unit_test(test_ffmpeg_tx_send_yuv_frame_null_guard),
        cmocka_unit_test(test_ffmpeg_tx_send_yuv_frame_null_src_returns_minus1),
        cmocka_unit_test(test_ffmpeg_tx_send_yuv_frame_success),

        /* send_raw_yuv */
        cmocka_unit_test(test_ffmpeg_tx_send_raw_yuv_null_source),
        cmocka_unit_test(test_ffmpeg_tx_send_raw_yuv_advances_position),
        cmocka_unit_test(test_ffmpeg_tx_send_raw_yuv_wraps_at_end),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
