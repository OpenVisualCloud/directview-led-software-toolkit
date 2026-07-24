/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 *
 * Unit tests for src/mtl/mtl_tx.c using cmocka.
 *
 * Strategy
 * --------
 * All three functions are pure computation — no MTL hardware is accessed:
 *
 *   get_transport_format()    — pure switch on AVPixelFormat
 *   get_st_fps()              — pure switch on integer fps
 *   mtl_copy_crop_to_frame()  — memcpy loops; dst->addr[] are plain malloc buffers
 *
 * Compiled with -DENABLE_MTL_TX so mtl_tx.c and mtl_tx.h are included.
 *
 * Covered
 * -------
 *   get_transport_format()    — all 4 known formats + unknown/fallback
 *   get_st_fps()              — 25, 30, 50, 60, unknown/fallback
 *   mtl_copy_crop_to_frame()  — null guards, YUV422P10LE full + crop offset,
 *                               YUV420P (vertical chroma subsampling),
 *                               YUV444P10LE (no subsampling)
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>

#include "app_context.h"
#include "core/session_manager.h"
#include "mtl/mtl_tx.h"
#include "util/logger.h"

/* =========================================================================
 * get_transport_format
 * ========================================================================= */

static void test_get_transport_format_yuv422p10le(void **state)
{
    (void)state;
    assert_int_equal(get_transport_format(AV_PIX_FMT_YUV422P10LE),
                     ST20_FMT_YUV_422_10BIT);
}

static void test_get_transport_format_yuv420p(void **state)
{
    (void)state;
    assert_int_equal(get_transport_format(AV_PIX_FMT_YUV420P),
                     ST20_FMT_YUV_420_8BIT);
}

static void test_get_transport_format_yuv444p10le(void **state)
{
    (void)state;
    assert_int_equal(get_transport_format(AV_PIX_FMT_YUV444P10LE),
                     ST20_FMT_YUV_444_10BIT);
}

static void test_get_transport_format_gbrp10le(void **state)
{
    (void)state;
    assert_int_equal(get_transport_format(AV_PIX_FMT_GBRP10LE),
                     ST20_FMT_RGB_10BIT);
}

static void test_get_transport_format_yuv422p12le(void **state)
{
    (void)state;
    assert_int_equal(get_transport_format(AV_PIX_FMT_YUV422P12LE),
                     ST20_FMT_YUV_422_12BIT);
}

static void test_get_transport_format_yuv444p12le(void **state)
{
    (void)state;
    assert_int_equal(get_transport_format(AV_PIX_FMT_YUV444P12LE),
                     ST20_FMT_YUV_444_12BIT);
}

static void test_get_transport_format_gbrp12le(void **state)
{
    (void)state;
    assert_int_equal(get_transport_format(AV_PIX_FMT_GBRP12LE),
                     ST20_FMT_RGB_12BIT);
}

static void test_get_transport_format_unknown_returns_error(void **state)
{
    (void)state;
    /* AV_PIX_FMT_NONE hits the default branch → error (-1) */
    assert_int_equal((int)get_transport_format(AV_PIX_FMT_NONE), -1);
}

/* =========================================================================
 * get_input_format — 12-bit formats
 * ========================================================================= */

static void test_get_input_format_yuv422p12le(void **state)
{
    (void)state;
    assert_int_equal(get_input_format(AV_PIX_FMT_YUV422P12LE),
                     ST_FRAME_FMT_YUV422PLANAR12LE);
}

static void test_get_input_format_yuv444p12le(void **state)
{
    (void)state;
    assert_int_equal(get_input_format(AV_PIX_FMT_YUV444P12LE),
                     ST_FRAME_FMT_YUV444PLANAR12LE);
}

static void test_get_input_format_gbrp12le(void **state)
{
    (void)state;
    assert_int_equal(get_input_format(AV_PIX_FMT_GBRP12LE),
                     ST_FRAME_FMT_GBRPLANAR12LE);
}

/* =========================================================================
 * get_input_format — 10-bit / 8-bit formats + unknown
 * ========================================================================= */

static void test_get_input_format_yuv422p10le(void **state)
{
    (void)state;
    assert_int_equal(get_input_format(AV_PIX_FMT_YUV422P10LE),
                     ST_FRAME_FMT_YUV422PLANAR10LE);
}

