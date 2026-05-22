/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

/*
 * mtl_tx.c — Intel Media Transport Library (MTL) pipeline TX helpers.
 *
 * Provides format-mapping, frame-copy, session lifecycle, and frame-send
 * functions used exclusively by the direct MTL TX path (ENABLE_MTL_TX).
 * None of this file is compiled in the default FFmpeg-avdevice build.
 *
 * Functions:
 *   get_transport_format()    — AVPixelFormat → st20_fmt (wire format)
 *   get_input_format()        — AVPixelFormat → st_frame_fmt (buffer layout)
 *   get_st_fps()              — integer fps   → st_fps (MTL enum)
 *   mtl_copy_crop_to_frame()  — crop AVFrame luma/chroma into st_frame addr[]
 *   mtl_tx_init()             — initialise the MTL library instance
 *   mtl_tx_uninit()           — release the MTL library instance
 *   mtl_tx_session_create()   — create one ST20P TX session
 *   mtl_tx_session_free()     — destroy one ST20P TX session
 *   mtl_tx_send_yuv_frame()   — get MTL frame, copy crop, put frame
 *   mtl_tx_send_raw_yuv()     — get MTL frame, memcpy raw buffer, put frame
 */

#ifdef ENABLE_MTL_TX

#include "mtl/mtl_tx.h"
#include "app_context.h"
#include "core/session_manager.h"
#include "util/logger.h"
#include <libavutil/pixdesc.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* =========================================================================
 * get_input_format / get_transport_format
 * =========================================================================
 *
 * get_input_format:     AVPixelFormat → st_frame_fmt  (in-memory buffer layout)
 * get_transport_format: AVPixelFormat → st20_fmt      (on-wire ST 2110-20 packing)
 *
 * These are DIFFERENT enums in MTL. Passing a st_frame_fmt value into
 * transport_fmt causes the numeric value to be reinterpreted as a different
 * wire format (e.g. ST_FRAME_FMT_YUV444PLANAR10LE=8 → ST20_FMT_RGB_8BIT=8).
 */
enum st_frame_fmt get_input_format(enum AVPixelFormat fmt) {
  switch (fmt) {
    case AV_PIX_FMT_YUV422P10LE: return ST_FRAME_FMT_YUV422PLANAR10LE;
    case AV_PIX_FMT_YUV420P:     return ST_FRAME_FMT_YUV420CUSTOM8;
    case AV_PIX_FMT_YUV444P10LE: return ST_FRAME_FMT_YUV444PLANAR10LE;
    case AV_PIX_FMT_GBRP10LE:    return ST_FRAME_FMT_GBRPLANAR10LE;
    default:
      LOG_ERROR("get_input_format: unsupported AVPixelFormat %d", fmt);
      return (enum st_frame_fmt)-1;
  }
}

enum st20_fmt get_transport_format(enum AVPixelFormat fmt) {
  switch (fmt) {
    case AV_PIX_FMT_YUV422P10LE: return ST20_FMT_YUV_422_10BIT;
    case AV_PIX_FMT_YUV420P:     return ST20_FMT_YUV_420_8BIT;
    case AV_PIX_FMT_YUV444P10LE: return ST20_FMT_YUV_444_10BIT;
    case AV_PIX_FMT_GBRP10LE:    return ST20_FMT_RGB_10BIT;
    default:
      LOG_ERROR("get_transport_format: unsupported AVPixelFormat %d", fmt);
      return (enum st20_fmt)-1;
  }
}

/* =========================================================================
 * get_st_fps
 * =========================================================================
 *
 * Map an integer frames-per-second value to the MTL st_fps enum.
 * MTL requires an exact fps enum for its transmission schedule; arbitrary
 * fractional rates are not supported by the current ST 2110-21 scheduler.
 */
enum st_fps get_st_fps(int fps) {
  switch (fps) {
    case 25: return ST_FPS_P25;
    case 30: return ST_FPS_P30;
    case 50: return ST_FPS_P50;
    case 60: return ST_FPS_P60;
    default:
      LOG_WARN("get_st_fps: unsupported fps %d, defaulting to ST_FPS_P30", fps);
      return ST_FPS_P30;
  }
}

