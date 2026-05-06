/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 *
 * Unit tests for src/ffmpeg/ffmpeg_decoder.c using cmocka.
 *
 * Covered
 * -------
 *   is_raw_yuv()          — pure extension check
 *   load_video_source()   — branch logic (empty url, raw YUV, missing files, video)
 *   close_ffmpeg_source() — null-guard + use_ffmpeg==false path
 *   close_shared_ffmpeg() — null-guard safety
 *
 * send_video_frame(), open_ffmpeg_output(), close_ffmpeg_output(), and
 * ffmpeg_decode_and_send() were removed (legacy dead code).
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Pull in FFmpeg types before dvledtx_context.h which uses AVPixelFormat */
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

/* Provide accessor function stubs that ffmpeg_decoder.c uses
 * (session_manager.c is not linked into this test). */
static _Atomic bool g_test_exit = false;
bool session_manager_should_exit(void) { return g_test_exit; }
void session_manager_request_exit(void) { g_test_exit = true; }
void session_manager_reset_exit(void) { g_test_exit = false; }

#include "app_context.h"
#include "ffmpeg/ffmpeg_decoder.h"
#include "ffmpeg/ffmpeg_frame_handler.h"

/* ==========================================================================
 * is_raw_yuv
 * ========================================================================== */

static void test_is_raw_yuv_dot_yuv_lowercase(void **state)
{
    (void)state;
    assert_true(is_raw_yuv("video.yuv"));
}

static void test_is_raw_yuv_dot_raw_lowercase(void **state)
{
    (void)state;
    assert_true(is_raw_yuv("video.raw"));
}

static void test_is_raw_yuv_dot_yuv_uppercase(void **state)
{
    (void)state;
    assert_true(is_raw_yuv("video.YUV"));
}

static void test_is_raw_yuv_dot_raw_uppercase(void **state)
{
    (void)state;
    assert_true(is_raw_yuv("video.RAW"));
}

static void test_is_raw_yuv_dot_yuv_mixed_case(void **state)
{
    (void)state;
    assert_true(is_raw_yuv("video.Yuv"));
}

static void test_is_raw_yuv_full_path_yuv(void **state)
{
    (void)state;
    assert_true(is_raw_yuv("/home/intel/data/frame_1920x1080.yuv"));
}

static void test_is_raw_yuv_full_path_raw(void **state)
{
    (void)state;
    assert_true(is_raw_yuv("/home/intel/data/frame_1920x1080.raw"));
}

static void test_is_raw_yuv_mp4_returns_false(void **state)
{
    (void)state;
    assert_false(is_raw_yuv("video.mp4"));
}

static void test_is_raw_yuv_h264_returns_false(void **state)
{
    (void)state;
    assert_false(is_raw_yuv("video.h264"));
}

static void test_is_raw_yuv_mkv_returns_false(void **state)
{
    (void)state;
    assert_false(is_raw_yuv("video.mkv"));
}

static void test_is_raw_yuv_no_extension_returns_false(void **state)
{
    (void)state;
    assert_false(is_raw_yuv("videofile"));
}

static void test_is_raw_yuv_empty_string_returns_false(void **state)
{
    (void)state;
    assert_false(is_raw_yuv(""));
}

static void test_is_raw_yuv_yuv_not_at_end_returns_false(void **state)
{
    (void)state;
    /* .yuv is not the final extension here */
    assert_false(is_raw_yuv("video.yuv.bak"));
}

static void test_is_raw_yuv_dot_only_returns_false(void **state)
{
    (void)state;
    assert_false(is_raw_yuv("."));
}

static void test_is_raw_yuv_just_extension_yuv(void **state)
{
    (void)state;
    /* Filename is just ".yuv" — extension IS .yuv */
    assert_true(is_raw_yuv(".yuv"));
}

/* ==========================================================================
 * load_video_source — branch logic
 * ========================================================================== */

static void test_load_video_source_empty_url_is_noop(void **state)
{
    (void)state;
    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    app.fmt = AV_PIX_FMT_YUV422P10LE;
    app.width = 1920; app.height = 1080;

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;

    /* Empty filename -> returns 0 and ctx stays unchanged */
    int ret = load_video_source(&ctx, "");
    assert_int_equal(ret, 0);
    assert_false(ctx.use_ffmpeg);
    assert_null(ctx.source_buffer);
}

static void test_load_video_source_null_url_is_noop(void **state)
{
    (void)state;
    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    app.fmt = AV_PIX_FMT_YUV422P10LE;
    app.width = 1920; app.height = 1080;

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;

    int ret = load_video_source(&ctx, NULL);
    assert_int_equal(ret, 0);
    assert_false(ctx.use_ffmpeg);
}

