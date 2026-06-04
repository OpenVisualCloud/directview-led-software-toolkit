/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#include "ffmpeg/ffmpeg_decoder.h"
#include "ffmpeg/ffmpeg_frame_handler.h"
#include "core/session_manager.h"
#include "app_context.h"
#include "util/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

/* =========================================================================
 * Helpers
 * ========================================================================= */


bool is_raw_yuv(const char* filename) {
  const char* ext = strrchr(filename, '.');
  if (ext == NULL) return false;
  return (strcasecmp(ext, ".yuv") == 0 || strcasecmp(ext, ".raw") == 0);
}

/* =========================================================================
 * Shared decode thread
 * ========================================================================= */
/*
 * shared_decode_thread() — ONE thread shared by all N TX sessions.
 *
 * Responsibility: decode the source MP4 frame-by-frame and colour-convert
 * into the shared yuv_frame buffer. Uses a double-barrier protocol to
 * safely hand the buffer to N TX threads and reclaim it after they finish.
 *
 * Per-frame flow:
 *   1. Inner loop: av_read_frame + avcodec_send/receive until one decoded
 *      frame is available. Loops on EAGAIN (H.264 B-frame reorder delay).
 *      On EOF: seeks back to start for infinite loop playback.
 *   2. sws_scale: convert decoded frame (yuv420p) → yuv_frame (yuv422p10le).
 *   3. barrier_decoded.wait: stall until ALL N TX threads also arrive.
 *      This guarantees yuv_frame is fully written before any TX thread reads.
 *   4. barrier_copied.wait: stall until ALL N TX threads finish their crop+send.
 *      This guarantees yuv_frame is no longer being read before we overwrite it.
 */