/* =========================================================================
 * mtl_copy_crop_to_frame
 * =========================================================================
 *
 * Copy a rectangular crop of a planar FFmpeg AVFrame into an MTL st_frame.
 *
 * dst->addr[0/1/2] point to MTL's DMA-mapped hugepage TX buffers (luma,
 * Cb, Cr planes respectively).  MTL expects tightly-packed rows with no
 * per-line stride padding (stride == crop_w * bps for luma).
 *
 * The AVPixFmtDescriptor is consulted for:
 *   - bps:          bytes per sample (supports 8-bit and 10LE packed).
 *   - log2_chroma_w: horizontal chroma subsampling shift (0 = 4:4:x, 1 = 4:2:x).
 *   - log2_chroma_h: vertical   chroma subsampling shift (0 = no subsamp, 1 = 4:2:0).
 *
 *   fmt          — pixel format of src (must match desc used to alloc MTL session)
 *   crop_x/y     — top-left corner in the full-width shared yuv_frame (luma coords)
 *   crop_w/h     — crop rectangle dimensions in luma pixels
 */
void mtl_copy_crop_to_frame(struct st_frame* dst, AVFrame* src,
                             int crop_x, int crop_y,
                             int crop_w, int crop_h,
                             enum AVPixelFormat fmt) {
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
  if (!desc || !dst || !src) return;

  /* E-4: Bounds check — prevent out-of-bounds read from src */
  if (crop_x < 0 || crop_y < 0 || crop_w <= 0 || crop_h <= 0) {
    LOG_ERROR("mtl_copy_crop_to_frame: invalid crop rect (x=%d y=%d w=%d h=%d)",
              crop_x, crop_y, crop_w, crop_h);
    return;
  }
  if (crop_x + crop_w > src->width || crop_y + crop_h > src->height) {
    LOG_ERROR("mtl_copy_crop_to_frame: crop rect (x=%d+w=%d=%d, y=%d+h=%d=%d) "
              "exceeds source (%dx%d)",
              crop_x, crop_w, crop_x + crop_w,
              crop_y, crop_h, crop_y + crop_h,
              src->width, src->height);
    return;
  }

  int bps          = (desc->comp[0].depth + 7) / 8; /* bytes per sample         */
  int chroma_w_shl = desc->log2_chroma_w;            /* 0=4:4:x  1=4:2:x         */
  int chroma_h_shl = desc->log2_chroma_h;            /* 0=no-v-sub  1=4:2:0      */
  int chroma_h     = crop_h >> chroma_h_shl;
  int chroma_y     = crop_y >> chroma_h_shl;
  int chroma_x     = crop_x >> chroma_w_shl;
  int chroma_w     = crop_w >> chroma_w_shl;
  int dst_y_stride = crop_w  * bps;
  int dst_c_stride = chroma_w * bps;

  /* Resolve destination plane pointers.
   * For CUSTOM8 formats (e.g. YUV420CUSTOM8), MTL provides a single
   * contiguous buffer in addr[0] with addr[1]/addr[2] == NULL.  The
   * Y/U/V planes are packed sequentially: Y then U then V, each with
   * tightly-packed rows (no line padding).  Compute the offsets manually. */
  uint8_t* dst_y = (uint8_t*)dst->addr[0];
  if (!dst_y) return;

  uint8_t* dst_u = dst->addr[1] ? (uint8_t*)dst->addr[1]
                                 : dst_y + dst_y_stride * crop_h;
  uint8_t* dst_v = dst->addr[2] ? (uint8_t*)dst->addr[2]
                                 : dst_u + dst_c_stride * chroma_h;

  /* Luma (Y) plane — full crop_h rows, crop_w samples wide */
  for (int line = 0; line < crop_h; line++)
    memcpy(dst_y + line * dst_y_stride,
           src->data[0] + (crop_y + line) * src->linesize[0] + crop_x * bps,
           dst_y_stride);

  /* Cb (U) chroma plane — chroma_h rows, chroma_w samples wide */
  if (src->data[1]) {
    for (int line = 0; line < chroma_h; line++)
      memcpy(dst_u + line * dst_c_stride,
             src->data[1] + (chroma_y + line) * src->linesize[1] + chroma_x * bps,
             dst_c_stride);
  }

  /* Cr (V) chroma plane — chroma_h rows, chroma_w samples wide */
  if (src->data[2]) {
    for (int line = 0; line < chroma_h; line++)
      memcpy(dst_v + line * dst_c_stride,
             src->data[2] + (chroma_y + line) * src->linesize[2] + chroma_x * bps,
             dst_c_stride);
  }
}

