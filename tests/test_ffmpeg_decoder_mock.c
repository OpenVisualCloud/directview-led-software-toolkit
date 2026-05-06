/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 *
 * Mock-based unit tests for src/ffmpeg/ffmpeg_decoder.c.
 *
 * Strategy
 * --------
 * 1. open_shared_ffmpeg() and open_ffmpeg_source() (static) need a real
 *    video file.  We generate a tiny MPEG2 file in /tmp at test startup.
 *
 * 2. shared_decode_thread() is exercised with the generated video and
 *    POSIX barriers acting as the TX-thread side.
 *
 * --wrap=av_guess_format is present so this binary can link ffmpeg_tx.c
 * (needed transitively through ffmpeg_frame_handler.c) without the
 * mtl_st20p muxer; it is not the primary test subject here.
 *
 * Covered
 * -------
 *   open_shared_ffmpeg()   — success + bad file
 *   load_video_source()    — real mp4, nonexistent mp4 (open_ffmpeg_source)
 *   shared_decode_thread() — decodes one frame via barrier synchronisation
 *   close_ffmpeg_source()  — full path
 *   close_shared_ffmpeg()  — full path
 *
 * open_ffmpeg_tx(), close_ffmpeg_tx(), ffmpeg_tx_send_yuv_frame(),
 * ffmpeg_tx_send_raw_yuv() and ffmpeg_decode_next_frame() are tested in
 * test_ffmpeg_tx_mock.c.
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

/* Provide accessor function stubs that ffmpeg_decoder.c uses */
static _Atomic bool g_test_exit = false;
bool session_manager_should_exit(void) { return g_test_exit; }
void session_manager_request_exit(void) { g_test_exit = true; }
void session_manager_reset_exit(void) { g_test_exit = false; }

#include "app_context.h"
#include "ffmpeg/ffmpeg_decoder.h"
#include "ffmpeg/ffmpeg_frame_handler.h"
#include "util/logger.h"

/* =========================================================================
 * --wrap=av_guess_format mock
 *
 * The real av_guess_format is renamed __real_av_guess_format by the linker.
 * Our __wrap_av_guess_format intercepts calls: when the caller asks for
 * "mtl_st20p" we return the "null" muxer format instead.
 * ========================================================================= */

const AVOutputFormat* __real_av_guess_format(const char*, const char*, const char*);

const AVOutputFormat* __wrap_av_guess_format(const char* short_name,
                                              const char* filename,
                                              const char* mime_type)
{
    if (short_name && strcmp(short_name, "mtl_st20p") == 0) {
        /* Return the "null" muxer: AVFMT_NOFILE, accepts everything */
        return __real_av_guess_format("null", NULL, NULL);
    }
    return __real_av_guess_format(short_name, filename, mime_type);
}

/* =========================================================================
 * Test video file generator
 *
 * Creates a tiny MPEG2 video: 3 frames, 16x16 px, yuv420p.
 * Returns 0 on success, -1 on failure.  Caller must unlink() when done.
 * ========================================================================= */

static char TEST_VIDEO_PATH[64] = "/tmp/dvledtx_test_mockXXXXXX";

static int generate_test_video(void)
{
    const int W = 16, H = 16, FPS = 25, NFRAMES = 3;
    int ret = -1;

    /* Create a unique temp file to avoid parallel test collisions */
    int tmp_fd = mkstemp(TEST_VIDEO_PATH);
    if (tmp_fd < 0) return -1;
    close(tmp_fd);

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!codec) return -1;

    AVCodecContext* enc = avcodec_alloc_context3(codec);
    if (!enc) return -1;
    enc->width       = W;
    enc->height      = H;
    enc->time_base   = (AVRational){1, FPS};
    enc->framerate   = (AVRational){FPS, 1};
    enc->pix_fmt     = AV_PIX_FMT_YUV420P;
    enc->gop_size    = 10;
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
        /* Simple gradient pattern */
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
    /* Flush encoder */
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
 * Helper: fill a minimal dvledtx_context for 16x16 video
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
 * Test: open_shared_ffmpeg — happy path with generated video
 * ========================================================================= */

static void test_open_shared_ffmpeg_success(void **state)
{
    (void)state;
    struct dvledtx_context app;
    fill_app_16x16(&app, 3);

    struct shared_decode_ctx dec;
    memset(&dec, 0, sizeof(dec));
    dec.app          = &app;
    dec.num_sessions = 3;

    int ret = open_shared_ffmpeg(&dec, TEST_VIDEO_PATH);
    assert_int_equal(ret, 0);
    assert_non_null(dec.fmt_ctx);
    assert_non_null(dec.codec_ctx);
    assert_non_null(dec.sws_ctx);
    assert_non_null(dec.yuv_frame);
    assert_non_null(dec.av_frame);
    assert_non_null(dec.av_packet);

    close_shared_ffmpeg(&dec);
}

static void test_open_shared_ffmpeg_bad_file_fails(void **state)
{
    (void)state;
    struct dvledtx_context app;
    fill_app_16x16(&app, 1);

    struct shared_decode_ctx dec;
    memset(&dec, 0, sizeof(dec));
    dec.app = &app;

    int ret = open_shared_ffmpeg(&dec, "/tmp/dvledtx_nonexistent_xyz.mp4");
    assert_int_equal(ret, -1);
}

/* =========================================================================
 * Test: load_video_source with a real (generated) video → open_ffmpeg_source
 * ========================================================================= */

static void test_load_video_source_mp4_calls_open_ffmpeg_source(void **state)
{
    (void)state;
    struct dvledtx_context app;
    fill_app_16x16(&app, 1);

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.idx = 0;
    ctx.app = &app;

    int ret = load_video_source(&ctx, TEST_VIDEO_PATH);
    assert_int_equal(ret, 0);
    assert_true(ctx.use_ffmpeg);
    assert_non_null(ctx.fmt_ctx);
    assert_non_null(ctx.codec_ctx);
    assert_non_null(ctx.sws_ctx);

    close_ffmpeg_source(&ctx);
}