void* shared_decode_thread(void* arg) {
  struct shared_decode_ctx* dec = (struct shared_decode_ctx*)arg;

  LOG_INFO("Shared decode thread started (%d sessions)", dec->num_sessions);

  /* Startup gate: wait until all threads are created before touching barriers */
  pthread_mutex_lock(&dec->start_mutex);
  while (dec->start_ready == false)
    pthread_cond_wait(&dec->start_cond, &dec->start_mutex);
  pthread_mutex_unlock(&dec->start_mutex);

  if (dec->exit == true) {
    LOG_INFO("Shared decode thread: startup aborted");
    return NULL;
  }

  /* Frame period in nanoseconds for FPS-based pacing (e.g. 33 333 333 ns @ 30fps).
   * The decode thread sleeps after each barrier_copied to ensure frames are
   * fed into the MTL TX ring at the configured rate. Without pacing, FFmpeg
   * decodes much faster than the network can transmit, causing MTL's 3-deep
   * frame ring to fill up and log st20p_tx_get_frame timeout on every frame. */
  int fps = dec->app->fps > 0 ? dec->app->fps : 30;
  long frame_period_ns = 1000000000L / fps;
  struct timespec last_frame_ts;
  clock_gettime(CLOCK_MONOTONIC, &last_frame_ts);

  while (dec->exit == false && session_manager_should_exit() == false) {
    bool got_frame = false;

    /* D-2: Decode watchdog — limit iterations to prevent infinite loop
     * when a crafted container causes EAGAIN indefinitely. */
    int decode_attempts = 0;
    static const int MAX_DECODE_ATTEMPTS = 10000;

    /* Inner loop: keep reading packets until one decoded frame is produced.
     * H.264 requires multiple packets before the first frame due to B-frames. */
    while (got_frame == false && dec->exit == false && session_manager_should_exit() == false) {
      if (++decode_attempts > MAX_DECODE_ATTEMPTS) {
        LOG_ERROR("Shared decode: watchdog triggered after %d attempts — "
                  "possible malformed container", MAX_DECODE_ATTEMPTS);
        dec->exit = true;
        break;
      }
      /* Read next compressed packet from the MP4 container */
      int ret = av_read_frame(dec->fmt_ctx, dec->av_packet);
      if (ret == AVERROR_EOF) {
        /* End of file — seek back to start and flush decoder for looping */
        int seek_ret = av_seek_frame(dec->fmt_ctx, dec->video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
        if (seek_ret < 0) {
          LOG_ERROR("Shared decode: av_seek_frame failed (ret=%d)", seek_ret);
        }
        avcodec_flush_buffers(dec->codec_ctx);
        LOG_DEBUG("Shared decode: loop restart");
        continue;
      }
      if (ret < 0) continue;

      /* MP4 interleaves video+audio packets; skip non-video streams */
      if (dec->av_packet->stream_index != dec->video_stream_idx) {
        av_packet_unref(dec->av_packet);
        continue;
      }

      /* Push compressed H.264 NAL unit into the decoder's input queue */
      ret = avcodec_send_packet(dec->codec_ctx, dec->av_packet);
      av_packet_unref(dec->av_packet); /* release compressed buffer immediately */
      if (ret < 0 && ret != AVERROR(EAGAIN)) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Shared decode: avcodec_send_packet failed: %s", errbuf);
        continue;
      }

      /* Try to pull a decoded raw frame from the decoder's output queue.
       * EAGAIN = decoder needs more packets (B-frame reorder) → loop again.
       * 0      = got a full decoded frame in dec->av_frame (yuv420p). */
      ret = avcodec_receive_frame(dec->codec_ctx, dec->av_frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) continue;
      if (ret < 0) break;

      /* Colour-convert + chroma upsample: yuv420p (8-bit) → yuv422p10le (10-bit).
       * Output goes into dec->yuv_frame — the single shared full-width (1920px)
       * buffer that all TX threads will read from simultaneously. */
      int rows = convert_frame_format(dec->sws_ctx, dec->av_frame,
                                      dec->codec_ctx->height, dec->yuv_frame);
      av_frame_unref(dec->av_frame); /* return decoded frame back to FFmpeg pool */
      if (rows <= 0) {
        LOG_ERROR("Shared decode: convert_frame_format failed (ret=%d)", rows);
        continue; /* don't send garbage frame to TX threads */
      }

      dec->frame_counter++;
      got_frame = true;
    }

    if (got_frame == false) break; /* error or exit — leave main loop */

    /* SYNC POINT 1 — barrier_decoded (count = N+1: decode thread + N TX threads).
     * Decode thread arrives here after sws_scale is complete.
     * All N TX threads arrive after their previous barrier_copied.
     * Once all N+1 arrive → all are released → TX threads start reading yuv_frame. */
    pthread_barrier_wait(&dec->barrier_decoded);

    /* SYNC POINT 2 — barrier_copied (count = N+1).
     * Decode thread arrives here immediately after barrier_decoded.
     * N TX threads arrive after ffmpeg_tx_send_yuv_frame() / mtl_tx_send_yuv_frame() completes.
     * Once all N+1 arrive → decode thread is released → safe to overwrite yuv_frame. */
    pthread_barrier_wait(&dec->barrier_copied);

    /* FPS pacing: sleep until the next frame deadline.
     * Compute elapsed time since last frame and sleep for the remainder of
     * the frame period. This prevents FFmpeg from decoding faster than the
     * network can transmit, which would exhaust MTL's TX ring buffers. */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ns = (now.tv_sec  - last_frame_ts.tv_sec)  * 1000000000L
                    + (now.tv_nsec - last_frame_ts.tv_nsec);
    long sleep_ns = frame_period_ns - elapsed_ns;
    if (sleep_ns > 0) {
      struct timespec req = { .tv_sec = 0, .tv_nsec = sleep_ns };
      nanosleep(&req, NULL);
    }
    clock_gettime(CLOCK_MONOTONIC, &last_frame_ts);
  }

  /* Signal exit: hit both barriers one final time so TX threads don't deadlock */
  dec->exit = true;
  pthread_barrier_wait(&dec->barrier_decoded);
  pthread_barrier_wait(&dec->barrier_copied);

  LOG_INFO("Shared decode thread stopped, decoded %u frames", dec->frame_counter);
  return NULL;
}

/* =========================================================================
 * Common FFmpeg decoder open/close helpers
 *
 * Both the shared decode path (open_shared_ffmpeg) and the per-session
 * decode path (open_ffmpeg_source) need the same sequence:
 *   avformat_open_input -> find_stream -> find_decoder -> alloc_context ->
 *   open2 -> sws_getContext -> alloc frames/packet -> alloc yuv_frame buffer.
 *
 * open_ffmpeg_decoder() extracts this common logic.  The caller passes in
 * pointers to the target struct's fields.
 * ========================================================================= */
