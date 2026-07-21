/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 *
 * Unit tests for src/core/session_manager.c using cmocka.
 *
 * Strategy
 * --------
 * session_manager.c calls hardware-dependent functions that we cannot
 * exercise without MTL / real video files:
 *
 *   open_ffmpeg_tx()      — needs mtl_st20p muxer (DPDK hardware)
 *   close_ffmpeg_tx()     — depends on out_fmt_ctx set by open_ffmpeg_tx
 *   open_shared_ffmpeg()  — needs a real video file + codec
 *   close_shared_ffmpeg() — depends on open_shared_ffmpeg
 *   load_video_source()   — calls open_ffmpeg_source (needs codec/file)
 *   close_ffmpeg_source() — depends on load_video_source
 *
 * We provide MOCK REPLACEMENTS for all six in this translation unit.
 * The linker resolves them before the real ffmpeg_decoder.o because we
 * compile ffmpeg_decoder.c OUT of this test executable and supply the
 * stubs directly.
 *
 * What is actually under test
 * ---------------------------
 *   session_manager_init()         — allocation, shared-decoder decision logic,
 *                                    crop-fallback calculation
 *   session_manager_start()        — g_dvledtx_exit reset, thread launch
 *   session_manager_stop()         — thread join, running flag
 *   session_manager_cleanup()      — resource release, idempotency
 *   session_manager_is_running()   — simple accessor
 *   create_st20p_tx_session()      — crop rect from session_net vs. fallback
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

/* session_manager.c provides accessor functions for the exit flag */

#include "core/session_manager.h"
#include "ffmpeg/ffmpeg_decoder.h"
#include "ffmpeg/ffmpeg_tx.h"
#include "ffmpeg/ffmpeg_frame_handler.h"
#include "app_context.h"
#include "util/logger.h"

#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

/* ==========================================================================
 * Mock call counters and return-value injection
 * ========================================================================== */

static int mock_open_ffmpeg_tx_calls      = 0;
static int mock_close_ffmpeg_tx_calls     = 0;
static int mock_load_video_source_calls   = 0;
static int mock_close_ffmpeg_source_calls = 0;
static int mock_open_shared_ffmpeg_calls  = 0;
static int mock_close_shared_ffmpeg_calls = 0;

static int mock_open_ffmpeg_tx_ret     = 0;
static int mock_open_shared_ffmpeg_ret = 0;

/* Return-value injection for individual mock functions */
static int mock_ffmpeg_tx_send_yuv_frame_ret    = 0;
static int mock_ffmpeg_tx_send_raw_yuv_ret      = 0;
static int mock_ffmpeg_decode_next_frame_true_n = 0; /* return true this many times, then false */
static int mock_load_video_source_ret           = 0;

/* ==========================================================================

 * Mock: open_ffmpeg_tx
 *
 * Allocates enc_frame + enc_pkt and a null-muxer output context so that
 * the TX thread body (Path B) can actually run without real hardware.
 * ========================================================================== */

int open_ffmpeg_tx(struct st20p_tx_ctx* ctx)
{
    mock_open_ffmpeg_tx_calls++;
    if (mock_open_ffmpeg_tx_ret != 0)
        return mock_open_ffmpeg_tx_ret;

    int w = ctx->crop_width  > 0 ? ctx->crop_width  : (int)ctx->app->width;
    int h = ctx->crop_height > 0 ? ctx->crop_height : (int)ctx->app->height;

    ctx->enc_frame = av_frame_alloc();
    if (ctx->enc_frame) {
        ctx->enc_frame->format = ctx->app->fmt;
        ctx->enc_frame->width  = w;
        ctx->enc_frame->height = h;
        av_frame_get_buffer(ctx->enc_frame, 32);
    }

    int frame_sz = av_image_get_buffer_size(ctx->app->fmt, w, h, 1);
    if (frame_sz > 0) {
        ctx->enc_pkt = av_packet_alloc();
        if (ctx->enc_pkt)
            av_new_packet(ctx->enc_pkt, frame_sz);
    }

    /* Use a null-muxer output context so av_write_frame doesn't crash */
    AVFormatContext* fmt_ctx = NULL;
    if (avformat_alloc_output_context2(&fmt_ctx, NULL, "null", NULL) == 0 && fmt_ctx) {
        AVStream* st = avformat_new_stream(fmt_ctx, NULL);
        if (st) {
            st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
            st->codecpar->width      = w;
            st->codecpar->height     = h;
            st->codecpar->format     = ctx->app->fmt;
            int hdr_ret __attribute__((unused)) = avformat_write_header(fmt_ctx, NULL);
            ctx->out_fmt_ctx = fmt_ctx;
            ctx->out_stream  = st;
        } else {
            avformat_free_context(fmt_ctx);
        }
    }
    return 0;
}

