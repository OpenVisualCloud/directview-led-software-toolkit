/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

/*
 * ffmpeg_tx.c — FFmpeg avdevice TX path (mtl_st20p muxer).
 *
 * Implements frame transmission via Intel MTL's "mtl_st20p" FFmpeg output
 * device plugin (libavdevice).  This is the DEFAULT TX path used when
 * -DENABLE_MTL_TX is NOT set.
 *
 * Flow per frame:
 *   open_ffmpeg_tx()              → avformat_write_header() → MTL TX session created
 *   ffmpeg_tx_send_yuv_frame()    → crop + pack + av_write_frame
 *   ffmpeg_tx_send_raw_yuv()      → read from source_buffer + pack + av_write_frame
 *   close_ffmpeg_tx()             → av_write_trailer + avformat_free_context
 *
 * None of this file is compiled when ENABLE_MTL_TX is defined — in that
 * mode libavdevice is not linked and MTL is driven directly from mtl_tx.c.
 */

#ifndef ENABLE_MTL_TX

#include "ffmpeg/ffmpeg_tx.h"
#include "ffmpeg/ffmpeg_frame_handler.h"
#include "core/session_manager.h"
#include "app_context.h"
#include "util/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* =========================================================================
 * open_ffmpeg_tx
 * =========================================================================
 *
 * Creates an AVFormatContext backed by the "mtl_st20p" muxer (Intel MTL
 * FFmpeg plugin).  This is AVFMT_NOFILE — MTL manages its own TX ring
 * buffer internally; no URL or file descriptor is needed.
 *
 * Steps:
 *   1. Locate the mtl_st20p muxer registered by avdevice_register_all().
 *   2. Allocate output context; set NIC/IP/port options via AVOptions.
 *   3. Add a RAWVIDEO stream (no encoder — MTL accepts raw packed pixels).
 *   4. avformat_write_header() → mtl_st20p_write_header() in the plugin:
 *      allocates the MTL TX session and DMA hugepage ring buffers.
 *   5. Pre-allocate enc_frame (crop-width scratch) and enc_pkt
 *      (contiguous frame buffer) whose size must match MTL's frame_size.
 */