static void test_get_input_format_yuv420p(void **state)
{
    (void)state;
    assert_int_equal(get_input_format(AV_PIX_FMT_YUV420P),
                     ST_FRAME_FMT_YUV420CUSTOM8);
}

static void test_get_input_format_yuv444p10le(void **state)
{
    (void)state;
    assert_int_equal(get_input_format(AV_PIX_FMT_YUV444P10LE),
                     ST_FRAME_FMT_YUV444PLANAR10LE);
}

static void test_get_input_format_gbrp10le(void **state)
{
    (void)state;
    assert_int_equal(get_input_format(AV_PIX_FMT_GBRP10LE),
                     ST_FRAME_FMT_GBRPLANAR10LE);
}

static void test_get_input_format_unknown_returns_error(void **state)
{
    (void)state;
    assert_int_equal((int)get_input_format(AV_PIX_FMT_NONE), -1);
}

/* =========================================================================
 * get_st_fps
 * ========================================================================= */

static void test_get_st_fps_25(void **state)
{
    (void)state;
    assert_int_equal(get_st_fps(25), ST_FPS_P25);
}

static void test_get_st_fps_30(void **state)
{
    (void)state;
    assert_int_equal(get_st_fps(30), ST_FPS_P30);
}

static void test_get_st_fps_50(void **state)
{
    (void)state;
    assert_int_equal(get_st_fps(50), ST_FPS_P50);
}

static void test_get_st_fps_60(void **state)
{
    (void)state;
    assert_int_equal(get_st_fps(60), ST_FPS_P60);
}

static void test_get_st_fps_unknown_defaults_to_30(void **state)
{
    (void)state;
    assert_int_equal(get_st_fps(24),  ST_FPS_P30);
    assert_int_equal(get_st_fps(0),   ST_FPS_P30);
    assert_int_equal(get_st_fps(120), ST_FPS_P30);
}

/* =========================================================================
 * mtl_copy_crop_to_frame — helpers
 * ========================================================================= */

/* Allocate an AVFrame filled with a solid colour per plane. */
static AVFrame *make_src_frame(enum AVPixelFormat fmt, int w, int h, uint8_t base)
{
    AVFrame *f = av_frame_alloc();
    if (!f) return NULL;
    f->format = fmt;
    f->width  = w;
    f->height = h;
    if (av_frame_get_buffer(f, 32) < 0) { av_frame_free(&f); return NULL; }
    av_frame_make_writable(f);

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    for (int p = 0; p < AV_NUM_DATA_POINTERS && f->data[p]; p++) {
        int ph = (p == 0) ? h : h >> desc->log2_chroma_h;
        memset(f->data[p], base + (uint8_t)(p * 0x11),
               (size_t)f->linesize[p] * ph);
    }
    return f;
}

/* Allocate an st_frame with plain malloc-backed addr[] plane buffers. */
static struct st_frame *make_dst_frame(enum AVPixelFormat fmt,
                                        int crop_w, int crop_h,
                                        uint8_t **plane_bufs)
{
    struct st_frame *f = calloc(1, sizeof(*f));
    if (!f) return NULL;

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    int bps      = (desc->comp[0].depth + 7) / 8;
    int chroma_w = crop_w >> desc->log2_chroma_w;
    int chroma_h = crop_h >> desc->log2_chroma_h;

    plane_bufs[0] = calloc(1, (size_t)crop_w  * crop_h  * bps);
    plane_bufs[1] = calloc(1, (size_t)chroma_w * chroma_h * bps);
    plane_bufs[2] = calloc(1, (size_t)chroma_w * chroma_h * bps);

    f->addr[0] = plane_bufs[0];
    f->addr[1] = plane_bufs[1];
    f->addr[2] = plane_bufs[2];
    f->addr[3] = NULL;
    return f;
}

static void free_dst_frame(struct st_frame *f, uint8_t **bufs)
{
    for (int i = 0; i < 3; i++) free(bufs[i]);
    free(f);
}

/* Verify every byte of the calloc'd destination planes is still zero, i.e.
 * mtl_copy_crop_to_frame() wrote nothing (used by the invalid-rect tests to
 * assert the early-return contract, not merely the absence of a crash). */
