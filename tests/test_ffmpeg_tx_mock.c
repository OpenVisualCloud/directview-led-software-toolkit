/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 *
 * Mock-based unit tests for src/ffmpeg/ffmpeg_tx.c and
 * src/ffmpeg/ffmpeg_decoder.c — combined path tests.
 *
 * Strategy
 * --------
 * open_ffmpeg_tx() calls av_guess_format("mtl_st20p", ...) which only
 * succeeds with DPDK hardware.  We use --wrap=av_guess_format to redirect
 * that call to the "null" muxer (AVFMT_NOFILE), letting the full function
 * body execute without hardware.
 *
 * open_ffmpeg_source() (static in ffmpeg_decoder.c) needs a real video
 * file — we generate a tiny MPEG2 clip in /tmp at startup.
 *
 * Covered
 * -------
 *   open_ffmpeg_tx()           — success, app-default fallback, crop fallback
 *   close_ffmpeg_tx()          — full path (opened via open_ffmpeg_tx)
 *   ffmpeg_tx_send_yuv_frame() — decode + send one frame pipeline
 *   ffmpeg_decode_next_frame() — decodes one frame into ctx->yuv_frame, exits on flag
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
#include <pthread.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <libavdevice/avdevice.h>

static _Atomic bool g_test_exit = false;
bool session_manager_should_exit(void) { return g_test_exit; }
void session_manager_request_exit(void) { g_test_exit = true; }
void session_manager_reset_exit(void) { g_test_exit = false; }

#include "app_context.h"
#include "ffmpeg/ffmpeg_decoder.h"
#include "ffmpeg/ffmpeg_tx.h"
#include "ffmpeg/ffmpeg_frame_handler.h"
#include "util/logger.h"

/* =========================================================================
 * --wrap=av_guess_format: redirect "mtl_st20p" → "null" muxer
 * ========================================================================= */

const AVOutputFormat* __real_av_guess_format(const char*, const char*, const char*);

const AVOutputFormat* __wrap_av_guess_format(const char* short_name,
                                              const char* filename,
                                              const char* mime_type)
{
    if (short_name && strcmp(short_name, "mtl_st20p") == 0)
        return __real_av_guess_format("null", NULL, NULL);
    return __real_av_guess_format(short_name, filename, mime_type);
}

/* =========================================================================
 * Test video generator — 3-frame 16×16 MPEG2 clip written to /tmp
 * ========================================================================= */

static char TEST_VIDEO_PATH[64] = "/tmp/dvledtx_tx_mock_XXXXXX";

static int generate_test_video(void)
{
    const int W = 16, H = 16, FPS = 25, NFRAMES = 3;
    int ret = -1;

    int tmp_fd = mkstemp(TEST_VIDEO_PATH);
    if (tmp_fd < 0) return -1;
    close(tmp_fd);

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!codec) return -1;

    AVCodecContext* enc = avcodec_alloc_context3(codec);
    if (!enc) return -1;
    enc->width        = W;
    enc->height       = H;
    enc->time_base    = (AVRational){1, FPS};
    enc->framerate    = (AVRational){FPS, 1};
    enc->pix_fmt      = AV_PIX_FMT_YUV420P;
    enc->gop_size     = 10;
    enc->max_b_frames = 0;
    if (avcodec_open2(enc, codec, NULL) < 0) goto fail_enc;

    AVFormatContext* fmt = NULL;
    if (avformat_alloc_output_context2(&fmt, NULL, "mpeg", TEST_VIDEO_PATH) < 0)
        goto fail_enc;

    AVStream* st = avformat_new_stream(fmt, codec);
    if (!st) goto fail_fmt;
    avcodec_parameters_from_context(st->codecpar, enc);
    st->time_base = enc->time_base;

    if (avio_open(&fmt->pb, TEST_VIDEO_PATH, AVIO_FLAG_WRITE) < 0) goto fail_fmt;
    if (avformat_write_header(fmt, NULL) < 0) goto fail_io;

    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width  = W;
    frame->height = H;
    av_frame_get_buffer(frame, 32);

    AVPacket* pkt = av_packet_alloc();
    for (int i = 0; i < NFRAMES; i++) {
        av_frame_make_writable(frame);
        memset(frame->data[0], (i * 50) & 0xFF, (size_t)frame->linesize[0] * H);
        memset(frame->data[1], 128, (size_t)frame->linesize[1] * (H / 2));
        memset(frame->data[2], 128, (size_t)frame->linesize[2] * (H / 2));
        frame->pts = i;
        avcodec_send_frame(enc, frame);
        while (avcodec_receive_packet(enc, pkt) == 0) {
            pkt->stream_index = st->index;
            av_interleaved_write_frame(fmt, pkt);
            av_packet_unref(pkt);
        }
    }
    avcodec_send_frame(enc, NULL);
    while (avcodec_receive_packet(enc, pkt) == 0) {
        pkt->stream_index = st->index;
        av_interleaved_write_frame(fmt, pkt);
        av_packet_unref(pkt);
    }
    av_write_trailer(fmt);
    ret = 0;

    av_packet_free(&pkt);
    av_frame_free(&frame);
