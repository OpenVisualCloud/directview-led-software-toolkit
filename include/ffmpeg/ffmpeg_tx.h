/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#pragma once

/*
 * ffmpeg_tx.h — public API for the FFmpeg avdevice TX path (mtl_st20p muxer).
 *
 * Implements video frame transmission via the Intel MTL "mtl_st20p" FFmpeg
 * muxer plugin (libavdevice).  This is the DEFAULT TX path used when
 * -DENABLE_MTL_TX is NOT set.
 *
 * NOT compiled when -DENABLE_MTL_TX is set — in that mode the MTL pipeline
 * API (st20p_tx_get_frame / st20p_tx_put_frame) is used instead and
 * libavdevice is not linked.  See src/mtl/mtl_tx.c for that path.
 *
 * Implementation: src/ffmpeg/ffmpeg_tx.c
 */

#ifndef ENABLE_MTL_TX

#include "core/session_manager.h"
#include <libavutil/frame.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Session lifetime
 * ---------------------------------------------------------------------- */

/*
 * open_ffmpeg_tx():
 *   Locates the mtl_st20p AVOutputFormat, allocates an AVFormatContext,
 *   sets NIC/IP/port options via AVOptions, adds a RAWVIDEO stream, then
 *   calls avformat_write_header() which triggers mtl_st20p_write_header()
 *   inside the plugin — this is where the MTL TX session and DMA ring
 *   buffers are actually allocated.
 *   Also pre-allocates enc_frame (crop-width scratch) and enc_pkt
 *   (contiguous packed frame buffer) whose size must equal MTL's frame_size.
 *
 * close_ffmpeg_tx():
 *   Writes the AVFormat trailer (mtl_st20p: no-op), frees the AVFormatContext
 *   (this destroys the MTL TX session internally), and releases enc_frame/pkt.
 */
int  open_ffmpeg_tx(struct st20p_tx_ctx* ctx);
void close_ffmpeg_tx(struct st20p_tx_ctx* ctx);

/* -------------------------------------------------------------------------
 * Frame transmission
 * ---------------------------------------------------------------------- */

/*
 * ffmpeg_tx_send_yuv_frame() — crop a strip from src and transmit via mtl_st20p.
 *
 *   src    = full-width shared yuv_frame produced by the decode thread.
 *   crop_* = crop rectangle for this session (pixels, from JSON config).
 *
 * Steps:
 *   1. crop_yuv_frame() — copy the strip from src into ctx->enc_frame.
 *   2. av_image_copy_to_buffer — pack planar enc_frame into enc_pkt->data
 *      as a contiguous byte stream (align=1, no stride padding).
 *   3. av_write_frame — hand enc_pkt to the mtl_st20p muxer which copies it
 *      into the MTL DMA TX ring; DPDK then sends it as ST 2110-20 RTP/UDP.
 *
 * Returns 0 on success, -1 on error.
 */
int ffmpeg_tx_send_yuv_frame(struct st20p_tx_ctx* ctx, const AVFrame* src,
                             int crop_x, int crop_y, int crop_w, int crop_h);

/*
 * ffmpeg_tx_send_raw_yuv() — transmit one frame from the raw YUV source buffer.
 *
 * Reads frame_size bytes sequentially from ctx->source_buffer (wrapping at
 * end for loop playback), packs into enc_pkt and calls av_write_frame.
 *
 * Returns 0 on success, -1 on error.
 */
int ffmpeg_tx_send_raw_yuv(struct st20p_tx_ctx* ctx);

#endif /* !ENABLE_MTL_TX */
