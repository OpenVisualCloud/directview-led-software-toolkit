/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 *
 * Unit tests for the MAX_DECODE_ATTEMPTS watchdog in ffmpeg_decoder.c.
 *
 * Strategy
 * --------
 * --wrap=av_read_frame lets us inject a perpetual failure return so the
 * decode loop never produces a frame.  A global flag controls whether the
 * mock is active, keeping setup functions (avformat_open_input, etc.) on
 * the real path.
 *
 * --wrap=av_guess_format is present because ffmpeg_frame_handler.c is
 * linked transitively.
 *
 * Covered
 * -------
 *   ffmpeg_decode_next_frame() — watchdog triggers after MAX_DECODE_ATTEMPTS
 *   shared_decode_thread()     — watchdog triggers, sets dec->exit = true
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

/* Provide accessor function stubs (session_manager.c is not linked) */
static _Atomic bool g_test_exit = false;
bool session_manager_should_exit(void) { return g_test_exit; }
void session_manager_request_exit(void) { g_test_exit = true; }
void session_manager_reset_exit(void) { g_test_exit = false; }

#include "app_context.h"
#include "ffmpeg/ffmpeg_decoder.h"
#include "ffmpeg/ffmpeg_frame_handler.h"
#include "util/logger.h"

/* =========================================================================
 * --wrap=av_read_frame mock
 *
 * When g_mock_av_read_frame is true, every call returns g_mock_ret instead
 * of reading from the real container.  This forces the decode loop to spin
 * without producing a frame until the watchdog triggers.
 * ========================================================================= */

static bool g_mock_av_read_frame = false;
static int  g_mock_ret = -1;  /* generic FFmpeg error (not EOF) */

int __real_av_read_frame(AVFormatContext*, AVPacket*);

int __wrap_av_read_frame(AVFormatContext* s, AVPacket* pkt)
{
    if (g_mock_av_read_frame)
        return g_mock_ret;
    return __real_av_read_frame(s, pkt);
}

/* =========================================================================
 * --wrap=av_guess_format mock (redirect "mtl_st20p" → "null" muxer)
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

static char TEST_VIDEO_PATH[64] = "/tmp/dvledtx_watchdog_XXXXXX";

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
 * Helper: fill a minimal dvledtx_context for 16×16 video
 * ========================================================================= */

static void fill_app_16x16(struct dvledtx_context* app, int sessions)
{
    memset(app, 0, sizeof(*app));
    strncpy(app->port,         "0000:06:00.0",  sizeof(app->port) - 1);
    strncpy(app->sip_addr_str, "192.168.50.29", sizeof(app->sip_addr_str) - 1);
    strncpy(app->dip_addr_str, "239.168.85.20", sizeof(app->dip_addr_str) - 1);
    app->width          = 16;
    app->height         = 16;
    app->fps            = 25;
    app->fmt            = AV_PIX_FMT_YUV422P10LE;
    app->udp_port       = 20000;
    app->payload_type   = 96;
    app->st20p_sessions = sessions;
    for (int i = 0; i < sessions && i < MAX_TX_SESSIONS; i++) {
        app->session_net[i].udp_port     = 20000 + i * 2;
        app->session_net[i].payload_type = 96;
        app->session_net[i].crop_x = 0;
        app->session_net[i].crop_y = 0;
        app->session_net[i].crop_w = 16;
        app->session_net[i].crop_h = 16;
    }
}

/* =========================================================================
 * Test: ffmpeg_decode_next_frame — watchdog triggers after 10000 attempts
 *
 * Mock av_read_frame returns -1 (generic error) on every call.  The decode
 * loop logs an error per iteration and increments decode_attempts until
 * MAX_DECODE_ATTEMPTS_PER_SESSION (10000) is exceeded, then returns false.
 * ========================================================================= */

static void test_per_session_watchdog_triggers(void **state)
{
    (void)state;
    struct dvledtx_context app;
    fill_app_16x16(&app, 1);
    app.exit    = false;
    g_test_exit = false;

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.idx         = 0;
    ctx.app         = &app;
    ctx.crop_width  = 16;
    ctx.crop_height = 16;

    /* Open source normally (mock disabled) */
    assert_int_equal(load_video_source(&ctx, TEST_VIDEO_PATH), 0);
    assert_true(ctx.use_ffmpeg);

    /* Enable mock: av_read_frame always returns generic error */
    g_mock_av_read_frame = true;
    g_mock_ret           = -1;

    bool got = ffmpeg_decode_next_frame(&ctx);
    assert_false(got);  /* watchdog must have triggered */

    g_mock_av_read_frame = false;
    close_ffmpeg_source(&ctx);
}

/* =========================================================================
 * Test: shared_decode_thread — watchdog triggers, sets dec->exit = true
 *
 * Same mock strategy.  We act as the single TX-thread peer so the final
 * barrier-pair sync completes and the thread can be joined cleanly.
 * ========================================================================= */

static void test_shared_decode_watchdog_triggers(void **state)
{
    (void)state;
    struct dvledtx_context app;
    fill_app_16x16(&app, 1);
    g_test_exit = false;

    struct shared_decode_ctx dec;
    memset(&dec, 0, sizeof(dec));
    dec.app          = &app;
    dec.num_sessions = 1;
    dec.exit         = false;

    /* Open shared decoder normally (mock disabled) */
    int ret = open_shared_ffmpeg(&dec, TEST_VIDEO_PATH);
    assert_int_equal(ret, 0);

    /* Barriers: decode thread + 1 test "TX thread" = 2 */
    pthread_barrier_init(&dec.barrier_decoded, NULL, 2);
    pthread_barrier_init(&dec.barrier_copied,  NULL, 2);
    pthread_mutex_init(&dec.start_mutex, NULL);
    pthread_cond_init(&dec.start_cond, NULL);
    dec.start_ready = true;

    /* Enable mock BEFORE starting thread: every av_read_frame returns -1 */
    g_mock_av_read_frame = true;
    g_mock_ret           = -1;

    pthread_t tid;
    ret = pthread_create(&tid, NULL, shared_decode_thread, &dec);
    assert_int_equal(ret, 0);

    /* Decode thread will spin 10000 times, trigger watchdog, set exit=true,
     * then hit the final barrier pair.  We participate so it can exit. */
    pthread_barrier_wait(&dec.barrier_decoded);
    pthread_barrier_wait(&dec.barrier_copied);

    pthread_join(tid, NULL);

    assert_true(dec.exit);
    assert_int_equal(dec.frame_counter, 0);  /* no frames decoded */

    g_mock_av_read_frame = false;

    pthread_barrier_destroy(&dec.barrier_decoded);
    pthread_barrier_destroy(&dec.barrier_copied);
    pthread_mutex_destroy(&dec.start_mutex);
    pthread_cond_destroy(&dec.start_cond);
    close_shared_ffmpeg(&dec);
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
        cmocka_unit_test(test_per_session_watchdog_triggers),
        cmocka_unit_test(test_shared_decode_watchdog_triggers),
    };

    int result = cmocka_run_group_tests(tests, NULL, NULL);
    unlink(TEST_VIDEO_PATH);
    return result;
}