void close_ffmpeg_tx(struct st20p_tx_ctx* ctx)
{
    mock_close_ffmpeg_tx_calls++;
    if (ctx->out_fmt_ctx) {
        av_write_trailer(ctx->out_fmt_ctx);
        avformat_free_context(ctx->out_fmt_ctx);
        ctx->out_fmt_ctx = NULL;
    }
    if (ctx->enc_frame) av_frame_free(&ctx->enc_frame);
    if (ctx->enc_pkt)   av_packet_free(&ctx->enc_pkt);
}

int ffmpeg_tx_send_yuv_frame(struct st20p_tx_ctx* ctx, const AVFrame* src,
                              int crop_x, int crop_y, int crop_w, int crop_h)
{
    (void)src; (void)crop_x; (void)crop_y; (void)crop_w; (void)crop_h;
    if (mock_ffmpeg_tx_send_yuv_frame_ret != 0) return mock_ffmpeg_tx_send_yuv_frame_ret;
    ctx->frames_sent++;
    return 0;
}

int ffmpeg_tx_send_raw_yuv(struct st20p_tx_ctx* ctx)
{
    if (mock_ffmpeg_tx_send_raw_yuv_ret != 0) return mock_ffmpeg_tx_send_raw_yuv_ret;
    if (!ctx->enc_frame || !ctx->enc_pkt) return -1;
    if (!ctx->source_buffer || ctx->source_size == 0) return -1;
    size_t frame_bytes = ctx->frame_size;
    if (ctx->current_pos + frame_bytes > ctx->source_size)
        ctx->current_pos = 0;
    size_t copy_sz = (ctx->current_pos + frame_bytes <= ctx->source_size)
                       ? frame_bytes : (ctx->source_size - ctx->current_pos);
    ctx->current_pos += copy_sz;
    ctx->frames_sent++;
    return 0;
}

/* ==========================================================================
 * Mock: ffmpeg_decoder.h functions
 * ========================================================================== */

/* is_raw_yuv is defined in ffmpeg_decoder.c which is not compiled into this
 * test target; provide a self-contained stub. */
bool is_raw_yuv(const char* filename)
{
    if (!filename || !*filename) return false;
    const char* ext = strrchr(filename, '.');
    return ext && (strcasecmp(ext, ".yuv") == 0 || strcasecmp(ext, ".raw") == 0);
}

bool ffmpeg_decode_next_frame(struct st20p_tx_ctx* ctx)
{
    (void)ctx;
    if (mock_ffmpeg_decode_next_frame_true_n > 0) {
        mock_ffmpeg_decode_next_frame_true_n--;
        return true;
    }
    return false;
}

int load_video_source(struct st20p_tx_ctx* ctx, const char* filename)
{
    mock_load_video_source_calls++;
    if (mock_load_video_source_ret != 0)
        return mock_load_video_source_ret;
    if (filename && *filename) {
        const char* ext = strrchr(filename, '.');
        bool is_yuv = ext && (strcasecmp(ext, ".yuv") == 0 ||
                               strcasecmp(ext, ".raw") == 0);
        ctx->use_ffmpeg = !is_yuv;
    }
    return 0;
}

void close_ffmpeg_source(struct st20p_tx_ctx* ctx)
{
    mock_close_ffmpeg_source_calls++;
    ctx->use_ffmpeg = false;
}

int open_shared_ffmpeg(struct shared_decode_ctx* dec, const char* filename)
{
    mock_open_shared_ffmpeg_calls++;
    (void)dec; (void)filename;
    return mock_open_shared_ffmpeg_ret;
}

void close_shared_ffmpeg(struct shared_decode_ctx* dec)
{
    mock_close_shared_ffmpeg_calls++;
    (void)dec;
}