fail_io:
    avio_closep(&fmt->pb);
fail_fmt:
    avformat_free_context(fmt);
fail_enc:
    avcodec_free_context(&enc);
    return ret;
}

/* =========================================================================
 * Helper: minimal dvledtx_context for 16×16 video
 * ========================================================================= */

static void fill_app_16x16(struct dvledtx_context* app, int sessions)
{
    memset(app, 0, sizeof(*app));

    dvledtx_context_alloc(app, 1, sessions);

    strncpy(app->nics[0].port,         "0000:06:00.0",  sizeof(app->nics[0].port) - 1);
    strncpy(app->nics[0].sip_addr_str, "192.168.50.29", sizeof(app->nics[0].sip_addr_str) - 1);
    strncpy(app->nics[0].dip_addr_str, "239.168.85.20", sizeof(app->nics[0].dip_addr_str) - 1);
    app->width          = 16;
    app->height         = 16;
    app->fps            = 25;
    app->fmt            = AV_PIX_FMT_YUV422P10LE;
    app->udp_port       = 20000;
    app->payload_type   = 96;
    for (int i = 0; i < sessions; i++) {
        app->session_net[i].udp_port     = 20000 + i * 2;
        app->session_net[i].payload_type = 96;
        app->session_net[i].crop_w       = 16;
        app->session_net[i].crop_h       = 16;
    }
}

/* =========================================================================
 * open_ffmpeg_tx — success and fallback variants
 * ========================================================================= */