int open_ffmpeg_tx(struct st20p_tx_ctx* ctx) {
  int out_w  = ctx->crop_width  > 0 ? ctx->crop_width  : (int)ctx->app->width;
  int height = ctx->crop_height > 0 ? ctx->crop_height : (int)ctx->app->height;

  /* Locate the MTL muxer registered by avdevice_register_all() in main() */
  const AVOutputFormat* fmt = av_guess_format("mtl_st20p", NULL, NULL);
  if (fmt == NULL) {
    LOG_ERROR("ST20P TX(%d): mtl_st20p muxer not found — "
              "rebuild FFmpeg with Intel MTL plugin (--enable-mtl)", ctx->idx);
    return -1;
  }

  /* mtl_st20p is AVFMT_NOFILE: no URL or avio required */
  int ret = avformat_alloc_output_context2(&ctx->out_fmt_ctx, fmt, NULL, NULL);
  if (ret < 0 || ctx->out_fmt_ctx == NULL) {
    LOG_ERROR("ST20P TX(%d): cannot alloc output context for mtl_st20p (ret=%d)",
              ctx->idx, ret);
    return -1;
  }

  /* Resolve per-session network params from JSON config; fall back to
   * app-level defaults for CLI-only runs (session_net[] is zero-initialised). */
  int udp_port = ctx->app->session_net[ctx->idx].udp_port;
  if (udp_port == 0) udp_port = (int)ctx->app->udp_port + (ctx->idx * 2);

  int payload_type = ctx->app->session_net[ctx->idx].payload_type;
  if (payload_type == 0) payload_type = ctx->app->payload_type;

  ret = av_opt_set    (ctx->out_fmt_ctx->priv_data, "p_port",       ctx->app->port,         0);
  if (ret < 0) LOG_WARN("ST20P TX(%d): av_opt_set p_port failed (ret=%d)", ctx->idx, ret);
  ret = av_opt_set    (ctx->out_fmt_ctx->priv_data, "p_sip",        ctx->app->sip_addr_str, 0);
  if (ret < 0) LOG_WARN("ST20P TX(%d): av_opt_set p_sip failed (ret=%d)", ctx->idx, ret);
  ret = av_opt_set    (ctx->out_fmt_ctx->priv_data, "p_tx_ip",      ctx->app->dip_addr_str, 0);
  if (ret < 0) LOG_WARN("ST20P TX(%d): av_opt_set p_tx_ip failed (ret=%d)", ctx->idx, ret);
  ret = av_opt_set_int(ctx->out_fmt_ctx->priv_data, "udp_port",     (int64_t)udp_port,      0);
  if (ret < 0) LOG_WARN("ST20P TX(%d): av_opt_set_int udp_port failed (ret=%d)", ctx->idx, ret);
  ret = av_opt_set_int(ctx->out_fmt_ctx->priv_data, "payload_type", (int64_t)payload_type,  0);
  if (ret < 0) LOG_WARN("ST20P TX(%d): av_opt_set_int payload_type failed (ret=%d)", ctx->idx, ret);

  /* RAWVIDEO stream — no encoder; MTL accepts raw packed pixel data directly. */
  ctx->out_stream = avformat_new_stream(ctx->out_fmt_ctx, NULL);
  if (ctx->out_stream == NULL) {
    LOG_ERROR("ST20P TX(%d): avformat_new_stream failed", ctx->idx);
    avformat_free_context(ctx->out_fmt_ctx); ctx->out_fmt_ctx = NULL;
    return -1;
  }
  ctx->out_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
  ctx->out_stream->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
  ctx->out_stream->codecpar->width      = out_w;
  ctx->out_stream->codecpar->height     = height;
  ctx->out_stream->codecpar->format     = ctx->app->fmt;
  ctx->out_stream->avg_frame_rate       = (AVRational){ctx->app->fps, 1};
  ctx->out_stream->time_base            = (AVRational){1, ctx->app->fps};

  ret = avformat_write_header(ctx->out_fmt_ctx, NULL);
  if (ret < 0) {
    char errbuf[128];
    av_strerror(ret, errbuf, sizeof(errbuf));
    LOG_ERROR("ST20P TX(%d): avformat_write_header (mtl_st20p) failed: %s",
              ctx->idx, errbuf);
    avformat_free_context(ctx->out_fmt_ctx); ctx->out_fmt_ctx = NULL;
    return -1;
  }

  /* Scratch AVFrame for the cropped strip */
  ctx->enc_frame = av_frame_alloc();
  if (ctx->enc_frame == NULL) {
    LOG_ERROR("ST20P TX(%d): av_frame_alloc (enc_frame) failed", ctx->idx);
    avformat_free_context(ctx->out_fmt_ctx); ctx->out_fmt_ctx = NULL;
    return -1;
  }
  ctx->enc_frame->format = ctx->app->fmt;
  ctx->enc_frame->width  = out_w;
  ctx->enc_frame->height = height;
  ret = av_frame_get_buffer(ctx->enc_frame, 32);
  if (ret < 0) {
    LOG_ERROR("ST20P TX(%d): av_frame_get_buffer failed (ret=%d)", ctx->idx, ret);
    av_frame_free(&ctx->enc_frame);
    avformat_free_context(ctx->out_fmt_ctx); ctx->out_fmt_ctx = NULL;
    return -1;
  }

  /* Pre-allocate enc_pkt with the exact packed frame size (align=1). */
  int frame_sz = av_image_get_buffer_size(ctx->app->fmt, out_w, height, 1);
  if (frame_sz < 0) {
    LOG_ERROR("ST20P TX(%d): av_image_get_buffer_size failed (ret=%d)",
              ctx->idx, frame_sz);
    av_frame_free(&ctx->enc_frame);
    avformat_free_context(ctx->out_fmt_ctx); ctx->out_fmt_ctx = NULL;
    return -1;
  }
  ctx->enc_pkt = av_packet_alloc();
  if (ctx->enc_pkt == NULL) {
    LOG_ERROR("ST20P TX(%d): av_packet_alloc failed", ctx->idx);
    av_frame_free(&ctx->enc_frame);
    avformat_free_context(ctx->out_fmt_ctx); ctx->out_fmt_ctx = NULL;
    return -1;
  }
  ret = av_new_packet(ctx->enc_pkt, frame_sz);
  if (ret < 0) {
    LOG_ERROR("ST20P TX(%d): av_new_packet failed (ret=%d)", ctx->idx, ret);
    av_packet_free(&ctx->enc_pkt);
    av_frame_free(&ctx->enc_frame);
    avformat_free_context(ctx->out_fmt_ctx); ctx->out_fmt_ctx = NULL;
    return -1;
  }

  ctx->pts = 0;
  LOG_INFO("ST20P TX(%d): ffmpeg_tx opened (%dx%d %s @ %dfps) -> %s:%u via %s",
           ctx->idx, out_w, height, ffmpeg_fmt_name(ctx->app->fmt), ctx->app->fps,
           ctx->app->dip_addr_str, (unsigned)udp_port, ctx->app->port);
  return 0;
}

