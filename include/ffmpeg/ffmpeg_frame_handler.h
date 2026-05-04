/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#pragma once

/*
 * ffmpeg_frame_handler.h — frame processing API (crop, format conversion).
 *
 * Provides pixel-buffer operations that are independent of the TX transport
 * (FFmpeg avdevice or MTL pipeline).  Used by both ffmpeg_tx.c and
 * ffmpeg_decoder.c (shared decode thread).
 *
 * Implementation: src/ffmpeg/ffmpeg_frame_handler.c
 */

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <stdbool.h>

/* Return the human-readable name for a pixel format, or "unknown". */
static inline const char* ffmpeg_fmt_name(enum AVPixelFormat fmt) {
  const char* n = av_get_pix_fmt_name(fmt);
  return n ? n : "unknown";
}

/* -------------------------------------------------------------------------
 * Color-space / format conversion
 * ---------------------------------------------------------------------- */

/*
 * convert_frame_format() — colour-convert src into dst using sws_scale.
 *
 *   sws_ctx    = SwsContext pre-created for the src→dst format/size mapping.
 *   src        = decoded raw frame (e.g. yuv420p from H.264 decoder).
 *   src_height = source frame height in luma lines.
 *   dst        = pre-allocated output frame (e.g. yuv422p10le).
 *
 * Returns the number of output rows written (>= 0), or a negative value on
 * error (mirrors sws_scale return convention).
 */
int convert_frame_format(struct SwsContext* sws_ctx,
                         const AVFrame* src, int src_height,
                         AVFrame* dst);

/* -------------------------------------------------------------------------
 * Crop
 * ---------------------------------------------------------------------- */

/*
 * crop_yuv_frame() — copy a rectangular crop strip from src into dst.
 *
 *   dst     = pre-allocated AVFrame with dimensions (crop_w × crop_h) in fmt.
 *   src     = full-width decoded frame (e.g. 1920×1080).
 *   crop_*  = crop rectangle in luma pixel coordinates.
 *   fmt     = pixel format; used to derive bytes-per-sample and chroma shifts
 *             via AVPixFmtDescriptor.
 *
 * Handles all planar YUV formats (4:2:0, 4:2:2, 4:4:4) and GBRP at any
 * bit depth supported by AVPixFmtDescriptor.
 *
 * Returns 0 on success, -1 if any argument is NULL or the descriptor is
 * unknown.
 */
int crop_yuv_frame(AVFrame* dst, const AVFrame* src,
                   int crop_x, int crop_y, int crop_w, int crop_h,
                   enum AVPixelFormat fmt);
