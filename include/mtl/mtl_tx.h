/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#pragma once

/*
 * mtl_tx.h — public API for the direct MTL pipeline TX path.
 *
 * mtl_tx_init() / mtl_tx_uninit() are declared unconditionally so that the
 * FFmpeg avdevice path can also pre-initialise MTL with ALL NIC ports before
 * opening any mtl_st20p session.  Without this, DPDK EAL is initialised by
 * the first avformat_write_header() call with only one port, causing
 * "mt_port_by_name … is not valid" for every subsequent NIC.
 *
 * All other symbols (format helpers, session create/free, send functions)
 * are only needed when ENABLE_MTL_TX is defined.
 *
 * Implementation: src/mtl/mtl_tx.c
 */

#include "mtl_api.h"
#include "st_pipeline_api.h"
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>

/* Forward declarations */
struct dvledtx_context;
struct st20p_tx_ctx;
typedef struct session_manager_s session_manager_t;

/* -------------------------------------------------------------------------
 * MTL library lifecycle — always available (FFmpeg and direct-MTL paths)
 * ---------------------------------------------------------------------- */

/*
 * mtl_tx_init() — initialise the MTL/DPDK library with ALL ports from app.
 *   Must be called once before any TX session (FFmpeg or direct) is opened.
 *   Stores the handle in manager->mtl.
 * Returns 0 on success, -1 on failure.
 */
int  mtl_tx_init(session_manager_t* manager, struct dvledtx_context* app);

/*
 * mtl_tx_uninit() — release the MTL library instance stored in manager->mtl.
 */
void mtl_tx_uninit(session_manager_t* manager);

#ifdef ENABLE_MTL_TX

/* -------------------------------------------------------------------------
 * Format mapping
 * ---------------------------------------------------------------------- */

/*
 * get_transport_format() — map AVPixelFormat → MTL st20_fmt (wire format).
 * Returns (enum st20_fmt)-1 on unsupported format.
 */
enum st20_fmt get_transport_format(enum AVPixelFormat fmt);

/*
 * get_input_format() — map AVPixelFormat → MTL st_frame_fmt (buffer layout).
 * Returns (enum st_frame_fmt)-1 on unsupported format.
 */
enum st_frame_fmt get_input_format(enum AVPixelFormat fmt);

/*
 * get_st_fps() — map integer fps → MTL enum st_fps.
 * Supports 25/30/50/60; any other value defaults to ST_FPS_P30 with a warning.
 */
enum st_fps get_st_fps(int fps);

/* -------------------------------------------------------------------------
 * Frame copy (crop from AVFrame into MTL DMA buffer)
 * ---------------------------------------------------------------------- */

/*
 * mtl_copy_crop_to_frame() — copy a crop rectangle from a decoded AVFrame
 * into an MTL st_frame DMA buffer (dst->addr[]).
 *
 * Handles all planar YUV formats (4:2:0, 4:2:2, 4:4:4) and GBRP at any
 * supported bit depth.
 */
void mtl_copy_crop_to_frame(struct st_frame* dst, const AVFrame* src,
                             int crop_x, int crop_y,
                             int crop_w, int crop_h,
                             enum AVPixelFormat fmt);

/* -------------------------------------------------------------------------
 * Per-session TX session lifecycle
 * ---------------------------------------------------------------------- */

/*
 * mtl_tx_session_create() — build st20p_tx_ops from ctx/app and call
 *   st20p_tx_create().  Sets ctx->handle and ctx->frame_size on success.
 * Returns 0 on success, -1 on failure.
 */
int  mtl_tx_session_create(session_manager_t* manager, struct st20p_tx_ctx* ctx,
                            struct dvledtx_context* app, int session_idx);

/*
 * mtl_tx_session_free() — call st20p_tx_free() and clear ctx->handle.
 */
void mtl_tx_session_free(struct st20p_tx_ctx* ctx);

/* -------------------------------------------------------------------------
 * Per-frame transmission
 * ---------------------------------------------------------------------- */

/*
 * mtl_tx_send_yuv_frame() — obtain a free MTL DMA TX frame, copy the crop
 *   strip from src via mtl_copy_crop_to_frame(), set the RTP timestamp, and
 *   put the frame back for transmission.
 *
 *   src    = full-width decoded AVFrame (from shared or per-session decoder).
 *   crop_* = crop rectangle for this session.
 *
 * Returns 0 on success, -1 on error.
 */
int mtl_tx_send_yuv_frame(struct st20p_tx_ctx* ctx, const AVFrame* src,
                           int crop_x, int crop_y, int crop_w, int crop_h);

/*
 * mtl_tx_send_raw_yuv() — obtain a free MTL DMA TX frame internally via
 *   st20p_tx_get_frame(), copy frame_size bytes from ctx->source_buffer
 *   (wrapping for loop playback), then put the frame for transmission.
 *
 * Returns 0 on success, -1 on error.
 */
int mtl_tx_send_raw_yuv(struct st20p_tx_ctx* ctx);

#endif /* ENABLE_MTL_TX */