/* =========================================================================
 * close_ffmpeg_tx
 * ========================================================================= */
void close_ffmpeg_tx(struct st20p_tx_ctx* ctx) {
  if (ctx->out_fmt_ctx != NULL) {
    av_write_trailer(ctx->out_fmt_ctx);
    avformat_free_context(ctx->out_fmt_ctx);
    ctx->out_fmt_ctx = NULL;
  }
  if (ctx->enc_frame != NULL) { av_frame_free(&ctx->enc_frame); }
  if (ctx->enc_pkt != NULL)   { av_packet_free(&ctx->enc_pkt); }
}

/* =========================================================================
 * ffmpeg_tx_send_yuv_frame
 * =========================================================================
 *
 *   src    = full-width shared yuv_frame from the decode thread.
 *   crop_* = crop rectangle for this TX session.
 *
 * Steps:
 *   1. crop_yuv_frame(): copy the strip from src into ctx->enc_frame.
 *   2. av_image_copy_to_buffer: pack planar enc_frame into enc_pkt->data
 *      as a single contiguous byte stream (align=1, no per-line padding).
 *   3. av_write_frame: hand enc_pkt to mtl_st20p_write_packet() which
 *      copies it into the MTL DMA TX ring; DPDK transmits as RTP/UDP.
 *
 * Returns 0 on success, -1 on error.
 */
int ffmpeg_tx_send_yuv_frame(struct st20p_tx_ctx* ctx, AVFrame* src,
                             int crop_x, int crop_y, int crop_w, int crop_h) {
  if (ctx->out_fmt_ctx == NULL || ctx->enc_frame == NULL || ctx->enc_pkt == NULL) return -1;

  int ret = av_frame_make_writable(ctx->enc_frame);
  if (ret < 0) {
    LOG_ERROR("ST20P TX(%d): av_frame_make_writable failed (ret=%d)", ctx->idx, ret);
    return -1;
  }

  ret = crop_yuv_frame(ctx->enc_frame, src, crop_x, crop_y, crop_w, crop_h,
                       ctx->app->fmt);
  if (ret < 0) {
    LOG_ERROR("ST20P TX(%d): crop_yuv_frame failed", ctx->idx);
    return -1;
  }

  int packed = av_image_copy_to_buffer(
      ctx->enc_pkt->data, ctx->enc_pkt->size,
      (const uint8_t* const*)ctx->enc_frame->data, ctx->enc_frame->linesize,
      ctx->app->fmt, crop_w, crop_h, 1);
  if (packed < 0) {
    LOG_ERROR("ST20P TX(%d): av_image_copy_to_buffer failed (ret=%d)",
              ctx->idx, packed);
    return -1;
  }

  ctx->enc_pkt->size         = packed;
  ctx->enc_pkt->pts          = ctx->pts;
  ctx->enc_pkt->dts          = ctx->pts;
  ctx->pts++;
  ctx->enc_pkt->pos          = -1;
  ctx->enc_pkt->stream_index = ctx->out_stream->index;
  ret = av_write_frame(ctx->out_fmt_ctx, ctx->enc_pkt);
  if (ret < 0) {
    LOG_ERROR("ST20P TX(%d): av_write_frame failed (ret=%d)", ctx->idx, ret);
    return -1;
  }
  ctx->enc_pkt->size = packed; /* restore: av_write_frame may modify size */

  ctx->frames_sent++;
  if (ctx->frames_sent % 100 == 0)
    LOG_DEBUG("ST20P TX(%d): sent %d frames (ffmpeg_tx)", ctx->idx, ctx->frames_sent);
  return 0;
}