static bool dst_planes_all_zero(uint8_t **bufs, enum AVPixelFormat fmt,
                                int crop_w, int crop_h)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    int bps      = (desc->comp[0].depth + 7) / 8;
    int chroma_w = crop_w >> desc->log2_chroma_w;
    int chroma_h = crop_h >> desc->log2_chroma_h;
    size_t sizes[3] = {
        (size_t)crop_w   * crop_h   * bps,
        (size_t)chroma_w * chroma_h * bps,
        (size_t)chroma_w * chroma_h * bps,
    };
    for (int p = 0; p < 3; p++)
        for (size_t i = 0; i < sizes[p]; i++)
            if (bufs[p][i] != 0) return false;
    return true;
}

/* =========================================================================
 * mtl_copy_crop_to_frame — null guards
 * ========================================================================= */

static void test_mtl_copy_null_dst_no_crash(void **state)
{
    (void)state;
    AVFrame *src = make_src_frame(AV_PIX_FMT_YUV422P10LE, 16, 16, 0xAA);
    assert_non_null(src);
    mtl_copy_crop_to_frame(NULL, src, 0, 0, 16, 16, AV_PIX_FMT_YUV422P10LE);
    av_frame_free(&src);
}

static void test_mtl_copy_null_src_no_crash(void **state)
{
    (void)state;
    uint8_t *bufs[3] = {0};
    struct st_frame *dst = make_dst_frame(AV_PIX_FMT_YUV422P10LE, 16, 16, bufs);
    assert_non_null(dst);
    mtl_copy_crop_to_frame(dst, NULL, 0, 0, 16, 16, AV_PIX_FMT_YUV422P10LE);
    free_dst_frame(dst, bufs);
}

static void test_mtl_copy_null_planes_no_crash(void **state)
{
    (void)state;
    /* All addr[] = NULL — the per-plane if-guards skip every memcpy */
    struct st_frame dst;
    memset(&dst, 0, sizeof(dst));
    AVFrame *src = make_src_frame(AV_PIX_FMT_YUV422P10LE, 16, 16, 0xBB);
    assert_non_null(src);
    mtl_copy_crop_to_frame(&dst, src, 0, 0, 16, 16, AV_PIX_FMT_YUV422P10LE);
    av_frame_free(&src);
}

static void test_mtl_copy_invalid_crop_rect_no_copy(void **state)
{
    (void)state;
    uint8_t *bufs[3] = {0};
    struct st_frame *dst = make_dst_frame(AV_PIX_FMT_YUV422P10LE, 16, 16, bufs);
    assert_non_null(dst);
    AVFrame *src = make_src_frame(AV_PIX_FMT_YUV422P10LE, 16, 16, 0xCC);
    assert_non_null(src);
    /* crop_w <= 0 → invalid rect branch returns early without copying */
    mtl_copy_crop_to_frame(dst, src, 0, 0, 0, 16, AV_PIX_FMT_YUV422P10LE);
    /* Contract: nothing was written — dst planes stay zero-filled. */
    assert_true(dst_planes_all_zero(bufs, AV_PIX_FMT_YUV422P10LE, 16, 16));
    av_frame_free(&src);
    free_dst_frame(dst, bufs);
}

static void test_mtl_copy_crop_exceeds_source_no_copy(void **state)
{
    (void)state;
    uint8_t *bufs[3] = {0};
    struct st_frame *dst = make_dst_frame(AV_PIX_FMT_YUV422P10LE, 16, 16, bufs);
    assert_non_null(dst);
    AVFrame *src = make_src_frame(AV_PIX_FMT_YUV422P10LE, 16, 16, 0xDD);
    assert_non_null(src);
    /* crop_x + crop_w = 8 + 16 = 24 > src width 16 → exceeds-source branch */
    mtl_copy_crop_to_frame(dst, src, 8, 0, 16, 16, AV_PIX_FMT_YUV422P10LE);
    /* Contract: nothing was written — dst planes stay zero-filled. */
    assert_true(dst_planes_all_zero(bufs, AV_PIX_FMT_YUV422P10LE, 16, 16));
    av_frame_free(&src);
    free_dst_frame(dst, bufs);
}

/* =========================================================================
 * mtl_copy_crop_to_frame — data correctness
 * ========================================================================= */