static void test_open_ffmpeg_tx_success(void **state)
{
    (void)state;
    avdevice_register_all();

    struct dvledtx_context app;
    fill_app_16x16(&app, 1);

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.idx         = 0;
    ctx.app         = &app;
    ctx.crop_width  = 16;
    ctx.crop_height = 16;

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

static void test_open_ffmpeg_tx_uses_app_defaults(void **state)
{
    (void)state;
    avdevice_register_all();

    struct dvledtx_context app;
    fill_app_16x16(&app, 1);
    /* Clear per-session network config — with udp_port/payload_type zeroed,
     * open_ffmpeg_tx must fall back to the app-level defaults. */
    memset(app.session_net, 0, (size_t)app.st20p_sessions * sizeof(*app.session_net));

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.idx         = 0; /* zeroed session_net → falls back to app defaults */
    ctx.app         = &app;
    ctx.crop_width  = 16;
    ctx.crop_height = 16;

    int ret = open_ffmpeg_tx(&ctx);
    assert_int_equal(ret, 0);
    assert_non_null(ctx.out_fmt_ctx);

    close_ffmpeg_tx(&ctx);
}

static void test_open_ffmpeg_tx_crop_fallback(void **state)
{
    (void)state;
    avdevice_register_all();

    struct dvledtx_context app;
    fill_app_16x16(&app, 1);

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.idx         = 0;
    ctx.app         = &app;
    ctx.crop_width  = 0; /* zero → must fall back to app.width */
    ctx.crop_height = 0;

    int ret = open_ffmpeg_tx(&ctx);
    assert_int_equal(ret, 0);
    assert_non_null(ctx.out_fmt_ctx);

    close_ffmpeg_tx(&ctx);
}

/* =========================================================================
 * close_ffmpeg_tx — with context from open_ffmpeg_tx
 * ========================================================================= */

static void test_close_ffmpeg_tx_full(void **state)
{
    (void)state;
    avdevice_register_all();

    struct dvledtx_context app;
    fill_app_16x16(&app, 1);

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.idx         = 0;
    ctx.app         = &app;
    ctx.crop_width  = 16;
    ctx.crop_height = 16;

    assert_int_equal(open_ffmpeg_tx(&ctx), 0);

    close_ffmpeg_tx(&ctx);
    assert_null(ctx.out_fmt_ctx);
    assert_null(ctx.enc_frame);
    assert_null(ctx.enc_pkt);
}

/* =========================================================================
 * ffmpeg_decode_next_frame — decodes a frame from the generated MP4
 * ========================================================================= */

static void test_ffmpeg_decode_next_frame_returns_true(void **state)
{
    (void)state;
    avdevice_register_all();

    struct dvledtx_context app;
    fill_app_16x16(&app, 1);
    app.exit      = false;
    g_test_exit = false;

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.idx         = 0;
    ctx.app         = &app;
    ctx.crop_width  = 16;
    ctx.crop_height = 16;

    assert_int_equal(open_ffmpeg_tx(&ctx), 0);
    assert_int_equal(load_video_source(&ctx, TEST_VIDEO_PATH), 0);
    assert_true(ctx.use_ffmpeg);

    bool got = ffmpeg_decode_next_frame(&ctx);
    assert_true(got);
    assert_non_null(ctx.yuv_frame);

    got = ffmpeg_decode_next_frame(&ctx);
    assert_true(got);

    close_ffmpeg_source(&ctx);
    close_ffmpeg_tx(&ctx);
}

static void test_ffmpeg_decode_next_frame_exits_on_flag(void **state)
{
    (void)state;
    avdevice_register_all();

    struct dvledtx_context app;
    fill_app_16x16(&app, 1);
    app.exit = true; /* signal exit before first decode */

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.idx         = 0;
    ctx.app         = &app;
    ctx.crop_width  = 16;
    ctx.crop_height = 16;

    assert_int_equal(open_ffmpeg_tx(&ctx), 0);
    assert_int_equal(load_video_source(&ctx, TEST_VIDEO_PATH), 0);

    bool got = ffmpeg_decode_next_frame(&ctx);
    assert_false(got);

    close_ffmpeg_source(&ctx);
    close_ffmpeg_tx(&ctx);
}

/* =========================================================================
 * ffmpeg_tx_send_yuv_frame — decode one frame then send it
 * ========================================================================= */

static void test_ffmpeg_tx_send_yuv_frame_pipeline(void **state)
{
    (void)state;
    avdevice_register_all();

    struct dvledtx_context app;
    fill_app_16x16(&app, 1);
    app.exit      = false;
    g_test_exit = false;

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.idx         = 0;
    ctx.app         = &app;
    ctx.crop_width  = 16;
    ctx.crop_height = 16;

    assert_int_equal(open_ffmpeg_tx(&ctx), 0);
    assert_int_equal(load_video_source(&ctx, TEST_VIDEO_PATH), 0);

    /* Decode one frame into yuv_frame */
    assert_true(ffmpeg_decode_next_frame(&ctx));
    assert_non_null(ctx.yuv_frame);

    /* Send it via ffmpeg_tx */
    int ret = ffmpeg_tx_send_yuv_frame(&ctx, ctx.yuv_frame, 0, 0, 16, 16);
    /* null muxer may return 0 or benign error; we only check no crash */
    (void)ret;

    close_ffmpeg_source(&ctx);
    close_ffmpeg_tx(&ctx);
}

/* ffmpeg_tx_send_yuv_frame — crop rectangle exceeds source → crop_yuv_frame
 * fails and the send returns -1 (covers the crop-failure error branch). */
static void test_ffmpeg_tx_send_yuv_frame_bad_crop_fails(void **state)
{
    (void)state;
    avdevice_register_all();

    struct dvledtx_context app;
    fill_app_16x16(&app, 1);
    app.exit      = false;
    g_test_exit = false;

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.idx         = 0;
    ctx.app         = &app;
    ctx.crop_width  = 16;
    ctx.crop_height = 16;

    assert_int_equal(open_ffmpeg_tx(&ctx), 0);
    assert_int_equal(load_video_source(&ctx, TEST_VIDEO_PATH), 0);

    assert_true(ffmpeg_decode_next_frame(&ctx));
    assert_non_null(ctx.yuv_frame);

    /* crop_x + crop_w = 8 + 16 = 24 > source width 16 → crop_yuv_frame fails */
    int ret = ffmpeg_tx_send_yuv_frame(&ctx, ctx.yuv_frame, 8, 0, 16, 16);
    assert_int_equal(ret, -1);

    close_ffmpeg_source(&ctx);
    close_ffmpeg_tx(&ctx);
}

/* =========================================================================
 * open_ffmpeg_tx — multi-NIC redundant-port registration (idx 0, nic_count 2)
 * ========================================================================= */

static void test_open_ffmpeg_tx_multi_nic(void **state)
{
    (void)state;
    avdevice_register_all();

    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    assert_int_equal(dvledtx_context_alloc(&app, 2, 1), 0);

    strncpy(app.nics[0].port,         "0000:06:00.0",  sizeof(app.nics[0].port) - 1);
    strncpy(app.nics[0].sip_addr_str, "192.168.50.29", sizeof(app.nics[0].sip_addr_str) - 1);
    strncpy(app.nics[0].dip_addr_str, "239.168.85.20", sizeof(app.nics[0].dip_addr_str) - 1);
    strncpy(app.nics[1].port,         "0000:06:00.2",  sizeof(app.nics[1].port) - 1);
    strncpy(app.nics[1].sip_addr_str, "192.168.50.30", sizeof(app.nics[1].sip_addr_str) - 1);
    strncpy(app.nics[1].dip_addr_str, "239.168.85.21", sizeof(app.nics[1].dip_addr_str) - 1);
    app.width        = 16;
    app.height       = 16;
    app.fps          = 25;
    app.fmt          = AV_PIX_FMT_YUV422P10LE;
    app.udp_port     = 20000;
    app.payload_type = 96;
    app.session_net[0].udp_port     = 20000;
    app.session_net[0].payload_type = 96;
    app.session_net[0].crop_w       = 16;
    app.session_net[0].crop_h       = 16;
    app.session_net[0].nic_index    = 0;

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.idx         = 0; /* first session → registers second NIC as redundant */
    ctx.app         = &app;
    ctx.crop_width  = 16;
    ctx.crop_height = 16;

    assert_int_equal(open_ffmpeg_tx(&ctx), 0);
    assert_non_null(ctx.out_fmt_ctx);

    close_ffmpeg_tx(&ctx);
    dvledtx_context_free(&app);
}

/* =========================================================================
 * ffmpeg_tx_send_raw_yuv — guard + full raw playback path (with wrap)
 * ========================================================================= */

static void test_ffmpeg_tx_send_raw_yuv_guard(void **state)
{
    (void)state;
    /* enc_frame/enc_pkt NULL → early -1 without touching MTL */
    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    assert_int_equal(ffmpeg_tx_send_raw_yuv(&ctx), -1);
}

static void test_ffmpeg_tx_send_raw_yuv_pipeline(void **state)
{
    (void)state;
    avdevice_register_all();

    struct dvledtx_context app;
    fill_app_16x16(&app, 1);

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.idx         = 0;
    ctx.app         = &app;
    ctx.crop_width  = 16;
    ctx.crop_height = 16;

    assert_int_equal(open_ffmpeg_tx(&ctx), 0);

    int frame_sz = av_image_get_buffer_size(app.fmt, 16, 16, 1);
    assert_true(frame_sz > 0);
    ctx.frame_size   = (size_t)frame_sz;
    ctx.source_size  = (size_t)frame_sz * 2; /* room for two frames */
    ctx.source_buffer = calloc(1, ctx.source_size);
    assert_non_null(ctx.source_buffer);

    /* First send from offset 0 */
    (void)ffmpeg_tx_send_raw_yuv(&ctx);

    /* Force the loop-playback wrap branch (current_pos + frame > source_size) */
    ctx.current_pos = ctx.source_size;
    (void)ffmpeg_tx_send_raw_yuv(&ctx);

    free(ctx.source_buffer);
    close_ffmpeg_tx(&ctx);
    dvledtx_context_free(&app);
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void)
{
    logger_init_default();

    if (generate_test_video() != 0) {
        fprintf(stderr, "FATAL: cannot generate test video\n");
        return 1;
    }

    const struct CMUnitTest tests[] = {
        /* open_ffmpeg_tx */
        cmocka_unit_test(test_open_ffmpeg_tx_success),
        cmocka_unit_test(test_open_ffmpeg_tx_uses_app_defaults),
        cmocka_unit_test(test_open_ffmpeg_tx_crop_fallback),
        cmocka_unit_test(test_open_ffmpeg_tx_multi_nic),

        /* close_ffmpeg_tx */
        cmocka_unit_test(test_close_ffmpeg_tx_full),

        /* ffmpeg_decode_next_frame */
        cmocka_unit_test(test_ffmpeg_decode_next_frame_returns_true),
        cmocka_unit_test(test_ffmpeg_decode_next_frame_exits_on_flag),

        /* pipeline: decode + tx_send_yuv_frame */
        cmocka_unit_test(test_ffmpeg_tx_send_yuv_frame_pipeline),
        cmocka_unit_test(test_ffmpeg_tx_send_yuv_frame_bad_crop_fails),

        /* raw YUV send path */
        cmocka_unit_test(test_ffmpeg_tx_send_raw_yuv_guard),
        cmocka_unit_test(test_ffmpeg_tx_send_raw_yuv_pipeline),
    };

    int result = cmocka_run_group_tests(tests, NULL, NULL);
    unlink(TEST_VIDEO_PATH);
    return result;
}