void* shared_decode_thread(void* arg)
{
    struct shared_decode_ctx* dec = (struct shared_decode_ctx*)arg;
    /* Minimal stub: hit both barriers once then exit */
    pthread_barrier_wait(&dec->barrier_decoded);
    pthread_barrier_wait(&dec->barrier_copied);
    dec->exit = true;
    pthread_barrier_wait(&dec->barrier_decoded);
    pthread_barrier_wait(&dec->barrier_copied);
    return NULL;
}

/* ==========================================================================
 * Mock: ffmpeg_frame_handler.h functions
 * ========================================================================== */

int convert_frame_format(struct SwsContext* sws_ctx, const AVFrame* src,
                          int src_height, AVFrame* dst)
{
    (void)sws_ctx; (void)src; (void)src_height; (void)dst;
    return 0;
}

int crop_yuv_frame(AVFrame* dst, const AVFrame* src,
                   int crop_x, int crop_y, int crop_w, int crop_h,
                   enum AVPixelFormat fmt)
{
    (void)dst; (void)src; (void)crop_x; (void)crop_y;
    (void)crop_w; (void)crop_h; (void)fmt;
    return 0;
}

/* ==========================================================================
 * Mock counters reset helper
 * ========================================================================== */

static void reset_mock_counters(void)
{
    mock_open_ffmpeg_tx_calls      = 0;
    mock_close_ffmpeg_tx_calls     = 0;
    mock_load_video_source_calls   = 0;
    mock_close_ffmpeg_source_calls = 0;
    mock_open_shared_ffmpeg_calls  = 0;
    mock_close_shared_ffmpeg_calls = 0;
    mock_open_ffmpeg_tx_ret                 = 0;
    mock_open_shared_ffmpeg_ret             = 0;
    mock_ffmpeg_tx_send_yuv_frame_ret       = 0;
    mock_ffmpeg_tx_send_raw_yuv_ret         = 0;
    mock_ffmpeg_decode_next_frame_true_n    = 0;
    mock_load_video_source_ret              = 0;
}

/* ==========================================================================
 * Test fixture helpers
 * ========================================================================== */

static void fill_app(struct dvledtx_context* app, int sessions, const char* url)
{
    memset(app, 0, sizeof(*app));

    /* Allocate dynamic arrays */
    dvledtx_context_alloc(app, 1, sessions);

    strncpy(app->nics[0].port,         "0000:06:00.0",  sizeof(app->nics[0].port) - 1);
    strncpy(app->nics[0].sip_addr_str, "192.168.50.29", sizeof(app->nics[0].sip_addr_str) - 1);
    strncpy(app->nics[0].dip_addr_str, "239.168.85.20", sizeof(app->nics[0].dip_addr_str) - 1);
    strncpy(app->tx_url, url, sizeof(app->tx_url) - 1);
    app->width          = 1920;
    app->height         = 1080;
    app->fps            = 30;
    app->fmt            = AV_PIX_FMT_YUV422P10LE;
    app->udp_port       = 20000;
    app->payload_type   = 96;

    /* Distribute 1920px evenly across sessions — 640px each */
    for (int i = 0; i < sessions; i++) {
        app->session_net[i].udp_port     = 20000 + i * 2;
        app->session_net[i].payload_type = 96;
        app->session_net[i].crop_x       = i * 640;
        app->session_net[i].crop_y       = 0;
        app->session_net[i].crop_w       = 640;
        app->session_net[i].crop_h       = 1080;
    }
}

/* ==========================================================================
 * session_manager_is_running
 * ========================================================================== */

static void test_is_running_false_on_fresh_manager(void **state)
{
    (void)state;
    session_manager_t mgr;
    memset(&mgr, 0, sizeof(mgr));
    assert_false(session_manager_is_running(&mgr));
}

/* ==========================================================================
 * session_manager_init — single session, no URL
 * ========================================================================== */

