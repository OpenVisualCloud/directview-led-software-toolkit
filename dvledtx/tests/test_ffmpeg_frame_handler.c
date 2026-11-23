/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 *
 * Unit tests for src/ffmpeg/ffmpeg_frame_handler.c using cmocka.
 *
 * Covered:
 *   convert_frame_format() — NULL-guard, successful sws_scale call,
 *                            threaded sws_scale_frame path and the
 *                            single-threaded fallbacks
 *   crop_yuv_frame()       — NULL-guard, correct pixel copy from full-width
 *                            frame into crop-sized frame
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include "ffmpeg/ffmpeg_frame_handler.h"

/* ==========================================================================
 * convert_frame_format
 * ========================================================================== */

static void test_convert_frame_format_null_sws_returns_error(void **state) {
    (void)state;
    AVFrame* src = av_frame_alloc();
    AVFrame* dst = av_frame_alloc();
    int ret = convert_frame_format(NULL, src, 16, dst);
    assert_true(ret < 0);
    av_frame_free(&src);
    av_frame_free(&dst);
}

static void test_convert_frame_format_null_src_returns_error(void **state) {
    (void)state;
    struct SwsContext* sws = sws_getContext(
        16, 16, AV_PIX_FMT_YUV420P, 16, 16, AV_PIX_FMT_YUV422P10LE,
        SWS_FAST_BILINEAR, NULL, NULL, NULL);
    assert_non_null(sws);
    AVFrame* dst = av_frame_alloc();
    int ret = convert_frame_format(sws, NULL, 16, dst);
    assert_true(ret < 0);
    av_frame_free(&dst);
    sws_freeContext(sws);
}

static void test_convert_frame_format_null_dst_returns_error(void **state) {
    (void)state;
    struct SwsContext* sws = sws_getContext(
        16, 16, AV_PIX_FMT_YUV420P, 16, 16, AV_PIX_FMT_YUV422P10LE,
        SWS_FAST_BILINEAR, NULL, NULL, NULL);
    assert_non_null(sws);
    AVFrame* src = av_frame_alloc();
    int ret = convert_frame_format(sws, src, 16, NULL);
    assert_true(ret < 0);
    av_frame_free(&src);
    sws_freeContext(sws);
}

static void test_convert_frame_format_success(void **state) {
    (void)state;
    const int W = 64, H = 16;
    struct SwsContext* sws = sws_getContext(
        W, H, AV_PIX_FMT_YUV420P, W, H, AV_PIX_FMT_YUV422P10LE,
        SWS_FAST_BILINEAR, NULL, NULL, NULL);
    assert_non_null(sws);

    AVFrame* src = av_frame_alloc();
    src->format = AV_PIX_FMT_YUV420P;
    src->width  = W; src->height = H;
    assert_int_equal(av_frame_get_buffer(src, 32), 0);
    av_frame_make_writable(src);
    /* Fill with recognisable luma pattern */
    memset(src->data[0], 0x80, (size_t)src->linesize[0] * H);

    AVFrame* dst = av_frame_alloc();
    dst->format = AV_PIX_FMT_YUV422P10LE;
    dst->width  = W; dst->height = H;
    assert_int_equal(av_frame_get_buffer(dst, 32), 0);

    int rows = convert_frame_format(sws, src, H, dst);
    assert_true(rows > 0);

    av_frame_free(&src);
    av_frame_free(&dst);
    sws_freeContext(sws);
}

/* --------------------------------------------------------------------------
 * Helper: build a slice-threaded SwsContext the way ffmpeg_decoder.c does.
 * sws_getContext() cannot be used because the "threads" option has to be set
 * between allocation and initialisation.
 * -------------------------------------------------------------------------- */
static struct SwsContext* alloc_threaded_sws(int sw, int sh, enum AVPixelFormat sf,
                                             int dw, int dh, enum AVPixelFormat df,
                                             int threads) {
    struct SwsContext* c = sws_alloc_context();
    if (!c) return NULL;
    av_opt_set_int(c, "srcw",       sw,                0);
    av_opt_set_int(c, "srch",       sh,                0);
    av_opt_set_int(c, "src_format", sf,                0);
    av_opt_set_int(c, "dstw",       dw,                0);
    av_opt_set_int(c, "dsth",       dh,                0);
    av_opt_set_int(c, "dst_format", df,                0);
    av_opt_set_int(c, "sws_flags",  SWS_FAST_BILINEAR, 0);
    av_opt_set_int(c, "threads",    threads,           0);
    if (sws_init_context(c, NULL, NULL) < 0) { sws_freeContext(c); return NULL; }
    return c;
}