static int open_ffmpeg_decoder(
    const char* filename, const char* log_prefix,
    enum AVPixelFormat target_fmt, int target_w, int target_h,
    AVFormatContext** out_fmt_ctx, AVCodecContext** out_codec_ctx,
    struct SwsContext** out_sws_ctx, AVFrame** out_av_frame,
    AVFrame** out_yuv_frame, AVPacket** out_av_packet,
    int* out_video_stream_idx) {
  char errbuf[256];
  int ret;

  ret = avformat_open_input(out_fmt_ctx, filename, NULL, NULL);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    LOG_ERROR("%s: cannot open %s: %s", log_prefix, filename, errbuf);
    return -1;
  }
  ret = avformat_find_stream_info(*out_fmt_ctx, NULL);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    LOG_ERROR("%s: avformat_find_stream_info failed: %s", log_prefix, errbuf);
    avformat_close_input(out_fmt_ctx);
    return -1;
  }

  *out_video_stream_idx = av_find_best_stream(*out_fmt_ctx, AVMEDIA_TYPE_VIDEO,
                                               -1, -1, NULL, 0);
  if (*out_video_stream_idx < 0) {
    LOG_ERROR("%s: no video stream in %s", log_prefix, filename);
    avformat_close_input(out_fmt_ctx);
    return -1;
  }

  AVStream* stream = (*out_fmt_ctx)->streams[*out_video_stream_idx];
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (codec == NULL) {
    LOG_ERROR("%s: no decoder for codec_id=%d", log_prefix,
              stream->codecpar->codec_id);
    avformat_close_input(out_fmt_ctx);
    return -1;
  }

  *out_codec_ctx = avcodec_alloc_context3(codec);
  if (*out_codec_ctx == NULL) {
    LOG_ERROR("%s: avcodec_alloc_context3 failed", log_prefix);
    avformat_close_input(out_fmt_ctx);
    return -1;
  }
  ret = avcodec_parameters_to_context(*out_codec_ctx, stream->codecpar);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    LOG_ERROR("%s: avcodec_parameters_to_context failed: %s", log_prefix, errbuf);
    avcodec_free_context(out_codec_ctx);
    avformat_close_input(out_fmt_ctx);
    return -1;
  }
  (*out_codec_ctx)->thread_count = 4;
  ret = avcodec_open2(*out_codec_ctx, codec, NULL);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    LOG_ERROR("%s: avcodec_open2 failed: %s", log_prefix, errbuf);
    avcodec_free_context(out_codec_ctx);
    avformat_close_input(out_fmt_ctx);
    return -1;
  }

  *out_sws_ctx = sws_getContext(
    (*out_codec_ctx)->width, (*out_codec_ctx)->height, (*out_codec_ctx)->pix_fmt,
    target_w, target_h, target_fmt,
    SWS_FAST_BILINEAR, NULL, NULL, NULL);
  if (*out_sws_ctx == NULL) {
    LOG_ERROR("%s: sws_getContext failed", log_prefix);
    avcodec_free_context(out_codec_ctx);
    avformat_close_input(out_fmt_ctx);
    return -1;
  }

  *out_av_frame  = av_frame_alloc();
  *out_yuv_frame = av_frame_alloc();
  *out_av_packet = av_packet_alloc();

  (*out_yuv_frame)->format = target_fmt;
  (*out_yuv_frame)->width  = target_w;
  (*out_yuv_frame)->height = target_h;
  ret = av_image_alloc((*out_yuv_frame)->data, (*out_yuv_frame)->linesize,
                       target_w, target_h, target_fmt, 32);
  if (ret < 0) {
    av_frame_free(out_av_frame);
    av_frame_free(out_yuv_frame);
    av_packet_free(out_av_packet);
    sws_freeContext(*out_sws_ctx); *out_sws_ctx = NULL;
    avcodec_free_context(out_codec_ctx);
    avformat_close_input(out_fmt_ctx);
    return -1;
  }

  LOG_INFO("%s: opened '%s' Codec=%s %dx%d %s -> %dx%d %s",
           log_prefix, filename, codec->name,
           (*out_codec_ctx)->width, (*out_codec_ctx)->height,
           av_get_pix_fmt_name((*out_codec_ctx)->pix_fmt),
           target_w, target_h, ffmpeg_fmt_name(target_fmt));
  return 0;
}

/* Release common FFmpeg decoder resources.  NULL-safe for each field. */
static void close_ffmpeg_decoder(
    AVFormatContext** fmt_ctx, AVCodecContext** codec_ctx,
    struct SwsContext** sws_ctx, AVFrame** av_frame,
    AVFrame** yuv_frame, AVPacket** av_packet) {
  if (*av_frame != NULL)  av_frame_free(av_frame);
  if (*yuv_frame != NULL) {
    av_freep(&(*yuv_frame)->data[0]);
    av_frame_free(yuv_frame);
  }
  if (*av_packet != NULL) av_packet_free(av_packet);
  if (*sws_ctx != NULL)   { sws_freeContext(*sws_ctx); *sws_ctx = NULL; }
  if (*codec_ctx != NULL) avcodec_free_context(codec_ctx);
  if (*fmt_ctx != NULL)   avformat_close_input(fmt_ctx);
}

