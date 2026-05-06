/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>

/* FFmpeg headers — always needed for the decode path */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/* MTL pipeline API headers — only when building the direct MTL TX path */
#ifdef ENABLE_MTL_TX
#include "mtl_api.h"
#include "st_pipeline_api.h"
#endif

// Forward declarations
struct dvledtx_context;

/*
 * Shared decode context - ONE decoder feeds ALL N TX sessions.
 *
 * Synchronization flow per frame:
 *   1. Decode thread decodes + sws_scales into yuv_frame
 *   2. Hits barrier_decoded  -> unblocks all TX threads
 *   3. Each TX thread encodes + writes its crop strip via TX output
 *   4. Hits barrier_copied   -> unblocks decode thread
 *   5. Decode thread loops to next frame
 */
struct shared_decode_ctx {
  AVFormatContext*   fmt_ctx;
  AVCodecContext*    codec_ctx;
  struct SwsContext* sws_ctx;
  AVFrame*           av_frame;    /* raw decoded frame                    */
  AVFrame*           yuv_frame;   /* full-width yuv422p10le after sws     */
  AVPacket*          av_packet;
  int                video_stream_idx;

  pthread_barrier_t  barrier_decoded; /* decode done -> TX threads may copy */
  pthread_barrier_t  barrier_copied;  /* TX done     -> decode may proceed  */
  pthread_t          decode_thread;

  int                num_sessions;
  volatile bool      exit;

  /* Startup gate — threads wait on this before entering the barrier loop.
   * Prevents barrier deadlock when a TX thread fails to create. */
  pthread_mutex_t    start_mutex;
  pthread_cond_t     start_cond;
  bool               start_ready;

  struct dvledtx_context* app;
  uint32_t           frame_counter;   /* shared monotonic frame number      */
};

/* Video TX session context */
struct st20p_tx_ctx {
  int idx;
  pthread_t thread;
  struct dvledtx_context* app;
  char session_name[32];

  /* Raw YUV source (single-session path) */
  FILE* source_file;
  uint8_t* source_buffer;
  size_t source_size;
  size_t current_pos;
  bool loop_playback;

  /* Shared decode context - set when num_sessions > 1 */
  struct shared_decode_ctx* shared_dec;

  /* Per-session FFmpeg input decoder - used only when num_sessions == 1 */
  bool use_ffmpeg;
  AVFormatContext*   fmt_ctx;        /* input demuxer */
  AVCodecContext*    codec_ctx;      /* input decoder */
  struct SwsContext* sws_ctx;
  AVFrame*           av_frame;
  AVFrame*           yuv_frame;      /* decoded + scaled input frame */
  AVPacket*          av_packet;
  int                video_stream_idx;

#ifdef ENABLE_MTL_TX
  /* ── MTL pipeline TX path ────────────────────────────────────────────────
   * Used when -DENABLE_MTL_TX is defined. */
  st20p_tx_handle    handle;         /* MTL pipeline TX session handle         */
#else
  /* ── FFmpeg mtl_st20p muxer TX path (default) ───────────────────────────
   * avformat_write_header → av_write_frame → mtl_st20p plugin → MTL TX ring. */
  AVFormatContext*   out_fmt_ctx;    /* output muxer (mtl_st20p)               */
  AVCodecContext*    enc_ctx;        /* unused in Kahawai path (always NULL)   */
  AVStream*          out_stream;
  AVFrame*           enc_frame;      /* crop-width scratch frame               */
  AVPacket*          enc_pkt;        /* pre-allocated packed frame buffer      */
  int64_t            pts;
#endif /* ENABLE_MTL_TX */

  /* Crop rectangle (pixels, from JSON) */
  int crop_x_offset;
  int crop_y_offset;
  int crop_width;
  int crop_height;

  uint32_t frames_sent;
  size_t   frame_size;   /* packed frame byte size (set at session init)       */
};

/* TX session manager */
typedef struct session_manager_s {
  struct st20p_tx_ctx* st20p_sessions;
  int st20p_count;

  struct shared_decode_ctx* shared_dec;

#ifdef ENABLE_MTL_TX
  mtl_handle mtl;   /* MTL library instance — owns all st20p_tx sessions */
#endif

  bool running;
} session_manager_t;

int  session_manager_init(session_manager_t* manager, struct dvledtx_context* app);
int  session_manager_start(session_manager_t* manager);
int  session_manager_stop(session_manager_t* manager);
void session_manager_cleanup(session_manager_t* manager);
bool session_manager_is_running(const session_manager_t* manager);

/* Exit-flag accessors — encapsulate the cross-module exit signal. */
void session_manager_request_exit(void);
bool session_manager_should_exit(void);
void session_manager_reset_exit(void);

int create_st20p_tx_session(session_manager_t* manager, struct dvledtx_context* app, int session_idx);
