/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#pragma once

#include "session_manager.h"

/*
 * ffmpeg_decoder.h — public API for the FFmpeg decode/output module.
 *
 * All functions that directly drive the FFmpeg decode pipeline (input
 * demuxing, H.264 decoding, colour conversion) and the MTL Kahawai
 * (mtl_st20p) output path live in src/ffmpeg/ffmpeg_decoder.c.
 *
 * session_manager.c uses this header to call into those routines without
 * containing any FFmpeg decode logic itself.
 */

/* -------------------------------------------------------------------------
 * Utility
 * ---------------------------------------------------------------------- */

/* Returns true when filename has a .yuv or .raw extension */
bool is_raw_yuv(const char* filename);

/* -------------------------------------------------------------------------
 * MTL Kahawai (mtl_st20p) video output — one instance per TX session
 * ---------------------------------------------------------------------- */

int  open_ffmpeg_output(struct st20p_tx_ctx* ctx);
void close_ffmpeg_output(struct st20p_tx_ctx* ctx);

/* Crop, pack and transmit one frame. crop_x/y = top-left origin, crop_w/h = size */
void send_video_frame(struct st20p_tx_ctx* ctx, AVFrame* src,
                      int crop_x, int crop_y, int crop_w, int crop_h);

/* -------------------------------------------------------------------------
 * Shared decoder — one decoder feeds all N TX sessions (multi-session path)
 * ---------------------------------------------------------------------- */

/*  Open the container + H.264 decoder + sws colour-converter.
 *  Allocates dec->av_frame, dec->yuv_frame and dec->av_packet. */
int  open_shared_ffmpeg(struct shared_decode_ctx* dec, const char* filename);

/* Release all FFmpeg resources held by dec (does NOT destroy barriers or
 * free dec itself — those are caller responsibilities). */
void close_shared_ffmpeg(struct shared_decode_ctx* dec);

/* Thread entry point — passed directly to pthread_create().
 * Runs the decode+barrier loop until dec->exit or g_tx_app_exit are set. */
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

/* Decode one video frame from ctx->fmt_ctx and send it via MTL muxer.
 * Encapsulates the av_read_frame → avcodec_receive_frame → sws_scale →
 * send_video_frame inner loop.  Returns true when a frame was sent. */
bool ffmpeg_decode_and_send(struct st20p_tx_ctx* ctx);