/* A refcounted destination takes the sws_scale_frame() path, which reports 0
 * on success rather than a row count. convert_frame_format must normalise that
 * to dst->height so callers testing "rows <= 0" still see success. */
static void test_convert_frame_format_threaded_returns_dst_height(void **state) {
    (void)state;
    const int SW = 64, SH = 32, DW = 128, DH = 64;
    struct SwsContext* sws = alloc_threaded_sws(
        SW, SH, AV_PIX_FMT_GBRP12LE, DW, DH, AV_PIX_FMT_YUV444P12LE, 4);
    assert_non_null(sws);

    AVFrame* src = av_frame_alloc();
    src->format = AV_PIX_FMT_GBRP12LE; src->width = SW; src->height = SH;
    assert_int_equal(av_frame_get_buffer(src, 32), 0);
    assert_int_equal(av_frame_make_writable(src), 0);
    for (int p = 0; p < 3; p++)
        memset(src->data[p], 0x5A, (size_t)src->linesize[p] * SH);

    AVFrame* dst = av_frame_alloc();
    dst->format = AV_PIX_FMT_YUV444P12LE; dst->width = DW; dst->height = DH;
    assert_int_equal(av_frame_get_buffer(dst, 32), 0);
    assert_int_equal(av_frame_make_writable(dst), 0);
    memset(dst->data[0], 0x00, (size_t)dst->linesize[0] * DH);

    int rows = convert_frame_format(sws, src, SH, dst);
    assert_int_equal(rows, DH);
    assert_non_null(dst->buf[0]);   /* must not have been reallocated away */

    /* The destination was actually written, not left as the memset zeros. */
    bool wrote = false;
    for (int b = 0; b < DW * 2 && !wrote; b++)
        if (dst->data[0][b] != 0x00) wrote = true;
    assert_true(wrote);

    av_frame_free(&src);
    av_frame_free(&dst);
    sws_freeContext(sws);
}

/* A destination without an AVBuffer (av_image_alloc / av_image_fill_arrays,
 * as used by the raw-YUV path) must fall back to sws_scale, because
 * sws_scale_frame would silently reallocate it. */
static void test_convert_frame_format_non_refcounted_dst_falls_back(void **state) {
    (void)state;
    const int W = 64, H = 32;
    struct SwsContext* sws = alloc_threaded_sws(
        W, H, AV_PIX_FMT_YUV420P, W, H, AV_PIX_FMT_YUV422P10LE, 4);
    assert_non_null(sws);

    AVFrame* src = av_frame_alloc();
    src->format = AV_PIX_FMT_YUV420P; src->width = W; src->height = H;
    assert_int_equal(av_frame_get_buffer(src, 32), 0);
    assert_int_equal(av_frame_make_writable(src), 0);
    memset(src->data[0], 0x80, (size_t)src->linesize[0] * H);

    AVFrame* dst = av_frame_alloc();
    dst->format = AV_PIX_FMT_YUV422P10LE; dst->width = W; dst->height = H;
    assert_true(av_image_alloc(dst->data, dst->linesize, W, H,
                               AV_PIX_FMT_YUV422P10LE, 32) > 0);
    assert_null(dst->buf[0]);
    uint8_t* original_plane = dst->data[0];

    int rows = convert_frame_format(sws, src, H, dst);
    assert_true(rows > 0);
    /* The caller-owned buffer must still be the conversion target. */
    assert_ptr_equal(dst->data[0], original_plane);
    assert_null(dst->buf[0]);

    av_freep(&dst->data[0]);
    av_frame_free(&dst);
    av_frame_free(&src);
    sws_freeContext(sws);
}

/* A partial source slice cannot use the frame API (it needs the whole frame),
 * so it must fall back to sws_scale and report the rows it produced. */