/* =========================================================================
 * ffmpeg_tx_send_raw_yuv
 * =========================================================================
 *
 * Reads frame_size bytes sequentially from ctx->source_buffer (wrapping for
 * loop playback), packs into enc_pkt via av_image_copy, and calls
 * av_write_frame.
 *
 * Returns 0 on success, -1 on error.
 */
int ffmpeg_tx_send_raw_yuv(struct st20p_tx_ctx* ctx) {
  if (ctx->enc_frame == NULL || ctx->enc_pkt == NULL) return -1;
  if (ctx->source_buffer == NULL || ctx->source_size == 0) return -1;

  int ret = av_frame_make_writable(ctx->enc_frame);
  if (ret < 0) {
    LOG_ERROR("ST20P TX(%d): av_frame_make_writable failed (ret=%d)", ctx->idx, ret);
    return -1;
  }

  size_t frame_bytes = ctx->frame_size;
  if (ctx->current_pos + frame_bytes > ctx->source_size)
    ctx->current_pos = 0;

  /* Raw YUV files are packed (no per-line stride padding). Use
   * av_image_fill_arrays with align=1 to map the packed source buffer
   * onto the plane pointers/linesizes for av_image_copy. */
  uint8_t* planes[AV_NUM_DATA_POINTERS];
  int      linesizes[AV_NUM_DATA_POINTERS];
  ret = av_image_fill_arrays(planes, linesizes,
                             ctx->source_buffer + ctx->current_pos,
                             ctx->app->fmt,
                             ctx->enc_frame->width, ctx->enc_frame->height, 1);
  if (ret < 0) {
    LOG_ERROR("ST20P TX(%d): av_image_fill_arrays failed (ret=%d)", ctx->idx, ret);
    return -1;
  }

  av_image_copy(ctx->enc_frame->data, ctx->enc_frame->linesize,
                (const uint8_t**)planes, linesizes,
                ctx->app->fmt, ctx->enc_frame->width, ctx->enc_frame->height);
  ctx->current_pos += frame_bytes;

  int packed = av_image_copy_to_buffer(
      ctx->enc_pkt->data, ctx->enc_pkt->size,
      (const uint8_t* const*)ctx->enc_frame->data, ctx->enc_frame->linesize,
      ctx->app->fmt, ctx->enc_frame->width, ctx->enc_frame->height, 1);
  if (packed < 0) {
    LOG_ERROR("ST20P TX(%d): av_image_copy_to_buffer failed (ret=%d)", ctx->idx, packed);
    return -1;
  }

  ctx->enc_pkt->size         = packed;
  ctx->enc_pkt->pts          = ctx->pts;
  ctx->enc_pkt->dts          = ctx->pts;
  ctx->pts++;
  ctx->enc_pkt->pos          = -1;
  ctx->enc_pkt->stream_index = ctx->out_stream->index;
  ret = av_write_frame(ctx->out_fmt_ctx, ctx->enc_pkt);
  if (ret < 0) {
    LOG_ERROR("ST20P TX(%d): av_write_frame (raw YUV) failed (ret=%d)", ctx->idx, ret);
    return -1;
  }
  ctx->enc_pkt->size = packed;

  ctx->frames_sent++;
  if (ctx->frames_sent % 100 == 0)
    LOG_DEBUG("ST20P TX(%d): sent %d frames (raw yuv)", ctx->idx, ctx->frames_sent);
  return 0;
}

#endif /* !ENABLE_MTL_TX */