static void test_mtl_copy_yuv422p10le_full_frame(void **state)
{
    (void)state;
    const int W = 16, H = 8;
    enum AVPixelFormat fmt = AV_PIX_FMT_YUV422P10LE;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    int bps = (desc->comp[0].depth + 7) / 8; /* 2 for 10LE */
    int cw  = W >> desc->log2_chroma_w;       /* W/2 for YUV422 */
    int ch  = H >> desc->log2_chroma_h;       /* H   (no vertical sub) */

    AVFrame *src = make_src_frame(fmt, W, H, 0xAA);
    assert_non_null(src);

    uint8_t *bufs[3] = {0};
    struct st_frame *dst = make_dst_frame(fmt, W, H, bufs);
    assert_non_null(dst);

    mtl_copy_crop_to_frame(dst, src, 0, 0, W, H, fmt);

    /* Y plane: tightly packed, no stride padding */
    for (int line = 0; line < H; line++)
        assert_memory_equal((uint8_t *)dst->addr[0] + line * W * bps,
                            src->data[0] + line * src->linesize[0],
                            (size_t)W * bps);

    /* Cb and Cr planes */
    for (int line = 0; line < ch; line++) {
        assert_memory_equal((uint8_t *)dst->addr[1] + line * cw * bps,
                            src->data[1] + line * src->linesize[1],
                            (size_t)cw * bps);
        assert_memory_equal((uint8_t *)dst->addr[2] + line * cw * bps,
                            src->data[2] + line * src->linesize[2],
                            (size_t)cw * bps);
    }

    av_frame_free(&src);
    free_dst_frame(dst, bufs);
}

static void test_mtl_copy_yuv422p10le_crop_offset(void **state)
{
    (void)state;
    /* Full frame: 32×16, crop right half: crop_x=16, crop_w=16 */
    const int W = 32, H = 16, CX = 16, CY = 0, CW = 16, CH = 16;
    enum AVPixelFormat fmt = AV_PIX_FMT_YUV422P10LE;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    int bps = (desc->comp[0].depth + 7) / 8;

    /* Left half → 0xAA, right half → 0xBB */
    AVFrame *src = av_frame_alloc();
    src->format = fmt; src->width = W; src->height = H;
    assert_int_equal(av_frame_get_buffer(src, 32), 0);
    av_frame_make_writable(src);
    for (int p = 0; p < AV_NUM_DATA_POINTERS && src->data[p]; p++) {
        int pw = (p == 0) ? W : W >> desc->log2_chroma_w;
        int ph = (p == 0) ? H : H >> desc->log2_chroma_h;
        for (int row = 0; row < ph; row++) {
            memset(src->data[p] + row * src->linesize[p],
                   0xAA, (size_t)(pw / 2) * bps);
            memset(src->data[p] + row * src->linesize[p] + (pw / 2) * bps,
                   0xBB, (size_t)(pw / 2) * bps);
        }
    }

    uint8_t *bufs[3] = {0};
    struct st_frame *dst = make_dst_frame(fmt, CW, CH, bufs);
    assert_non_null(dst);

    mtl_copy_crop_to_frame(dst, src, CX, CY, CW, CH, fmt);

    /* The right-half crop should contain only 0xBB bytes */
    int cw = CW >> desc->log2_chroma_w;
    int ch = CH >> desc->log2_chroma_h;
    for (int i = 0; i < CW * CH * bps; i++)
        assert_int_equal(((uint8_t *)dst->addr[0])[i], 0xBB);
    for (int i = 0; i < cw * ch * bps; i++) {
        assert_int_equal(((uint8_t *)dst->addr[1])[i], 0xBB);
        assert_int_equal(((uint8_t *)dst->addr[2])[i], 0xBB);
    }

    av_frame_free(&src);
    free_dst_frame(dst, bufs);
}