static void test_convert_frame_format_partial_slice_falls_back(void **state) {
    (void)state;
    const int W = 64, H = 32;
    struct SwsContext* sws = alloc_threaded_sws(
        W, H, AV_PIX_FMT_YUV420P, W, H, AV_PIX_FMT_YUV422P10LE, 4);
    assert_non_null(sws);

    AVFrame* src = av_frame_alloc();
    src->format = AV_PIX_FMT_YUV420P; src->width = W; src->height = H;
    assert_int_equal(av_frame_get_buffer(src, 32), 0);
    assert_int_equal(av_frame_make_writable(src), 0);
    memset(src->data[0], 0x80, (size_t)src->linesize[0] * H);

    AVFrame* dst = av_frame_alloc();
    dst->format = AV_PIX_FMT_YUV422P10LE; dst->width = W; dst->height = H;
    assert_int_equal(av_frame_get_buffer(dst, 32), 0);

    /* Half the source height — must not be reported as a full-frame convert. */
    int rows = convert_frame_format(sws, src, H / 2, dst);
    assert_true(rows > 0);
    assert_true(rows < H);

    av_frame_free(&src);
    av_frame_free(&dst);
    sws_freeContext(sws);
}

/* Repeated conversions through the frame API must keep targeting the same
 * buffer: sws_frame_start()/sws_frame_end() ref and unref the destination on
 * every call, so a refcount slip would show up as a reallocated plane. */
static void test_convert_frame_format_threaded_is_reusable(void **state) {
    (void)state;
    const int W = 64, H = 32;
    struct SwsContext* sws = alloc_threaded_sws(
        W, H, AV_PIX_FMT_YUV420P, W, H, AV_PIX_FMT_YUV444P12LE, 4);
    assert_non_null(sws);

    AVFrame* src = av_frame_alloc();
    src->format = AV_PIX_FMT_YUV420P; src->width = W; src->height = H;
    assert_int_equal(av_frame_get_buffer(src, 32), 0);
    assert_int_equal(av_frame_make_writable(src), 0);
    memset(src->data[0], 0x40, (size_t)src->linesize[0] * H);

    AVFrame* dst = av_frame_alloc();
    dst->format = AV_PIX_FMT_YUV444P12LE; dst->width = W; dst->height = H;
    assert_int_equal(av_frame_get_buffer(dst, 32), 0);
    uint8_t*   plane = dst->data[0];
    AVBufferRef* buf = dst->buf[0];

    for (int i = 0; i < 8; i++)
        assert_int_equal(convert_frame_format(sws, src, H, dst), H);

    assert_ptr_equal(dst->data[0], plane);
    assert_ptr_equal(dst->buf[0], buf);

    av_frame_free(&src);
    av_frame_free(&dst);
    sws_freeContext(sws);
}

/* ==========================================================================
 * crop_yuv_frame
 * ========================================================================== */

static void test_crop_yuv_frame_null_dst_returns_error(void **state) {
    (void)state;
    AVFrame* src = av_frame_alloc();
    int ret = crop_yuv_frame(NULL, src, 0, 0, 8, 8, AV_PIX_FMT_YUV422P10LE);
    assert_int_equal(ret, -1);
    av_frame_free(&src);
}

static void test_crop_yuv_frame_null_src_returns_error(void **state) {
    (void)state;
    AVFrame* dst = av_frame_alloc();
    int ret = crop_yuv_frame(dst, NULL, 0, 0, 8, 8, AV_PIX_FMT_YUV422P10LE);
    assert_int_equal(ret, -1);
    av_frame_free(&dst);
}

static void test_crop_yuv_frame_unknown_format_returns_error(void **state) {
    (void)state;
    AVFrame* src = av_frame_alloc();
    AVFrame* dst = av_frame_alloc();
    int ret = crop_yuv_frame(dst, src, 0, 0, 8, 8, AV_PIX_FMT_NONE);
    assert_int_equal(ret, -1);
    av_frame_free(&src);
    av_frame_free(&dst);
}

