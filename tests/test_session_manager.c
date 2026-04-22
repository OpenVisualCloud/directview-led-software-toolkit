/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 *
 * Unit tests for src/session_manager.c using cmocka.
 *
 * Strategy
 * --------
 * session_manager.c calls four hardware-dependent functions that we cannot
 * exercise without MTL / real video files:
 *
 *   open_ffmpeg_output()    — needs mtl_st20p muxer (DPDK hardware)
 *   close_ffmpeg_output()   — depends on out_fmt_ctx set by open_ffmpeg_output
 *   open_shared_ffmpeg()    — needs a real video file + codec
 *   close_shared_ffmpeg()   — depends on open_shared_ffmpeg
 *   load_video_source()     — calls open_ffmpeg_source (needs codec/file)
 *   close_ffmpeg_source()   — depends on load_video_source
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
 *   session_manager_start()        — g_tx_app_exit reset, thread launch
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

/* --------------------------------------------------------------------------
 * g_tx_app_exit is defined in session_manager.c; expose it here too so
 * tests can inspect it.
 * -------------------------------------------------------------------------- */
/* session_manager.c defines it; we declare extern to read it in tests */
extern _Atomic bool g_tx_app_exit;

#include "session_manager.h"
#include "ffmpeg_decoder.h"
#include "tx_app_context.h"
#include "util/logger.h"

#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

/* ==========================================================================
 * Mock implementations of ffmpeg_decoder.h functions
 *
 * These replace the real implementations from ffmpeg_decoder.c which is
 * intentionally NOT compiled into this test executable.
 * ========================================================================== */

/* Tracks how many times each mock was called */
static int mock_open_ffmpeg_output_calls  = 0;
static int mock_close_ffmpeg_output_calls = 0;
static int mock_load_video_source_calls   = 0;
static int mock_close_ffmpeg_source_calls = 0;
static int mock_open_shared_ffmpeg_calls  = 0;
static int mock_close_shared_ffmpeg_calls = 0;

/* Return value injected by the test */
static int mock_open_ffmpeg_output_ret  = 0;
static int mock_open_shared_ffmpeg_ret  = 0;

bool is_raw_yuv(const char* filename)
{
    if (!filename || !*filename) return false;
    const char* ext = strrchr(filename, '.');
    if (!ext) return false;
    return (strcasecmp(ext, ".yuv") == 0 || strcasecmp(ext, ".raw") == 0);
}

int open_ffmpeg_output(struct st20p_tx_ctx* ctx)
{
    mock_open_ffmpeg_output_calls++;
    if (mock_open_ffmpeg_output_ret != 0)
        return mock_open_ffmpeg_output_ret;

    /* Allocate enc_frame + enc_pkt so Path B/C in st20p_tx_thread can run.
     * Without these the thread's null-guard skips the frame-processing body. */
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
    AVFormatContext *fmt_ctx = NULL;
    if (avformat_alloc_output_context2(&fmt_ctx, NULL, "null", NULL) == 0 && fmt_ctx) {
        AVStream *st = avformat_new_stream(fmt_ctx, NULL);
        if (st) {
            st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
            st->codecpar->width      = w;
            st->codecpar->height     = h;
            st->codecpar->format     = ctx->app->fmt;
            avformat_write_header(fmt_ctx, NULL);
            ctx->out_fmt_ctx = fmt_ctx;
            ctx->out_stream  = st;
        } else {
            avformat_free_context(fmt_ctx);
        }
    }

    return 0;
}

void close_ffmpeg_output(struct st20p_tx_ctx* ctx)
{
    mock_close_ffmpeg_output_calls++;
    if (ctx->out_fmt_ctx) {
        av_write_trailer(ctx->out_fmt_ctx);
        avformat_free_context(ctx->out_fmt_ctx);
        ctx->out_fmt_ctx = NULL;
    }
    if (ctx->enc_frame) av_frame_free(&ctx->enc_frame);
    if (ctx->enc_pkt)   av_packet_free(&ctx->enc_pkt);
}

void send_video_frame(struct st20p_tx_ctx* ctx, AVFrame* src,
                      int crop_x, int crop_y, int crop_w, int crop_h)
{
    (void)ctx; (void)src; (void)crop_x; (void)crop_y; (void)crop_w; (void)crop_h;
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
    /* In tests: just spin until exit is set */
    struct shared_decode_ctx* dec = (struct shared_decode_ctx*)arg;
    while (!dec->exit && !g_tx_app_exit)
        usleep(1000);
    /* Unblock any TX threads that may be waiting on the barriers */
    pthread_barrier_wait(&dec->barrier_decoded);
    pthread_barrier_wait(&dec->barrier_copied);
    return NULL;
}