static void test_load_video_source_nonexistent_yuv_returns_minus1(void **state)
{
    (void)state;
    /* A missing .yuv file is treated as "no source" (returns 0, no crash).
     * The real implementation opens the file; if it fails it returns 0. */
    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    app.fmt = AV_PIX_FMT_YUV422P10LE;
    app.width = 1920; app.height = 1080;

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;

    int ret = load_video_source(&ctx, "/tmp/dvledtx_nonexistent_12345.yuv");
    assert_int_equal(ret, -1);
    assert_false(ctx.use_ffmpeg);
    assert_null(ctx.source_buffer);
}

static void test_load_video_source_nonexistent_raw_returns_minus1(void **state)
{
    (void)state;
    /* A missing .raw file: is_raw_yuv() returns true, fopen fails → returns 0 */
    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    app.fmt = AV_PIX_FMT_YUV422P10LE;
    app.width = 1920; app.height = 1080;

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;

    int ret = load_video_source(&ctx, "/tmp/dvledtx_nonexistent_12345.raw");
    assert_int_equal(ret, -1);
    assert_false(ctx.use_ffmpeg);
    assert_null(ctx.source_buffer);
}

static void test_load_video_source_nonexistent_mp4_returns_minus1(void **state)
{
    (void)state;
    /* A missing non-YUV file goes through open_ffmpeg_source which calls
     * avformat_open_input. That fails → open_ffmpeg_source returns -1 →
     * load_video_source propagates -1. */
    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    app.fmt = AV_PIX_FMT_YUV422P10LE;
    app.width = 1920; app.height = 1080;

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;

    int ret = load_video_source(&ctx, "/tmp/dvledtx_nonexistent_12345.mp4");
    assert_int_equal(ret, -1);
    assert_false(ctx.use_ffmpeg);
    assert_null(ctx.fmt_ctx);
}

static void test_load_video_source_real_yuv_file_populates_buffer(void **state)
{
    (void)state;
    /* Write a small .yuv file (48 bytes = 4x4 YUV422 8-bit packed) and
     * verify that load_video_source reads it into source_buffer. */
    char path[] = "/tmp/dvledtx_test_XXXXXX";
    int fd = mkstemp(path);
    assert_true(fd >= 0);
    close(fd);
    /* Rename to .yuv suffix so is_raw_yuv() recognises it */
    char yuv_path[256];
    snprintf(yuv_path, sizeof(yuv_path), "%s.yuv", path);
    rename(path, yuv_path);
    FILE *f = fopen(yuv_path, "wb");
    assert_non_null(f);
    const uint8_t dummy[48] = {0xAB};
    fwrite(dummy, 1, sizeof(dummy), f);
    fclose(f);

    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    app.fmt = AV_PIX_FMT_YUV422P10LE;
    app.width = 4; app.height = 4;

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;

    int ret = load_video_source(&ctx, yuv_path);
    unlink(yuv_path);
    assert_int_equal(ret, 0);
    assert_non_null(ctx.source_buffer);
    assert_true(ctx.source_size > 0);
    assert_false(ctx.use_ffmpeg);
    assert_true(ctx.loop_playback);

    free(ctx.source_buffer);
}

/* ==========================================================================
 * close_ffmpeg_source — null-guard and use_ffmpeg==false
 * ========================================================================== */

static void test_close_ffmpeg_source_noop_when_not_use_ffmpeg(void **state)
{
    (void)state;
    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.use_ffmpeg = false;
    /* Must not crash even though all pointers are NULL */
    close_ffmpeg_source(&ctx);
}

/* ==========================================================================
 * close_shared_ffmpeg — null-guard safety
 * ========================================================================== */

static void test_close_shared_ffmpeg_all_null_no_crash(void **state)
{
    (void)state;
    struct shared_decode_ctx dec;
    memset(&dec, 0, sizeof(dec));
    /* All pointers NULL — must not crash */
    close_shared_ffmpeg(&dec);
}

/* ==========================================================================
 * close_shared_ffmpeg — with manually-allocated resources
 * ========================================================================== */

static void test_close_shared_ffmpeg_with_allocated_resources(void **state)
{
    (void)state;
    struct shared_decode_ctx dec;
    memset(&dec, 0, sizeof(dec));

    dec.av_frame  = av_frame_alloc();
    dec.av_packet = av_packet_alloc();

    /* yuv_frame needs av_image_alloc for data[0] so close_shared_ffmpeg
     * calls av_freep(&yuv_frame->data[0]) correctly */
    dec.yuv_frame = av_frame_alloc();
    dec.yuv_frame->format = AV_PIX_FMT_YUV422P10LE;
    dec.yuv_frame->width  = 16;
    dec.yuv_frame->height = 16;
    av_image_alloc(dec.yuv_frame->data, dec.yuv_frame->linesize,
                   16, 16, AV_PIX_FMT_YUV422P10LE, 32);

    dec.sws_ctx = sws_getContext(16, 16, AV_PIX_FMT_YUV420P,
                                 16, 16, AV_PIX_FMT_YUV422P10LE,
                                 SWS_FAST_BILINEAR, NULL, NULL, NULL);
    assert_non_null(dec.sws_ctx);

    /* Must free all resources without crashing */
    close_shared_ffmpeg(&dec);

    assert_null(dec.av_frame);
    assert_null(dec.yuv_frame);
    assert_null(dec.av_packet);
    assert_null(dec.sws_ctx);
}