static void test_crop_yuv_frame_copies_correct_pixels(void **state) {
    (void)state;
    /* Full-width frame 32x16 yuv422p10le.  Crop the right half (x=16, w=16). */
    const int FW = 32, FH = 16;
    const int CX = 16, CY = 0, CW = 16, CH = 16;
    enum AVPixelFormat fmt = AV_PIX_FMT_YUV422P10LE;

    AVFrame* src = av_frame_alloc();
    src->format = fmt; src->width = FW; src->height = FH;
    assert_int_equal(av_frame_get_buffer(src, 1), 0);
    av_frame_make_writable(src);

    /* Fill left half with 0x11 and right half with 0x22 (per-byte pattern).
     * yuv422p10le stores 10-bit values in 16-bit little-endian words. */
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    int bps = (desc->comp[0].depth + 7) / 8; /* 2 for 10-bit */
    for (int line = 0; line < FH; line++) {
        uint8_t* row = src->data[0] + line * src->linesize[0];
        memset(row,               0x11, (size_t)(FW / 2) * bps);
        memset(row + (FW/2)*bps,  0x22, (size_t)(FW / 2) * bps);
    }
    /* U and V chroma — 422 so same height, half width */
    for (int line = 0; line < FH; line++) {
        uint8_t* u = src->data[1] + line * src->linesize[1];
        uint8_t* v = src->data[2] + line * src->linesize[2];
        memset(u,               0x33, (size_t)(FW/2 / 2) * bps);
        memset(u + (FW/2/2)*bps, 0x44, (size_t)(FW/2 / 2) * bps);
        memset(v,               0x55, (size_t)(FW/2 / 2) * bps);
        memset(v + (FW/2/2)*bps, 0x66, (size_t)(FW/2 / 2) * bps);
    }

    AVFrame* dst = av_frame_alloc();
    dst->format = fmt; dst->width = CW; dst->height = CH;
    assert_int_equal(av_frame_get_buffer(dst, 1), 0);
    av_frame_make_writable(dst);
    memset(dst->data[0], 0x00, (size_t)dst->linesize[0] * CH);

    int ret = crop_yuv_frame(dst, src, CX, CY, CW, CH, fmt);
    assert_int_equal(ret, 0);

    /* All luma pixels in the crop (right half) should be 0x22 */
    for (int line = 0; line < CH; line++) {
        uint8_t* row = dst->data[0] + line * dst->linesize[0];
        for (int px = 0; px < CW * bps; px++)
            assert_int_equal(row[px], 0x22);
    }

    av_frame_free(&src);
    av_frame_free(&dst);
}

static void test_crop_yuv_frame_origin_crop(void **state) {
    (void)state;
    /* Crop top-left quadrant x=0,y=0,w=16,h=8 from a 32x16 frame */
    const int FW = 32, FH = 16;
    const int CX = 0,  CY = 0, CW = 16, CH = 8;
    enum AVPixelFormat fmt = AV_PIX_FMT_YUV422P10LE;

    AVFrame* src = av_frame_alloc();
    src->format = fmt; src->width = FW; src->height = FH;
    assert_int_equal(av_frame_get_buffer(src, 1), 0);
    av_frame_make_writable(src);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    int bps = (desc->comp[0].depth + 7) / 8;

    /* Fill entire luma with 0xAB */
    for (int line = 0; line < FH; line++)
        memset(src->data[0] + line * src->linesize[0], 0xAB,
               (size_t)FW * bps);

    AVFrame* dst = av_frame_alloc();
    dst->format = fmt; dst->width = CW; dst->height = CH;
    assert_int_equal(av_frame_get_buffer(dst, 1), 0);

    int ret = crop_yuv_frame(dst, src, CX, CY, CW, CH, fmt);
    assert_int_equal(ret, 0);

    /* All output luma bytes should be 0xAB */
    for (int line = 0; line < CH; line++) {
        uint8_t* row = dst->data[0] + line * dst->linesize[0];
        for (int b = 0; b < CW * bps; b++)
            assert_int_equal(row[b], 0xAB);
    }

    av_frame_free(&src);
    av_frame_free(&dst);
}

/* ==========================================================================
 * main
 * ========================================================================== */

int main(void) {
    const struct CMUnitTest tests[] = {
        /* convert_frame_format */
        cmocka_unit_test(test_convert_frame_format_null_sws_returns_error),
        cmocka_unit_test(test_convert_frame_format_null_src_returns_error),
        cmocka_unit_test(test_convert_frame_format_null_dst_returns_error),
        cmocka_unit_test(test_convert_frame_format_success),
        cmocka_unit_test(test_convert_frame_format_threaded_returns_dst_height),
        cmocka_unit_test(test_convert_frame_format_non_refcounted_dst_falls_back),
        cmocka_unit_test(test_convert_frame_format_partial_slice_falls_back),
        cmocka_unit_test(test_convert_frame_format_threaded_is_reusable),

        /* crop_yuv_frame */
        cmocka_unit_test(test_crop_yuv_frame_null_dst_returns_error),
        cmocka_unit_test(test_crop_yuv_frame_null_src_returns_error),
        cmocka_unit_test(test_crop_yuv_frame_unknown_format_returns_error),
        cmocka_unit_test(test_crop_yuv_frame_copies_correct_pixels),
        cmocka_unit_test(test_crop_yuv_frame_origin_crop),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