/* =========================================================================
 * Open/close shared FFmpeg decoder (multi-session input path)
 * ========================================================================= */
int open_shared_ffmpeg(struct shared_decode_ctx* dec, const char* filename) {
  const struct dvledtx_context* app = dec->app;
  return open_ffmpeg_decoder(
    filename, "Shared decode",
    app->fmt, (int)app->width, (int)app->height,
    &dec->fmt_ctx, &dec->codec_ctx, &dec->sws_ctx,
    &dec->av_frame, &dec->yuv_frame, &dec->av_packet,
    &dec->video_stream_idx);
}

void close_shared_ffmpeg(struct shared_decode_ctx* dec) {
  close_ffmpeg_decoder(
    &dec->fmt_ctx, &dec->codec_ctx, &dec->sws_ctx,
    &dec->av_frame, &dec->yuv_frame, &dec->av_packet);
}

/* =========================================================================
 * Per-session FFmpeg input decoder (single-session path)
 * ========================================================================= */
static int open_ffmpeg_source(struct st20p_tx_ctx* ctx, const char* filename) {
  char log_prefix[64];
  snprintf(log_prefix, sizeof(log_prefix), "ST20P TX(%d)", ctx->idx);
  int ret = open_ffmpeg_decoder(
    filename, log_prefix,
    ctx->app->fmt, (int)ctx->app->width, (int)ctx->app->height,
    &ctx->fmt_ctx, &ctx->codec_ctx, &ctx->sws_ctx,
    &ctx->av_frame, &ctx->yuv_frame, &ctx->av_packet,
    &ctx->video_stream_idx);
  if (ret == 0) ctx->use_ffmpeg = true;
  return ret;
}

void close_ffmpeg_source(struct st20p_tx_ctx* ctx) {
  if (ctx->use_ffmpeg == false) return;
  close_ffmpeg_decoder(
    &ctx->fmt_ctx, &ctx->codec_ctx, &ctx->sws_ctx,
    &ctx->av_frame, &ctx->yuv_frame, &ctx->av_packet);
}

/* =========================================================================
 * Video source loading
 * ========================================================================= */
int load_video_source(struct st20p_tx_ctx* ctx, const char* filename) {
  if (!filename || strlen(filename) == 0) {
    LOG_WARN("ST20P TX(%d): no source file configured", ctx->idx);
    return 0;
  }
  if (is_raw_yuv(filename)) {
    /* D-1: Maximum raw YUV file size cap to prevent memory exhaustion
     * (e.g. symlink to /dev/zero). Default 2 GB. */
    static const size_t MAX_RAW_YUV_SIZE = 2UL * 1024 * 1024 * 1024;

    FILE* f = fopen(filename, "rb");
    if (!f) { LOG_ERROR("ST20P TX(%d): Cannot open %s", ctx->idx, filename); return -1; }

    /* T-1: Use fstat(fd) instead of fseek/ftell to avoid TOCTOU race.
     * Once the file is open, fstat operates on the fd — not the path. */
    int fd = fileno(f);
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
      LOG_ERROR("ST20P TX(%d): fstat failed or not a regular file: %s",
                ctx->idx, filename);
      fclose(f); return -1;
    }
    size_t sz = (size_t)st.st_size;

    if (sz == 0) { fclose(f); return 0; }
    if (sz > MAX_RAW_YUV_SIZE) {
      LOG_ERROR("ST20P TX(%d): raw YUV file %s size %zu exceeds max %zu",
                ctx->idx, filename, sz, MAX_RAW_YUV_SIZE);
      fclose(f); return -1;
    }

    ctx->source_size = sz;
    ctx->source_buffer = malloc(ctx->source_size);
    if (ctx->source_buffer == NULL) { fclose(f); return -1; }
    size_t nread = fread(ctx->source_buffer, 1, ctx->source_size, f);
    fclose(f);
    if (nread != ctx->source_size) {
      LOG_WARN("ST20P TX(%d): fread read %zu of %zu bytes from %s",
               ctx->idx, nread, ctx->source_size, filename);
      ctx->source_size = nread;
    }
    ctx->current_pos   = 0;
    ctx->loop_playback = true;
    ctx->use_ffmpeg    = false;
    /* frame_size: size of one packed frame at crop dimensions (not full resolution).
     * The raw YUV file is expected to contain strips of crop_width × crop_height. */
    int w = ctx->crop_width  > 0 ? ctx->crop_width  : (int)ctx->app->width;
    int h = ctx->crop_height > 0 ? ctx->crop_height : (int)ctx->app->height;
    int fsize = av_image_get_buffer_size(ctx->app->fmt, w, h, 1);
    ctx->frame_size = (fsize > 0) ? (size_t)fsize : 0;
    /* Allocate the yuv_frame container (no buffer — data[] are set per-frame by
     * tx_fetch_next_frame using av_image_fill_arrays into source_buffer). */
    ctx->yuv_frame = av_frame_alloc();
    if (ctx->yuv_frame == NULL) {
      free(ctx->source_buffer); ctx->source_buffer = NULL;
      return -1;
    }
    ctx->yuv_frame->format = ctx->app->fmt;
    ctx->yuv_frame->width  = w;
    ctx->yuv_frame->height = h;
    LOG_INFO("ST20P TX(%d): Loaded %zu bytes from RAW YUV: %s",
           ctx->idx, ctx->source_size, filename);
    return 0;
  }
  return open_ffmpeg_source(ctx, filename);
}