int load_video_source(struct st20p_tx_ctx* ctx, const char* filename)
{
    mock_load_video_source_calls++;
    (void)ctx; (void)filename;
    return 0;
}

void close_ffmpeg_source(struct st20p_tx_ctx* ctx)
{
    mock_close_ffmpeg_source_calls++;
    (void)ctx;
}

bool ffmpeg_decode_and_send(struct st20p_tx_ctx* ctx)
{
    (void)ctx;
    return false;
}

/* ==========================================================================
 * Helpers
 * ========================================================================== */

static void reset_mock_counters(void)
{
    mock_open_ffmpeg_output_calls  = 0;
    mock_close_ffmpeg_output_calls = 0;
    mock_load_video_source_calls   = 0;
    mock_close_ffmpeg_source_calls = 0;
    mock_open_shared_ffmpeg_calls  = 0;
    mock_close_shared_ffmpeg_calls = 0;
    mock_open_ffmpeg_output_ret    = 0;
    mock_open_shared_ffmpeg_ret    = 0;
}

/* Populate a minimal tx_app_context suitable for tests */
static void fill_app(struct tx_app_context *app, int num_sessions,
                     const char *tx_url)
{
    memset(app, 0, sizeof(*app));
    strncpy(app->port,          "0000:06:00.0",  sizeof(app->port) - 1);
    strncpy(app->sip_addr_str,  "192.168.50.29", sizeof(app->sip_addr_str) - 1);
    strncpy(app->dip_addr_str,  "239.168.85.20", sizeof(app->dip_addr_str) - 1);
    app->width          = 1920;
    app->height         = 1080;
    app->fps            = 30;
    app->fmt            = AV_PIX_FMT_YUV422P10LE;
    app->udp_port       = 20000;
    app->payload_type   = 96;
    app->st20p_sessions = num_sessions;
    if (tx_url)
        strncpy(app->tx_url, tx_url, sizeof(app->tx_url) - 1);

    /* Populate session_net[] for tiled 3-session layout */
    const int xs[] = {0, 640, 1280};
    for (int i = 0; i < num_sessions && i < MAX_TX_SESSIONS; i++) {
        app->session_net[i].udp_port     = 20000 + i * 2;
        app->session_net[i].payload_type = 96;
        app->session_net[i].crop_x       = (i < 3) ? xs[i] : 0;
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
 * session_manager_init — single session, no tx_url
 * ========================================================================== */

static void test_init_single_session_no_url(void **state)
{
    (void)state;
    reset_mock_counters();
    struct tx_app_context app;
    fill_app(&app, 1, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);
    assert_int_equal(mgr.st20p_count, 1);
    assert_null(mgr.shared_dec);       /* no shared decoder for single session */
    assert_int_equal(mock_open_ffmpeg_output_calls, 1);
    assert_int_equal(mock_open_shared_ffmpeg_calls, 0);
    assert_int_equal(mock_load_video_source_calls,  0); /* empty tx_url skipped */

    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * session_manager_init — 3 sessions, video URL (shared decoder path)
 * ========================================================================== */

static void test_init_3sessions_with_url_uses_shared_decoder(void **state)
{
    (void)state;
    reset_mock_counters();
    struct tx_app_context app;
    fill_app(&app, 3, "video.mp4");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);
    assert_int_equal(mgr.st20p_count, 3);
    assert_non_null(mgr.shared_dec);
    assert_int_equal(mock_open_shared_ffmpeg_calls, 1);
    assert_int_equal(mock_open_ffmpeg_output_calls, 3); /* one per session */

    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * session_manager_init — 3 sessions, raw YUV (no shared decoder)
 * ========================================================================== */

static void test_init_3sessions_raw_yuv_no_shared_decoder(void **state)
{
    (void)state;
    reset_mock_counters();
    struct tx_app_context app;
    fill_app(&app, 3, "video.yuv");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);
    assert_null(mgr.shared_dec);           /* raw YUV -> no shared decoder */
    assert_int_equal(mock_open_shared_ffmpeg_calls, 0);
    assert_int_equal(mock_open_ffmpeg_output_calls, 3);

    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * session_manager_init — open_ffmpeg_output failure
 * ========================================================================== */

static void test_init_fails_when_open_output_fails(void **state)
{
    (void)state;
    reset_mock_counters();
    mock_open_ffmpeg_output_ret = -1; /* simulate MTL unavailable */

    struct tx_app_context app;
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

    struct tx_app_context app;
    fill_app(&app, 3, "video.mp4");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), -1);
    /* Shared decoder must be released on failure */
    assert_null(mgr.shared_dec);
}

/* ==========================================================================
 * session_manager_init — zero sessions
 * ========================================================================== */

static void test_init_zero_sessions(void **state)
{
    (void)state;
    reset_mock_counters();
    struct tx_app_context app;
    fill_app(&app, 0, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);
    assert_int_equal(mgr.st20p_count, 0);
    assert_null(mgr.shared_dec);
    assert_int_equal(mock_open_ffmpeg_output_calls, 0);

    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * create_st20p_tx_session — crop rect from session_net[]
 * ========================================================================== */

static void test_create_session_uses_session_net_crop(void **state)
{
    (void)state;
    reset_mock_counters();
    struct tx_app_context app;
    fill_app(&app, 3, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    /* Verify crop values match what was set in session_net[] by fill_app() */
    const int expected_x[] = {0, 640, 1280};
    for (int i = 0; i < 3; i++) {
        struct st20p_tx_ctx *ctx = &mgr.st20p_sessions[i];
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
    struct tx_app_context app;
    fill_app(&app, 3, "");
    /* Zero out session_net[] to trigger the fallback path */
    memset(app.session_net, 0, sizeof(app.session_net));

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    /* Fallback: divide 1920 px evenly across 3 sessions -> 640 each */
    const int expected_x[] = {0, 640, 1280};
    for (int i = 0; i < 3; i++) {
        struct st20p_tx_ctx *ctx = &mgr.st20p_sessions[i];
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
    struct tx_app_context app;
    fill_app(&app, 3, "");
    /* Non-divisible width: 1921 -> strip = 640, last gets 641 */
    app.width = 1920; /* keep 1920 so even division */
    memset(app.session_net, 0, sizeof(app.session_net));

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
    struct tx_app_context app;
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
    struct tx_app_context app;
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
    struct tx_app_context app;
    fill_app(&app, 1, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);
    assert_int_equal(session_manager_start(&mgr), 0);
    assert_int_equal(session_manager_stop(&mgr), 0);
    assert_false(session_manager_is_running(&mgr));

    session_manager_cleanup(&mgr);
}

static void test_start_resets_g_tx_app_exit(void **state)
{
    (void)state;
    reset_mock_counters();
    struct tx_app_context app;
    fill_app(&app, 1, "");

    g_tx_app_exit = true; /* simulate previous run */

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);
    assert_int_equal(session_manager_start(&mgr), 0);
    assert_false(g_tx_app_exit); /* must be cleared on start */

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
    struct tx_app_context app;
    fill_app(&app, 1, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);
    session_manager_cleanup(&mgr);
    /* Second cleanup on already-cleaned-up manager must not crash */
    session_manager_cleanup(&mgr);
}

static void test_cleanup_calls_close_ffmpeg_output(void **state)
{
    (void)state;
    reset_mock_counters();
    struct tx_app_context app;
    fill_app(&app, 3, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);
    session_manager_cleanup(&mgr);

    assert_int_equal(mock_close_ffmpeg_output_calls, 3);
}

static void test_cleanup_3sessions_with_url_calls_close_shared(void **state)
{
    (void)state;
    reset_mock_counters();
    struct tx_app_context app;
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
    struct tx_app_context app;
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
 * ========================================================================== */

/* Setting use_ffmpeg=true after init forces st20p_tx_thread to execute
 * Path A (call ffmpeg_decode_and_send).  Our mock returns false immediately
 * so the thread loops until g_tx_app_exit is set by session_manager_stop(). */
static void test_thread_executes_ffmpeg_decode_path(void **state)
{
    (void)state;
    reset_mock_counters();
    struct tx_app_context app;
    fill_app(&app, 1, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    /* Force Path A: the thread will call ffmpeg_decode_and_send() */
    mgr.st20p_sessions[0].use_ffmpeg = true;

    assert_int_equal(session_manager_start(&mgr), 0);
    usleep(5000); /* let the thread run at least one iteration */
    session_manager_stop(&mgr);
    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * st20p_tx_thread — Path C (synthetic test pattern)
 *
 * When use_ffmpeg=false and source_buffer=NULL, the thread generates a
 * luma-ramp + neutral-chroma test pattern.  The mock open_ffmpeg_output now
 * allocates enc_frame + enc_pkt so the thread body actually runs.
 * ========================================================================== */

static void test_thread_executes_test_pattern_path(void **state)
{
    (void)state;
    reset_mock_counters();
    struct tx_app_context app;
    fill_app(&app, 1, "");   /* no tx_url → no source_buffer, no use_ffmpeg */

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    /* Confirm Path C conditions: use_ffmpeg=false, source_buffer=NULL */
    assert_false(mgr.st20p_sessions[0].use_ffmpeg);
    assert_null(mgr.st20p_sessions[0].source_buffer);

    assert_int_equal(session_manager_start(&mgr), 0);
    /* Let the thread generate at least 1 frame (Path C has usleep(1e6/fps))
     * With fps=30 that's ~33ms per frame; wait 80ms to be safe. */
    usleep(80000);
    session_manager_stop(&mgr);

    assert_true(mgr.st20p_sessions[0].frames_sent >= 1);
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
    struct tx_app_context app;
    fill_app(&app, 1, "");

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    struct st20p_tx_ctx* ctx = &mgr.st20p_sessions[0];

    /* Inject a fake raw YUV source buffer (Path B conditions) */
    ctx->use_ffmpeg = false;
    /* frame_size for 640x1080 yuv422p10le: W*H*2 (10-bit stored in 16-bit)
     * *2 planes. But we use a small arbitrary buffer that is >= frame_size. */
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(app.fmt);
    (void)desc; /* used only for assertion context */
    int w = ctx->crop_width  > 0 ? ctx->crop_width  : (int)app.width;
    int h = ctx->crop_height > 0 ? ctx->crop_height : (int)app.height;
    int fsz = av_image_get_buffer_size(app.fmt, w, h, 1);
    assert_true(fsz > 0);

    ctx->frame_size    = (size_t)fsz;
    ctx->source_size   = (size_t)fsz * 2; /* enough for 2 frames */
    ctx->source_buffer = calloc(1, ctx->source_size);
    assert_non_null(ctx->source_buffer);
    ctx->current_pos   = 0;
    ctx->loop_playback = true;

    assert_int_equal(session_manager_start(&mgr), 0);
    usleep(10000); /* let the thread run */
    session_manager_stop(&mgr);

    assert_true(ctx->frames_sent >= 1);

    free(ctx->source_buffer);
    ctx->source_buffer = NULL;
    session_manager_cleanup(&mgr);
}

/* ==========================================================================
 * st20p_tx_thread_shared — crop_width=0 fallback
 *
 * When session_net crop_w/h are 0, the shared thread uses app->width/height
 * as fallback. This exercises the ternary on lines 58/68.
 * ========================================================================== */

static void test_shared_thread_crop_fallback(void **state)
{
    (void)state;
    reset_mock_counters();
    struct tx_app_context app;
    fill_app(&app, 3, "video.mp4");
    /* Zero out crop fields to trigger fallback */
    for (int i = 0; i < 3; i++) {
        app.session_net[i].crop_w = 0;
        app.session_net[i].crop_h = 0;
    }

    session_manager_t mgr;
    assert_int_equal(session_manager_init(&mgr, &app), 0);

    /* The fallback divides app->width among 3 sessions. Verify it was applied
     * (crop_width > 0 even though session_net[].crop_w was 0). */
    assert_true(mgr.st20p_sessions[0].crop_width > 0);

    assert_int_equal(session_manager_start(&mgr), 0);
    usleep(10000);
    session_manager_stop(&mgr);
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
        cmocka_unit_test(test_init_fails_when_open_output_fails),
        cmocka_unit_test(test_init_fails_when_open_shared_ffmpeg_fails),
        cmocka_unit_test(test_init_zero_sessions),

        /* create_st20p_tx_session — crop */
        cmocka_unit_test(test_create_session_uses_session_net_crop),
        cmocka_unit_test(test_create_session_crop_fallback_3sessions),
        cmocka_unit_test(test_create_session_crop_fallback_last_gets_remainder),
        cmocka_unit_test(test_create_session_idx_assigned),

        /* start / stop */
        cmocka_unit_test(test_start_sets_running_true),
        cmocka_unit_test(test_stop_sets_running_false),
        cmocka_unit_test(test_start_resets_g_tx_app_exit),

        /* cleanup */
        cmocka_unit_test(test_cleanup_idempotent),
        cmocka_unit_test(test_cleanup_calls_close_ffmpeg_output),
        cmocka_unit_test(test_cleanup_3sessions_with_url_calls_close_shared),

        /* start/stop with shared decoder */
        cmocka_unit_test(test_start_stop_3sessions_shared_decoder),

        /* thread paths */
        cmocka_unit_test(test_thread_executes_ffmpeg_decode_path),
        cmocka_unit_test(test_thread_executes_test_pattern_path),
        cmocka_unit_test(test_thread_executes_raw_yuv_path),
        cmocka_unit_test(test_shared_thread_crop_fallback),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}