/* =========================================================================
 * mtl_tx_init / mtl_tx_uninit
 * =========================================================================
 *
 * mtl_tx_init() — initialise the MTL library using parameters from app.
 *   Stores the resulting handle in manager->mtl.
 *   Allocates one TX queue per session plus two spare queues, and minimal
 *   RX queues for MTL control traffic.
 *
 * mtl_tx_uninit() — release the MTL library instance.
 */
int mtl_tx_init(session_manager_t* manager, struct dvledtx_context* app) {
  struct mtl_init_params mtl_params;
  memset(&mtl_params, 0, sizeof(mtl_params));

  mtl_params.flags   = MTL_FLAG_BIND_NUMA | MTL_FLAG_DEV_AUTO_START_STOP;
  mtl_params.num_ports = 1;
  snprintf(mtl_params.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", app->port);
  memcpy(mtl_params.sip_addr[MTL_PORT_P], app->sip_addr, MTL_IP_ADDR_LEN);
  mtl_params.pmd[MTL_PORT_P] = mtl_pmd_by_port_name(app->port);

  uint16_t tx_queues = (uint16_t)(app->st20p_sessions + 2);
  uint16_t rx_queues = 2;
  mtl_params.tx_queues_cnt[MTL_PORT_P] = tx_queues;
  mtl_params.rx_queues_cnt[MTL_PORT_P] = rx_queues;

  LOG_INFO("MTL init: port=%s pmd=%d tx_queues=%d rx_queues=%d",
           app->port, mtl_params.pmd[MTL_PORT_P], tx_queues, rx_queues);

  manager->mtl = mtl_init(&mtl_params);
  if (!manager->mtl) {
    LOG_ERROR("Failed to initialise MTL library");
    return -1;
  }
  LOG_INFO("MTL library initialised successfully");
  return 0;
}

void mtl_tx_uninit(session_manager_t* manager) {
  if (manager->mtl) {
    mtl_uninit(manager->mtl);
    manager->mtl = NULL;
  }
}

/* =========================================================================
 * mtl_tx_session_create / mtl_tx_session_free
 * =========================================================================
 *
 * mtl_tx_session_create() — build st20p_tx_ops from ctx/app and call
 *   st20p_tx_create().  Sets ctx->handle and ctx->frame_size on success.
 *
 * mtl_tx_session_free() — call st20p_tx_free() and clear ctx->handle.
 */
int mtl_tx_session_create(session_manager_t* manager, struct st20p_tx_ctx* ctx,
                           struct dvledtx_context* app, int session_idx) {
  struct st20p_tx_ops ops;
  memset(&ops, 0, sizeof(ops));

  ops.name = ctx->session_name;
  ops.priv = ctx;

  ops.port.num_port = 1;
  memcpy(ops.port.dip_addr[MTL_SESSION_PORT_P], app->dip_addr, MTL_IP_ADDR_LEN);
  snprintf(ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s", app->port);

  int udp_port = app->session_net[session_idx].udp_port;
  if (udp_port == 0) udp_port = (int)app->udp_port + (session_idx * 2);
  ops.port.udp_port[MTL_SESSION_PORT_P] = (uint16_t)udp_port;

  int payload_type = app->session_net[session_idx].payload_type;
  if (payload_type == 0) payload_type = app->payload_type;
  ops.port.payload_type = (uint8_t)payload_type;

  ops.width         = (uint32_t)ctx->crop_width;
  ops.height        = (uint32_t)ctx->crop_height;
  ops.fps           = get_st_fps(app->fps);
  ops.transport_fmt = get_transport_format(app->fmt);
  ops.input_fmt     = get_input_format(app->fmt);
  if ((int)ops.transport_fmt == -1 || (int)ops.input_fmt == -1) {
    LOG_ERROR("Unsupported pixel format %d for MTL ST20P TX session %d",
              app->fmt, session_idx);
    return -1;
  }
  ops.device        = ST_PLUGIN_DEVICE_AUTO;
  ops.framebuff_cnt = 3;
  ops.flags         = ST20P_TX_FLAG_BLOCK_GET;

  ctx->handle = st20p_tx_create(manager->mtl, &ops);
  if (!ctx->handle) {
    LOG_ERROR("Failed to create MTL ST20P TX session %d", session_idx);
    return -1;
  }
  ctx->frame_size = st20p_tx_frame_size(ctx->handle);
  LOG_INFO("ST20P TX session %d: MTL handle created, frame_size=%zu, "
           "crop=%dx%d+%d+%d udp_port=%d",
           session_idx, ctx->frame_size,
           ctx->crop_width, ctx->crop_height,
           ctx->crop_x_offset, ctx->crop_y_offset,
           udp_port);
  return 0;
}

void mtl_tx_session_free(struct st20p_tx_ctx* ctx) {
  if (ctx->handle) {
    st20p_tx_free(ctx->handle);
    ctx->handle = NULL;
  }
}

/* =========================================================================
 * mtl_tx_send_yuv_frame
 * =========================================================================
 *
 * Obtains a free DMA TX ring buffer from MTL, copies the crop strip from
 * src into it via mtl_copy_crop_to_frame(), sets the RTP timestamp, and
 * puts the frame back to MTL for transmission.
 *
 * Returns 0 on success, -1 on error (e.g. get_frame returned NULL after
 * timeout, or copy failed).
 */
int mtl_tx_send_yuv_frame(struct st20p_tx_ctx* ctx, AVFrame* src,
                          int crop_x, int crop_y, int crop_w, int crop_h) {
  if (!ctx->handle || !src) return -1;

  struct st_frame* frame = st20p_tx_get_frame(ctx->handle);
  if (!frame) {
    LOG_ERROR("ST20P TX(%d): st20p_tx_get_frame returned NULL", ctx->idx);
    return -1;
  }

  mtl_copy_crop_to_frame(frame, src, crop_x, crop_y, crop_w, crop_h,
                         ctx->app->fmt);

  frame->tfmt      = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  frame->timestamp = ctx->frames_sent * 90000 / (uint32_t)ctx->app->fps;

  int ret = st20p_tx_put_frame(ctx->handle, frame);
  if (ret < 0) {
    LOG_ERROR("ST20P TX(%d): st20p_tx_put_frame failed (ret=%d)", ctx->idx, ret);
    return -1;
  }

  ctx->frames_sent++;
  if (ctx->frames_sent % 100 == 0)
    LOG_INFO("ST20P TX(%d): sent %d frames (MTL)", ctx->idx, ctx->frames_sent);
  return 0;
}

/* =========================================================================
 * mtl_tx_send_raw_yuv
 * =========================================================================
 *
 * Obtains a free DMA TX ring buffer, copies frame_size bytes from
 * ctx->source_buffer (wrapping for loop playback) directly into MTL's
 * DMA buffer, then puts the frame for transmission.
 *
 * Returns 0 on success, -1 on error.
 */
int mtl_tx_send_raw_yuv(struct st20p_tx_ctx* ctx) {
  if (!ctx->handle || !ctx->source_buffer || ctx->source_size == 0) return -1;

  struct st_frame* frame = st20p_tx_get_frame(ctx->handle);
  if (!frame) {
    LOG_ERROR("ST20P TX(%d): st20p_tx_get_frame returned NULL", ctx->idx);
    return -1;
  }

  size_t frame_bytes = ctx->frame_size;
  if (ctx->current_pos + frame_bytes > ctx->source_size)
    ctx->current_pos = 0;

  if (frame->addr[0])
    memcpy(frame->addr[0], ctx->source_buffer + ctx->current_pos, frame_bytes);
  ctx->current_pos += frame_bytes;

  frame->tfmt      = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  frame->timestamp = ctx->frames_sent * 90000 / (uint32_t)ctx->app->fps;

  int ret = st20p_tx_put_frame(ctx->handle, frame);
  if (ret < 0) {
    LOG_ERROR("ST20P TX(%d): st20p_tx_put_frame (raw yuv) failed (ret=%d)",
              ctx->idx, ret);
    return -1;
  }

  ctx->frames_sent++;
  if (ctx->frames_sent % 100 == 0)
    LOG_INFO("ST20P TX(%d): sent %d frames (MTL raw yuv)", ctx->idx, ctx->frames_sent);
  return 0;
}

#endif /* ENABLE_MTL_TX */
