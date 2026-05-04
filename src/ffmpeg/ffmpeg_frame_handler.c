/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

/*
 * ffmpeg_frame_handler.c — frame processing: crop and format conversion.
 *
 * Provides pixel-buffer operations shared by the FFmpeg TX path and the MTL
 * TX path:
 *
 *   convert_frame_format() — wraps sws_scale for src→dst colour conversion.
 *   crop_yuv_frame()       — copies a rectangular strip from a full-width
 *                            AVFrame into a smaller crop-sized AVFrame.
 *
 * These are transport-agnostic: the result AVFrame is consumed either by
 * ffmpeg_tx.c (packed via av_image_copy_to_buffer + av_write_frame) or by
 * mtl_tx.c (copied into an MTL DMA buffer via mtl_copy_crop_to_frame).
 */

#include "ffmpeg/ffmpeg_frame_handler.h"
#include "util/logger.h"
#include <string.h>

#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>

/* =========================================================================
 * convert_frame_format
 * ========================================================================= */
int convert_frame_format(struct SwsContext* sws_ctx,
                         const AVFrame* src, int src_height,
                         AVFrame* dst) {
  if (!sws_ctx || !src || !dst) {
    LOG_ERROR("convert_frame_format: NULL argument");
    return -1;
  }

  int rows = sws_scale(sws_ctx,
                       (const uint8_t* const*)src->data,
                       src->linesize,
                       0, src_height,
                       dst->data, dst->linesize);
  if (rows <= 0)
    LOG_ERROR("convert_frame_format: sws_scale failed (ret=%d)", rows);

  return rows;
}

/* =========================================================================
 * crop_yuv_frame
 * ========================================================================= */
int crop_yuv_frame(AVFrame* dst, const AVFrame* src,
                   int crop_x, int crop_y, int crop_w, int crop_h,
                   enum AVPixelFormat fmt) {
  if (!dst || !src) {
    LOG_ERROR("crop_yuv_frame: NULL argument");
    return -1;
  }

  /* E-4: Bounds check — prevent out-of-bounds read from src */
  if (crop_x < 0 || crop_y < 0 || crop_w <= 0 || crop_h <= 0) {
    LOG_ERROR("crop_yuv_frame: invalid crop rect (x=%d y=%d w=%d h=%d)",
              crop_x, crop_y, crop_w, crop_h);
    return -1;
  }
  if (crop_x + crop_w > src->width || crop_y + crop_h > src->height) {
    LOG_ERROR("crop_yuv_frame: crop rect (x=%d+w=%d=%d, y=%d+h=%d=%d) "
              "exceeds source (%dx%d)",
              crop_x, crop_w, crop_x + crop_w,
              crop_y, crop_h, crop_y + crop_h,
              src->width, src->height);
    return -1;
  }

  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
  if (!desc) {
    LOG_ERROR("crop_yuv_frame: unknown pixel format %d", fmt);
    return -1;
  }

  int bps          = (desc->comp[0].depth + 7) / 8;
  int chroma_w_shl = desc->log2_chroma_w;
  int chroma_h_shl = desc->log2_chroma_h;
  int chroma_h     = crop_h >> chroma_h_shl;
  int chroma_y     = crop_y >> chroma_h_shl;
  int chroma_x     = crop_x >> chroma_w_shl;
  int chroma_w     = crop_w >> chroma_w_shl;

  /* Luma (Y) plane */
  for (int line = 0; line < crop_h; line++)
    memcpy(dst->data[0] + line * dst->linesize[0],
           src->data[0] + (crop_y + line) * src->linesize[0] + crop_x * bps,
           (size_t)crop_w * bps);

  /* Cb (U) chroma plane */
  if (dst->data[1] && src->data[1]) {
    for (int line = 0; line < chroma_h; line++)
      memcpy(dst->data[1] + line * dst->linesize[1],
             src->data[1] + (chroma_y + line) * src->linesize[1] + chroma_x * bps,
             (size_t)chroma_w * bps);
  }

  /* Cr (V) chroma plane */
  if (dst->data[2] && src->data[2]) {
    for (int line = 0; line < chroma_h; line++)
      memcpy(dst->data[2] + line * dst->linesize[2],
             src->data[2] + (chroma_y + line) * src->linesize[2] + chroma_x * bps,
             (size_t)chroma_w * bps);
  }

  return 0;
}