static void test_mtl_copy_yuv420p_full_frame(void **state)
{
    (void)state;
    /* YUV420P: both horizontal and vertical chroma subsampled */
    const int W = 16, H = 16;
    enum AVPixelFormat fmt = AV_PIX_FMT_YUV420P;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    int bps = 1; /* 8-bit */
    int cw  = W >> desc->log2_chroma_w;
    int ch  = H >> desc->log2_chroma_h;

    AVFrame *src = make_src_frame(fmt, W, H, 0xCC);
    assert_non_null(src);

    uint8_t *bufs[3] = {0};
    struct st_frame *dst = make_dst_frame(fmt, W, H, bufs);
    assert_non_null(dst);

    mtl_copy_crop_to_frame(dst, src, 0, 0, W, H, fmt);

    for (int line = 0; line < H; line++)
        assert_memory_equal((uint8_t *)dst->addr[0] + line * W * bps,
                            src->data[0] + line * src->linesize[0],
                            (size_t)W * bps);
    for (int line = 0; line < ch; line++) {
        assert_memory_equal((uint8_t *)dst->addr[1] + line * cw * bps,
                            src->data[1] + line * src->linesize[1],
                            (size_t)cw * bps);
        assert_memory_equal((uint8_t *)dst->addr[2] + line * cw * bps,
                            src->data[2] + line * src->linesize[2],
                            (size_t)cw * bps);
    }

    av_frame_free(&src);
    free_dst_frame(dst, bufs);
}

static void test_mtl_copy_yuv444p10le_full_frame(void **state)
{
    (void)state;
    /* YUV444P10LE: no subsampling — all three planes are W×H */
    const int W = 8, H = 8;
    enum AVPixelFormat fmt = AV_PIX_FMT_YUV444P10LE;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    int bps = (desc->comp[0].depth + 7) / 8;

    AVFrame *src = make_src_frame(fmt, W, H, 0xDD);
    assert_non_null(src);

    uint8_t *bufs[3] = {0};
    struct st_frame *dst = make_dst_frame(fmt, W, H, bufs);
    assert_non_null(dst);

    mtl_copy_crop_to_frame(dst, src, 0, 0, W, H, fmt);

    for (int p = 0; p < 3; p++)
        for (int line = 0; line < H; line++)
            assert_memory_equal((uint8_t *)dst->addr[p] + line * W * bps,
                                src->data[p] + line * src->linesize[p],
                                (size_t)W * bps);

    av_frame_free(&src);
    free_dst_frame(dst, bufs);
}

/* =========================================================================
 * Session lifecycle / send — guard and error paths (no MTL hardware)
 * ========================================================================= */

/* mtl_tx_session_free() with a NULL handle must be a safe no-op. */
static void test_mtl_tx_session_free_null_handle(void **state)
{
    (void)state;
    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.handle = NULL;
    mtl_tx_session_free(&ctx); /* must not crash */
    assert_null(ctx.handle);
}

/* mtl_tx_send_yuv_frame() returns -1 when the session handle is NULL. */
static void test_mtl_tx_send_yuv_frame_null_handle(void **state)
{
    (void)state;
    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    AVFrame *src = make_src_frame(AV_PIX_FMT_YUV422P10LE, 16, 16, 0xAA);
    assert_non_null(src);
    assert_int_equal(mtl_tx_send_yuv_frame(&ctx, src, 0, 0, 16, 16), -1);
    av_frame_free(&src);
}

/* mtl_tx_send_yuv_frame() returns -1 when the src frame is NULL. */
static void test_mtl_tx_send_yuv_frame_null_src(void **state)
{
    (void)state;
    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.handle = (st20p_tx_handle)0x1; /* non-NULL; guard trips on src == NULL first */
    assert_int_equal(mtl_tx_send_yuv_frame(&ctx, NULL, 0, 0, 16, 16), -1);
}

/* mtl_tx_send_raw_yuv() returns -1 for each guard combination. */
static void test_mtl_tx_send_raw_yuv_guards(void **state)
{
    (void)state;
    struct st20p_tx_ctx ctx;

    /* NULL handle */
    memset(&ctx, 0, sizeof(ctx));
    assert_int_equal(mtl_tx_send_raw_yuv(&ctx), -1);

    /* handle set but no source buffer */
    memset(&ctx, 0, sizeof(ctx));
    ctx.handle = (st20p_tx_handle)0x1;
    ctx.source_buffer = NULL;
    ctx.source_size = 100;
    assert_int_equal(mtl_tx_send_raw_yuv(&ctx), -1);

    /* handle + buffer set but zero source size */
    memset(&ctx, 0, sizeof(ctx));
    ctx.handle = (st20p_tx_handle)0x1;
    uint8_t buf[16] = {0};
    ctx.source_buffer = buf;
    ctx.source_size = 0;
    assert_int_equal(mtl_tx_send_raw_yuv(&ctx), -1);
}

/* mtl_tx_session_create() returns -1 (before touching MTL) when the pixel
 * format is unsupported, exercising the ops-setup and error-return path. */
