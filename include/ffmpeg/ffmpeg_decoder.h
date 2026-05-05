/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#pragma once

#include "core/session_manager.h"

/*
 * ffmpeg_decoder.h — public API for the FFmpeg decode module.
 *
 * Covers MP4/MKV container demuxing, H.264 decoding, and libswscale colour
 * conversion.  session_manager.c uses this header to call into those
 * routines without containing any FFmpeg decode logic itself.
 */

/* -------------------------------------------------------------------------
 * Utility
 * ---------------------------------------------------------------------- */

/* Returns true when filename has a .yuv or .raw extension */
bool is_raw_yuv(const char* filename);

/* -------------------------------------------------------------------------
 * Shared decoder — one decoder feeds all N TX sessions (multi-session path)
 * ---------------------------------------------------------------------- */

/* Open the container + H.264 decoder + sws colour-converter.
 * Allocates dec->av_frame, dec->yuv_frame and dec->av_packet. */
int  open_shared_ffmpeg(struct shared_decode_ctx* dec, const char* filename);

/* Release all FFmpeg resources held by dec (does NOT destroy barriers or
 * free dec itself — those are caller responsibilities). */
void close_shared_ffmpeg(struct shared_decode_ctx* dec);

/* Thread entry point — passed directly to pthread_create().
 * Runs the decode+barrier loop until dec->exit or g_dvledtx_exit are set. */
void* shared_decode_thread(void* arg);

/* -------------------------------------------------------------------------
 * Per-session source — single-session or raw-YUV path
 * ---------------------------------------------------------------------- */

/* Load a video source for ctx.  For .yuv/.raw files: reads into
 * ctx->source_buffer (raw path).  For everything else: opens the FFmpeg
 * H.264 decoder (sets ctx->use_ffmpeg = true). */
int  load_video_source(struct st20p_tx_ctx* ctx, const char* filename);

/* Release FFmpeg input decoder resources held by ctx (use_ffmpeg path).
 * Safe to call even when ctx->use_ffmpeg is false. */
void close_ffmpeg_source(struct st20p_tx_ctx* ctx);

/* -------------------------------------------------------------------------
 * Per-frame decode (single-session path)
 * ---------------------------------------------------------------------- */

/* Decode one frame from ctx->fmt_ctx into ctx->yuv_frame.
 *
 * Reads AVPackets until avcodec_receive_frame() produces a decoded frame,
 * then calls convert_frame_format() (libswscale) to colour-convert into
 * ctx->yuv_frame.
 *
 * Handles H.264 B-frame reorder (EAGAIN) and EOF loop-restart internally.
 *
 * Returns true when ctx->yuv_frame is freshly populated, false on
 * error or when the exit flag is set. */
bool ffmpeg_decode_next_frame(struct st20p_tx_ctx* ctx);