/* ==========================================================================
 * close_ffmpeg_source — use_ffmpeg=true with allocated resources
 * ========================================================================== */

static void test_close_ffmpeg_source_with_allocated_resources(void **state)
{
    (void)state;
    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.use_ffmpeg = true;
    ctx.av_frame   = av_frame_alloc();
    ctx.av_packet  = av_packet_alloc();

    ctx.yuv_frame = av_frame_alloc();
    ctx.yuv_frame->format = AV_PIX_FMT_YUV422P10LE;
    ctx.yuv_frame->width  = 16;
    ctx.yuv_frame->height = 16;
    av_image_alloc(ctx.yuv_frame->data, ctx.yuv_frame->linesize,
                   16, 16, AV_PIX_FMT_YUV422P10LE, 32);

    ctx.sws_ctx = sws_getContext(16, 16, AV_PIX_FMT_YUV420P,
                                 16, 16, AV_PIX_FMT_YUV422P10LE,
                                 SWS_FAST_BILINEAR, NULL, NULL, NULL);
    assert_non_null(ctx.sws_ctx);

    /* codec_ctx and fmt_ctx remain NULL — the if-guards skip them */
    close_ffmpeg_source(&ctx);

    assert_null(ctx.av_frame);
    assert_null(ctx.yuv_frame);
    assert_null(ctx.av_packet);
    assert_null(ctx.sws_ctx);
}

/* ==========================================================================
 * ffmpeg_decode_next_frame — null-guard (line 477 in ffmpeg_decoder.c)
 *
 * Calling with a zero-initialised ctx (no fmt_ctx / codec_ctx / etc.) must
 * return false without crashing.
 * ========================================================================== */

static void test_ffmpeg_decode_next_frame_null_ctx_returns_false(void **state)
{
    (void)state;

    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    app.fmt    = AV_PIX_FMT_YUV422P10LE;
    app.width  = 64;
    app.height = 16;

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;

    /* All FFmpeg context pointers are NULL — null-guard must return false */
    bool result = ffmpeg_decode_next_frame(&ctx);
    assert_false(result);
}

/* ==========================================================================
 * main
 * ========================================================================== */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* --- is_raw_yuv --- */
        cmocka_unit_test(test_is_raw_yuv_dot_yuv_lowercase),
        cmocka_unit_test(test_is_raw_yuv_dot_raw_lowercase),
        cmocka_unit_test(test_is_raw_yuv_dot_yuv_uppercase),
        cmocka_unit_test(test_is_raw_yuv_dot_raw_uppercase),
        cmocka_unit_test(test_is_raw_yuv_dot_yuv_mixed_case),
        cmocka_unit_test(test_is_raw_yuv_full_path_yuv),
        cmocka_unit_test(test_is_raw_yuv_full_path_raw),
        cmocka_unit_test(test_is_raw_yuv_mp4_returns_false),
        cmocka_unit_test(test_is_raw_yuv_h264_returns_false),
        cmocka_unit_test(test_is_raw_yuv_mkv_returns_false),
        cmocka_unit_test(test_is_raw_yuv_no_extension_returns_false),
        cmocka_unit_test(test_is_raw_yuv_empty_string_returns_false),
        cmocka_unit_test(test_is_raw_yuv_yuv_not_at_end_returns_false),
        cmocka_unit_test(test_is_raw_yuv_dot_only_returns_false),
        cmocka_unit_test(test_is_raw_yuv_just_extension_yuv),

        /* --- load_video_source --- */
        cmocka_unit_test(test_load_video_source_empty_url_is_noop),
        cmocka_unit_test(test_load_video_source_null_url_is_noop),
        cmocka_unit_test(test_load_video_source_nonexistent_yuv_returns_minus1),
        cmocka_unit_test(test_load_video_source_nonexistent_raw_returns_minus1),
        cmocka_unit_test(test_load_video_source_nonexistent_mp4_returns_minus1),
        cmocka_unit_test(test_load_video_source_real_yuv_file_populates_buffer),

        /* --- close_ffmpeg_source --- */
        cmocka_unit_test(test_close_ffmpeg_source_noop_when_not_use_ffmpeg),
        cmocka_unit_test(test_close_ffmpeg_source_with_allocated_resources),

        /* --- close_shared_ffmpeg --- */
        cmocka_unit_test(test_close_shared_ffmpeg_all_null_no_crash),
        cmocka_unit_test(test_close_shared_ffmpeg_with_allocated_resources),

        /* --- ffmpeg_decode_next_frame --- */
        cmocka_unit_test(test_ffmpeg_decode_next_frame_null_ctx_returns_false),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