static void test_load_video_source_nonexistent_mp4_returns_minus1(void **state)
{
    (void)state;
    /* A nonexistent .mp4 file causes avformat_open_input to fail inside
     * open_ffmpeg_source (static) → returns -1 → load_video_source propagates -1. */
    struct dvledtx_context app;
    fill_app_16x16(&app, 1);

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.idx = 0;
    ctx.app = &app;

    int ret = load_video_source(&ctx, "/tmp/dvledtx_nonexistent_xyz.mp4");
    assert_int_equal(ret, -1);
    assert_false(ctx.use_ffmpeg);
    assert_null(ctx.fmt_ctx);
}

/* =========================================================================
 * Test: shared_decode_thread — runs with generated video + barriers
 * ========================================================================= */

static void test_shared_decode_thread_decodes_frames(void **state)
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

    int ret = open_shared_ffmpeg(&dec, TEST_VIDEO_PATH);
    assert_int_equal(ret, 0);

    /* Barriers: count = num_sessions + 1 (decode thread + N TX threads).
     * In this test, "we" are the single TX thread. */
    pthread_barrier_init(&dec.barrier_decoded, NULL, 2);
    pthread_barrier_init(&dec.barrier_copied,  NULL, 2);

    /* Initialize and signal the startup gate so the thread proceeds */
    pthread_mutex_init(&dec.start_mutex, NULL);
    pthread_cond_init(&dec.start_cond, NULL);
    dec.start_ready = true;

    pthread_t tid;
    ret = pthread_create(&tid, NULL, shared_decode_thread, &dec);
    assert_int_equal(ret, 0);

    /* Act as TX thread: wait for decoded frame, then signal copied */
    pthread_barrier_wait(&dec.barrier_decoded); /* frame decoded */
    /* We could inspect dec.yuv_frame here */
    assert_true(dec.frame_counter >= 1);
    pthread_barrier_wait(&dec.barrier_copied);  /* release decode thread */

    /* Signal exit and do one more barrier pair to let it terminate */
    dec.exit = true;
    /* The decode thread may be waiting at barrier_decoded or processing.
     * To safely terminate, we join — the thread will hit exit and do final
     * barrier waits. We hit those barriers too. */
    pthread_barrier_wait(&dec.barrier_decoded);
    pthread_barrier_wait(&dec.barrier_copied);

    pthread_join(tid, NULL);

    assert_true(dec.frame_counter >= 1);

    pthread_barrier_destroy(&dec.barrier_decoded);
    pthread_barrier_destroy(&dec.barrier_copied);
    pthread_mutex_destroy(&dec.start_mutex);
    pthread_cond_destroy(&dec.start_cond);
    close_shared_ffmpeg(&dec);
}

/* =========================================================================
 * Test: close_ffmpeg_source with full context from open_ffmpeg_source
 * ========================================================================= */

static void test_close_ffmpeg_source_full(void **state)
{
    (void)state;
    struct dvledtx_context app;
    fill_app_16x16(&app, 1);

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.idx = 0;
    ctx.app = &app;

    int ret = load_video_source(&ctx, TEST_VIDEO_PATH);
    assert_int_equal(ret, 0);
    assert_true(ctx.use_ffmpeg);

    close_ffmpeg_source(&ctx);
    assert_null(ctx.fmt_ctx);
    assert_null(ctx.codec_ctx);
    assert_null(ctx.sws_ctx);
    assert_null(ctx.av_frame);
    assert_null(ctx.yuv_frame);
    assert_null(ctx.av_packet);
}

/* =========================================================================
 * Test: close_shared_ffmpeg with full context from open_shared_ffmpeg
 * ========================================================================= */

static void test_close_shared_ffmpeg_full(void **state)
{
    (void)state;
    struct dvledtx_context app;
    fill_app_16x16(&app, 1);

    struct shared_decode_ctx dec;
    memset(&dec, 0, sizeof(dec));
    dec.app = &app;

    int ret = open_shared_ffmpeg(&dec, TEST_VIDEO_PATH);
    assert_int_equal(ret, 0);

    close_shared_ffmpeg(&dec);
    assert_null(dec.fmt_ctx);
    assert_null(dec.codec_ctx);
    assert_null(dec.sws_ctx);
    assert_null(dec.av_frame);
    assert_null(dec.yuv_frame);
    assert_null(dec.av_packet);
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void)
{
    logger_init_default();

    /* Generate the tiny MPEG2 test video used by several tests */
    if (generate_test_video() != 0) {
        fprintf(stderr, "FATAL: cannot generate test video\n");
        return 1;
    }

    const struct CMUnitTest tests[] = {
        /* open_shared_ffmpeg */
        cmocka_unit_test(test_open_shared_ffmpeg_success),
        cmocka_unit_test(test_open_shared_ffmpeg_bad_file_fails),

        /* load_video_source → open_ffmpeg_source (static) */
        cmocka_unit_test(test_load_video_source_mp4_calls_open_ffmpeg_source),
        cmocka_unit_test(test_load_video_source_nonexistent_mp4_returns_minus1),

        /* shared_decode_thread */
        cmocka_unit_test(test_shared_decode_thread_decodes_frames),

        /* close paths with fully-populated contexts */
        cmocka_unit_test(test_close_ffmpeg_source_full),
        cmocka_unit_test(test_close_shared_ffmpeg_full),
    };

    int result = cmocka_run_group_tests(tests, NULL, NULL);

    unlink(TEST_VIDEO_PATH);
    return result;
}
