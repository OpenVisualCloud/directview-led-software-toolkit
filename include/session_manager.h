/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/* FFmpeg headers */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

// Forward declarations
struct tx_app_context;

/*
 * Shared decode context - ONE decoder feeds ALL N TX sessions.
 *
 * Synchronization flow per frame:
 *   1. Decode thread decodes + sws_scales into yuv_frame
 *   2. Hits barrier_decoded  -> unblocks all TX threads
 *   3. Each TX thread encodes + writes its crop strip via FFmpeg output
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
  pthread_barrier_t  barrier_copied;  /* TX done     -> encode may proceed  */
  pthread_t          decode_thread;

  int                num_sessions;
  volatile bool      exit;

  struct tx_app_context* app;
  uint32_t           frame_counter;   /* shared monotonic frame number      */
};

/* Video TX session context */
struct st20p_tx_ctx {
  int idx;
  pthread_t thread;
  struct tx_app_context* app;
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

  /* FFmpeg output (sender) — Kahawai (mtl_st20p) muxer, no encoder */
  AVFormatContext*   out_fmt_ctx;    /* output muxer (mtl_st20p / Kahawai) */
  AVCodecContext*    enc_ctx;        /* unused in Kahawai path (always NULL) */
  AVStream*          out_stream;
  AVFrame*           enc_frame;      /* crop-width scratch frame for send_video_frame */
  AVPacket*          enc_pkt;        /* pre-allocated packed frame buffer for MTL */
  int64_t            pts;

  /* Vertical crop */
  int crop_x_offset;
  int crop_width;

  uint32_t frames_sent;
  size_t   frame_size;
};

/* Audio TX session context */
struct st30p_tx_ctx {
  int idx;
  pthread_t thread;
  struct tx_app_context* app;
  char session_name[32];

  FILE* source_file;
  uint8_t* source_buffer;
  size_t source_size;
  size_t current_pos;
  bool loop_playback;

  /* FFmpeg output (UDP) */
  AVFormatContext*   out_fmt_ctx;
  AVCodecContext*    enc_ctx;
  AVStream*          out_stream;
  AVPacket*          enc_pkt;

  uint32_t frames_sent;
  size_t   frame_size;
};

/* TX session manager */
typedef struct {
  struct st20p_tx_ctx* st20p_sessions;
  int st20p_count;

  struct st30p_tx_ctx* st30p_sessions;
  int st30p_count;

  struct shared_decode_ctx* shared_dec;

  bool running;
} session_manager_t;

int  session_manager_init(session_manager_t* manager, struct tx_app_context* app);
int  session_manager_start(session_manager_t* manager);
int  session_manager_stop(session_manager_t* manager);
void session_manager_cleanup(session_manager_t* manager);
bool session_manager_is_running(const session_manager_t* manager);

int create_st20p_tx_session(session_manager_t* manager, struct tx_app_context* app, int session_idx);
int create_st30p_tx_session(session_manager_t* manager, struct tx_app_context* app, int session_idx);

int load_video_source(struct st20p_tx_ctx* ctx, const char* filename);
int load_audio_source(struct st30p_tx_ctx* ctx, const char* filename);