static void test_mtl_tx_session_create_unsupported_fmt(void **state)
{
    (void)state;
    struct dvledtx_context app;
    memset(&app, 0, sizeof(app));
    assert_int_equal(dvledtx_context_alloc(&app, 1, 1), 0);
    strncpy(app.nics[0].port, "0000:06:00.0", sizeof(app.nics[0].port) - 1);
    app.fps           = 30;
    app.fmt           = AV_PIX_FMT_NONE; /* unsupported → get_*_format() == -1 */
    app.udp_port      = 20000;
    app.payload_type  = 96;
    app.session_net[0].udp_port     = 0; /* exercise app-default fallback */
    app.session_net[0].payload_type = 0;
    app.session_net[0].nic_index    = 0;

    session_manager_t mgr;
    memset(&mgr, 0, sizeof(mgr));

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.idx         = 0;
    ctx.app         = &app;
    ctx.crop_width  = 16;
    ctx.crop_height = 16;
    strncpy(ctx.session_name, "test_sess", sizeof(ctx.session_name) - 1);

    assert_int_equal(mtl_tx_session_create(&mgr, &ctx, &app, 0), -1);
    assert_null(ctx.handle);

    dvledtx_context_free(&app);
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void)
{
    logger_init_default();

    const struct CMUnitTest tests[] = {
        /* get_transport_format */
        cmocka_unit_test(test_get_transport_format_yuv422p10le),
        cmocka_unit_test(test_get_transport_format_yuv420p),
        cmocka_unit_test(test_get_transport_format_yuv444p10le),
        cmocka_unit_test(test_get_transport_format_gbrp10le),
        cmocka_unit_test(test_get_transport_format_yuv422p12le),
        cmocka_unit_test(test_get_transport_format_yuv444p12le),
        cmocka_unit_test(test_get_transport_format_gbrp12le),
        cmocka_unit_test(test_get_transport_format_unknown_returns_error),

        /* get_input_format — 12-bit */
        cmocka_unit_test(test_get_input_format_yuv422p12le),
        cmocka_unit_test(test_get_input_format_yuv444p12le),
        cmocka_unit_test(test_get_input_format_gbrp12le),

        /* get_input_format — 10-bit / 8-bit + unknown */
        cmocka_unit_test(test_get_input_format_yuv422p10le),
        cmocka_unit_test(test_get_input_format_yuv420p),
        cmocka_unit_test(test_get_input_format_yuv444p10le),
        cmocka_unit_test(test_get_input_format_gbrp10le),
        cmocka_unit_test(test_get_input_format_unknown_returns_error),

        /* get_st_fps */
        cmocka_unit_test(test_get_st_fps_25),
        cmocka_unit_test(test_get_st_fps_30),
        cmocka_unit_test(test_get_st_fps_50),
        cmocka_unit_test(test_get_st_fps_60),
        cmocka_unit_test(test_get_st_fps_unknown_defaults_to_30),

        /* mtl_copy_crop_to_frame — null guards */
        cmocka_unit_test(test_mtl_copy_null_dst_no_crash),
        cmocka_unit_test(test_mtl_copy_null_src_no_crash),
        cmocka_unit_test(test_mtl_copy_null_planes_no_crash),
        cmocka_unit_test(test_mtl_copy_invalid_crop_rect_no_copy),
        cmocka_unit_test(test_mtl_copy_crop_exceeds_source_no_copy),

        /* mtl_copy_crop_to_frame — data correctness */
        cmocka_unit_test(test_mtl_copy_yuv422p10le_full_frame),
        cmocka_unit_test(test_mtl_copy_yuv422p10le_crop_offset),
        cmocka_unit_test(test_mtl_copy_yuv420p_full_frame),
        cmocka_unit_test(test_mtl_copy_yuv444p10le_full_frame),

        /* session lifecycle / send — guard and error paths */
        cmocka_unit_test(test_mtl_tx_session_free_null_handle),
        cmocka_unit_test(test_mtl_tx_send_yuv_frame_null_handle),
        cmocka_unit_test(test_mtl_tx_send_yuv_frame_null_src),
        cmocka_unit_test(test_mtl_tx_send_raw_yuv_guards),
        cmocka_unit_test(test_mtl_tx_session_create_unsupported_fmt),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