/* =========================================================================
 * ffmpeg_decode_next_frame — decode one frame into ctx->yuv_frame
 * =========================================================================
 *
 * Reads AVPackets from ctx->fmt_ctx until avcodec_receive_frame() produces
 * one decoded frame, then colour-converts it with convert_frame_format()
 * into ctx->yuv_frame.
 *
 * Handles H.264 B-frame reorder (EAGAIN) and EOF loop-restart internally.
 *
 * Returns true when a frame was successfully decoded into ctx->yuv_frame,
 * false on error or when the exit flag is set.
 */
bool ffmpeg_decode_next_frame(struct st20p_tx_ctx* ctx) {
  if (ctx->fmt_ctx == NULL || ctx->codec_ctx == NULL || ctx->sws_ctx == NULL ||
      ctx->yuv_frame == NULL || ctx->av_packet == NULL || ctx->av_frame == NULL)
    return false;

  char errbuf[128];
  /* D-2: Decode watchdog for per-session decode path */
  int decode_attempts = 0;
  static const int MAX_DECODE_ATTEMPTS_PER_SESSION = 10000;

  while (ctx->app->exit == false && session_manager_should_exit() == false) {
    if (++decode_attempts > MAX_DECODE_ATTEMPTS_PER_SESSION) {
      LOG_ERROR("ST20P TX(%d): decode watchdog triggered after %d attempts",
                ctx->idx, MAX_DECODE_ATTEMPTS_PER_SESSION);
      return false;
    }

    int ret = av_read_frame(ctx->fmt_ctx, ctx->av_packet);
    if (ret == AVERROR_EOF) {
      ret = av_seek_frame(ctx->fmt_ctx, ctx->video_stream_idx, 0,
                          AVSEEK_FLAG_BACKWARD);
      if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("ST20P TX(%d): av_seek_frame failed: %s", ctx->idx, errbuf);
      }
      avcodec_flush_buffers(ctx->codec_ctx);
      LOG_DEBUG("ST20P TX(%d): FFmpeg loop restart", ctx->idx);
      continue;
    }
    if (ret < 0) {
      av_strerror(ret, errbuf, sizeof(errbuf));
      LOG_ERROR("ST20P TX(%d): av_read_frame failed: %s", ctx->idx, errbuf);
      continue;
    }
    if (ctx->av_packet->stream_index != ctx->video_stream_idx) {
      av_packet_unref(ctx->av_packet);
      continue;
    }

    ret = avcodec_send_packet(ctx->codec_ctx, ctx->av_packet);
    av_packet_unref(ctx->av_packet);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
      av_strerror(ret, errbuf, sizeof(errbuf));
      LOG_ERROR("ST20P TX(%d): avcodec_send_packet failed: %s", ctx->idx, errbuf);
      continue;
    }

    ret = avcodec_receive_frame(ctx->codec_ctx, ctx->av_frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) continue;
    if (ret < 0) {
      av_strerror(ret, errbuf, sizeof(errbuf));
      LOG_ERROR("ST20P TX(%d): avcodec_receive_frame failed: %s", ctx->idx, errbuf);
      break;
    }

    int rows = convert_frame_format(ctx->sws_ctx, ctx->av_frame,
                                    ctx->codec_ctx->height, ctx->yuv_frame);
    av_frame_unref(ctx->av_frame);
    if (rows <= 0) {
      LOG_ERROR("ST20P TX(%d): convert_frame_format failed (ret=%d)",
                ctx->idx, rows);
      return false;
    }
    return true;
  }
  return false;
}