static void test_init_single_session_no_url(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 1, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    assert_int_equal(mgr.st20p_count, 1);
    assert_null(mgr.shared_dec);       /* no shared decoder for single session */
    assert_int_equal(mock_open_ffmpeg_tx_calls, 1);
    assert_int_equal(mock_open_shared_ffmpeg_calls, 0);
    assert_int_equal(mock_load_video_source_calls,  0); /* empty tx_url skipped */

    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * session_manager_init — 3 sessions with MP4 URL → shared decoder
 * ========================================================================== */

static void test_init_3sessions_with_url_uses_shared_decoder(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 3, "video.mp4");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    assert_int_equal(mgr.st20p_count, 3);
    assert_non_null(mgr.shared_dec);
    assert_int_equal(mock_open_shared_ffmpeg_calls, 1);
    assert_int_equal(mock_open_ffmpeg_tx_calls, 3); /* one per session */

    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * session_manager_init — 3 sessions with raw YUV → no shared decoder
 * ========================================================================== */

static void test_init_3sessions_raw_yuv_no_shared_decoder(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 3, "source.yuv");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    assert_null(mgr.shared_dec);           /* raw YUV -> no shared decoder */
    assert_int_equal(mock_open_shared_ffmpeg_calls, 0);
    assert_int_equal(mock_open_ffmpeg_tx_calls, 3);

    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * session_manager_init — 3 sessions with screen capture -> shared decoder
 * ========================================================================== */

static void test_init_3sessions_screen_capture_uses_shared_decoder(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 3, "");
    app.use_screen_capture = true;
    strncpy(app.screen_input, ":0.0+0,0", sizeof(app.screen_input) - 1);

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    assert_non_null(mgr.shared_dec);
    assert_int_equal(mock_open_shared_ffmpeg_calls, 1);
    assert_int_equal(mock_load_video_source_calls, 0);

    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * session_manager_init — single session screen capture -> per-session source
 * ========================================================================== */

static void test_init_single_session_screen_capture_loads_source(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 1, "");
    app.use_screen_capture = true;
    strncpy(app.screen_input, ":0.0+0,0", sizeof(app.screen_input) - 1);

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    assert_null(mgr.shared_dec);
    assert_int_equal(mock_load_video_source_calls, 1);

    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * session_manager_init — open_ffmpeg_tx failure
 * ========================================================================== */

static void test_init_fails_when_open_output_fails(void **state)
{
    (void)state;
    reset_mock_counters();
    mock_open_ffmpeg_tx_ret = -1; /* simulate TX unavailable */

    struct dvledtx_context app;
    fill_app(&app, 1, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), -1);
}

/* ==========================================================================
 * session_manager_init — open_shared_ffmpeg failure
 * ========================================================================== */

static void test_init_fails_when_open_shared_ffmpeg_fails(void **state)
{
    (void)state;
    reset_mock_counters();
    mock_open_shared_ffmpeg_ret = -1;

    struct dvledtx_context app;
    fill_app(&app, 3, "video.mp4");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), -1);
    assert_null(mgr.shared_dec);
}

/* ==========================================================================
 * session_manager_init — zero sessions
 * ========================================================================== */

static void test_init_zero_sessions(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 0, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);
    assert_int_equal(mgr.st20p_count, 0);
    assert_null(mgr.shared_dec);
    assert_int_equal(mock_open_ffmpeg_tx_calls, 0);

    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * create_st20p_tx_session — crop rect from session_net[]
 * ========================================================================== */

static void test_create_session_uses_session_net_crop(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 3, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    const int expected_x[] = {0, 640, 1280};
    for (int i = 0; i < 3; i++) {
        struct st20p_tx_ctx* ctx = &mgr.st20p_sessions[i];
        assert_int_equal(ctx->crop_x_offset, expected_x[i]);
        assert_int_equal(ctx->crop_y_offset, 0);
        assert_int_equal(ctx->crop_width,    640);
        assert_int_equal(ctx->crop_height,   1080);
    }

    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * create_st20p_tx_session — crop fallback when session_net[] is zeroed
 * ========================================================================== */

static void test_create_session_crop_fallback_3sessions(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 3, "");
    memset(app.session_net, 0, (size_t)app.st20p_sessions * sizeof(*app.session_net));

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    /* Fallback: divide 1920 px evenly across 3 sessions → 640 each */
    const int expected_x[] = {0, 640, 1280};
    for (int i = 0; i < 3; i++) {
        struct st20p_tx_ctx* ctx = &mgr.st20p_sessions[i];
        assert_int_equal(ctx->crop_x_offset, expected_x[i]);
        assert_int_equal(ctx->crop_y_offset, 0);
        assert_int_equal(ctx->crop_width,    640);
        assert_int_equal(ctx->crop_height,   1080);
    }

    session_manager_cleanup(&mgr);
}

static void test_create_session_crop_fallback_last_gets_remainder(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 3, "");
    app.width = 1920;
    memset(app.session_net, 0, (size_t)app.st20p_sessions * sizeof(*app.session_net));

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    /* Last session covers remaining pixels: 1920 - 2*640 = 640 */
    assert_int_equal(mgr.st20p_sessions[2].crop_x_offset, 1280);
    assert_int_equal(mgr.st20p_sessions[2].crop_width,    640);

    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * create_st20p_tx_session — session idx is stored correctly
 * ========================================================================== */

static void test_create_session_idx_assigned(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 3, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    for (int i = 0; i < 3; i++)
        assert_int_equal(mgr.st20p_sessions[i].idx, i);

    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * session_manager_start / stop
 * ========================================================================== */

static void test_start_sets_running_true(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 1, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);
    assert_int_equal(session_manager_start(&mgr), 0);
    assert_true(session_manager_is_running(&mgr));

    session_manager_stop(&mgr);
    session_manager_cleanup(&mgr);
}

static void test_stop_sets_running_false(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 1, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);
    assert_int_equal(session_manager_start(&mgr), 0);
    assert_int_equal(session_manager_stop(&mgr), 0);
    assert_false(session_manager_is_running(&mgr));

    session_manager_cleanup(&mgr);
}

static void test_start_resets_g_dvledtx_exit(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 1, "");

    session_manager_request_exit(); /* simulate previous run */

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);
    assert_int_equal(session_manager_start(&mgr), 0);
    assert_false(session_manager_should_exit()); /* must be cleared on start */

    session_manager_stop(&mgr);
    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * session_manager_cleanup — idempotency
 * ========================================================================== */

static void test_cleanup_idempotent(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 1, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);
    session_manager_cleanup(&mgr);
    /* Second cleanup on already-cleaned-up manager must not crash */
    session_manager_cleanup(&mgr);
}

static void test_cleanup_calls_close_ffmpeg_tx(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 3, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);
    session_manager_cleanup(&mgr);

    assert_int_equal(mock_close_ffmpeg_tx_calls, 3);
}

static void test_cleanup_3sessions_with_url_calls_close_shared(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 3, "video.mp4");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);
    session_manager_cleanup(&mgr);

    assert_int_equal(mock_close_shared_ffmpeg_calls, 1);
}

/* ==========================================================================
 * session_manager_start + stop with shared decoder (3-session)
 * ========================================================================== */

static void test_start_stop_3sessions_shared_decoder(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 3, "video.mp4");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);
    assert_int_equal(session_manager_start(&mgr), 0);
    assert_true(session_manager_is_running(&mgr));

    assert_int_equal(session_manager_stop(&mgr), 0);
    assert_false(session_manager_is_running(&mgr));

    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * st20p_tx_thread — Path A (use_ffmpeg)
 *
 * Setting use_ffmpeg=true forces st20p_tx_thread to call
 * ffmpeg_decode_next_frame().  Our mock returns false immediately so the
 * thread exits after the first failed decode attempt.
 * ========================================================================== */

static void test_thread_executes_ffmpeg_decode_path(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 1, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    /* Force Path A */
    mgr.st20p_sessions[0].use_ffmpeg = true;

    assert_int_equal(session_manager_start(&mgr), 0);
    usleep(5000); /* let the thread run at least one iteration */
    session_manager_stop(&mgr);
    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * st20p_tx_thread — Path B (raw YUV source_buffer)
 *
 * We manually inject source_buffer after init (simulating load_video_source
 * having loaded a raw YUV file). The thread reads frame_size bytes per frame.
 * ========================================================================== */

static void test_thread_executes_raw_yuv_path(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 1, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    struct st20p_tx_ctx* ctx = &mgr.st20p_sessions[0];

    /* Inject a fake raw YUV source buffer (Path B conditions) */
    ctx->use_ffmpeg = false;
    int w = ctx->crop_width  > 0 ? ctx->crop_width  : (int)app.width;
    int h = ctx->crop_height > 0 ? ctx->crop_height : (int)app.height;
    int fsz = av_image_get_buffer_size(app.fmt, w, h, 1);
    assert_true(fsz > 0);

    /* Allocate yuv_frame container (no buffer — data[] are set per-frame by
     * tx_fetch_next_frame using av_image_fill_arrays into source_buffer). */
    ctx->yuv_frame = av_frame_alloc();
    assert_non_null(ctx->yuv_frame);
    ctx->yuv_frame->format = app.fmt;
    ctx->yuv_frame->width  = w;
    ctx->yuv_frame->height = h;

    ctx->frame_size    = (size_t)fsz;
    ctx->source_size   = (size_t)fsz * 2;
    ctx->source_buffer = calloc(1, ctx->source_size);
    assert_non_null(ctx->source_buffer);
    ctx->current_pos   = 0;
    ctx->loop_playback = true;

    assert_int_equal(session_manager_start(&mgr), 0);
    usleep(10000);
    session_manager_stop(&mgr);

    assert_true(ctx->frames_sent >= 1);

    free(ctx->source_buffer);
    ctx->source_buffer = NULL;
    /* yuv_frame data[] point into source_buffer (no AVBuffer), so av_frame_free
     * only releases the AVFrame struct — safe to call after freeing source_buffer. */
    av_frame_free(&ctx->yuv_frame);
    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * st20p_tx_thread_shared — crop fallback (3-session, zeroed session_net)
 * ========================================================================== */

static void test_shared_thread_crop_fallback(void **state)
{
    (void)state;
    reset_mock_counters();
    struct dvledtx_context app;
    fill_app(&app, 3, "video.mp4");
    /* Zero out crop fields to trigger the fallback path */
    for (int i = 0; i < 3; i++) {
        app.session_net[i].crop_w = 0;
        app.session_net[i].crop_h = 0;
    }

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    /* Fallback: divide app->width among 3 sessions — crop_width > 0 */
    assert_true(mgr.st20p_sessions[0].crop_width > 0);

    assert_int_equal(session_manager_start(&mgr), 0);
    usleep(10000);
    session_manager_stop(&mgr);
    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * create_st20p_tx_session: load_video_source failure (lines 205-206)
 * ========================================================================== */

static void test_init_fails_when_load_video_source_fails(void **state)
{
    (void)state;
    reset_mock_counters();

    struct dvledtx_context app;
    fill_app(&app, 1, "video.mp4"); /* non-empty, non-raw → load_video_source called */

    /* Inject failure into load_video_source mock */
    mock_load_video_source_ret = -1;

    session_manager_t mgr;
    int ret = session_manager_init(&mgr, &app);
    assert_int_equal(ret, -1);

    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * st20p_tx_thread — "no valid source" path (lines 142-143)
 *
 * use_ffmpeg==false and source_buffer==NULL → thread logs error and breaks.
 * ========================================================================== */

static void test_thread_no_source_exits_immediately(void **state)
{
    (void)state;
    reset_mock_counters();

    struct dvledtx_context app;
    fill_app(&app, 1, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    /* Force the "no valid source" branch */
    mgr.st20p_sessions[0].use_ffmpeg    = false;
    mgr.st20p_sessions[0].source_buffer = NULL;
    mgr.st20p_sessions[0].source_size   = 0;

    assert_int_equal(session_manager_start(&mgr), 0);
    usleep(5000);
    session_manager_stop(&mgr);
    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * st20p_tx_thread_shared — ffmpeg_tx_send_yuv_frame failure (line 74)
 *
 * Mock returns -1; shared thread logs the error but does not exit early.
 * ========================================================================== */

static void test_shared_thread_send_yuv_failure_is_non_fatal(void **state)
{
    (void)state;
    reset_mock_counters();

    struct dvledtx_context app;
    fill_app(&app, 3, "video.mp4");

    /* Make ffmpeg_tx_send_yuv_frame fail */
    mock_ffmpeg_tx_send_yuv_frame_ret = -1;

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    assert_int_equal(session_manager_start(&mgr), 0);
    usleep(10000);
    session_manager_stop(&mgr);
    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * st20p_tx_thread — Path A: decode succeeds once, send fails (lines 123, 125-126)
 *
 * ffmpeg_decode_next_frame returns true once (so ffmpeg_tx_send_yuv_frame is
 * called) but ffmpeg_tx_send_yuv_frame returns -1.  The thread logs the error
 * and continues; on the next iteration decode returns false and the thread exits.
 * ========================================================================== */

static void test_thread_single_path_a_send_failure(void **state)
{
    (void)state;
    reset_mock_counters();

    struct dvledtx_context app;
    fill_app(&app, 1, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    /* Force Path A; decode succeeds once then fails */
    mgr.st20p_sessions[0].use_ffmpeg            = true;
    mock_ffmpeg_decode_next_frame_true_n         = 1;
    mock_ffmpeg_tx_send_yuv_frame_ret            = -1;

    assert_int_equal(session_manager_start(&mgr), 0);
    usleep(5000);
    session_manager_stop(&mgr);
    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * st20p_tx_thread — Path B: ffmpeg_tx_send_raw_yuv failure (line 138)
 * ========================================================================== */

static void test_thread_raw_yuv_send_failure_logs_and_continues(void **state)
{
    (void)state;
    reset_mock_counters();

    struct dvledtx_context app;
    fill_app(&app, 1, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    struct st20p_tx_ctx* ctx = &mgr.st20p_sessions[0];

    int w   = ctx->crop_width  > 0 ? ctx->crop_width  : (int)app.width;
    int h   = ctx->crop_height > 0 ? ctx->crop_height : (int)app.height;
    int fsz = av_image_get_buffer_size(app.fmt, w, h, 1);
    assert_true(fsz > 0);

    ctx->use_ffmpeg    = false;
    ctx->frame_size    = (size_t)fsz;
    ctx->source_size   = (size_t)fsz * 2;
    ctx->source_buffer = calloc(1, ctx->source_size);
    assert_non_null(ctx->source_buffer);
    ctx->current_pos   = 0;
    ctx->loop_playback = true;

    /* Allocate yuv_frame container required by tx_fetch_next_frame Path B */
    ctx->yuv_frame = av_frame_alloc();
    assert_non_null(ctx->yuv_frame);
    ctx->yuv_frame->format = app.fmt;
    ctx->yuv_frame->width  = w;
    ctx->yuv_frame->height = h;

    /* Make the raw-yuv send fail: Path B now routes through ffmpeg_tx_send_yuv_frame */
    mock_ffmpeg_tx_send_yuv_frame_ret = -1;

    assert_int_equal(session_manager_start(&mgr), 0);
    usleep(5000);
    session_manager_stop(&mgr);

    free(ctx->source_buffer);
    ctx->source_buffer = NULL;
    av_frame_free(&ctx->yuv_frame);
    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * main
 * ========================================================================== */

int main(void)
{
    logger_init_default();

    const struct CMUnitTest tests[] = {
        /* is_running */
        cmocka_unit_test(test_is_running_false_on_fresh_manager),

        /* init */
        cmocka_unit_test(test_init_single_session_no_url),
        cmocka_unit_test(test_init_3sessions_with_url_uses_shared_decoder),
        cmocka_unit_test(test_init_3sessions_raw_yuv_no_shared_decoder),
        cmocka_unit_test(test_init_3sessions_screen_capture_uses_shared_decoder),
        cmocka_unit_test(test_init_single_session_screen_capture_loads_source),
        cmocka_unit_test(test_init_fails_when_open_output_fails),
        cmocka_unit_test(test_init_fails_when_open_shared_ffmpeg_fails),
        cmocka_unit_test(test_init_zero_sessions),
        cmocka_unit_test(test_init_fails_when_load_video_source_fails),

        /* create_st20p_tx_session — crop */
        cmocka_unit_test(test_create_session_uses_session_net_crop),
        cmocka_unit_test(test_create_session_crop_fallback_3sessions),
        cmocka_unit_test(test_create_session_crop_fallback_last_gets_remainder),
        cmocka_unit_test(test_create_session_idx_assigned),

        /* start / stop */
        cmocka_unit_test(test_start_sets_running_true),
        cmocka_unit_test(test_stop_sets_running_false),
        cmocka_unit_test(test_start_resets_g_dvledtx_exit),

        /* cleanup */
        cmocka_unit_test(test_cleanup_idempotent),
        cmocka_unit_test(test_cleanup_calls_close_ffmpeg_tx),
        cmocka_unit_test(test_cleanup_3sessions_with_url_calls_close_shared),

        /* start/stop with shared decoder */
        cmocka_unit_test(test_start_stop_3sessions_shared_decoder),

        /* thread paths */
        cmocka_unit_test(test_thread_executes_ffmpeg_decode_path),
        cmocka_unit_test(test_thread_executes_raw_yuv_path),
        cmocka_unit_test(test_shared_thread_crop_fallback),
        cmocka_unit_test(test_thread_no_source_exits_immediately),
        cmocka_unit_test(test_shared_thread_send_yuv_failure_is_non_fatal),
        cmocka_unit_test(test_thread_single_path_a_send_failure),
        cmocka_unit_test(test_thread_raw_yuv_send_failure_logs_and_continues),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
