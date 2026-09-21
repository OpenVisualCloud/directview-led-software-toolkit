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
#include <unistd.h>
#include <sys/stat.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <libavdevice/avdevice.h>

/* =========================================================================
 * Helpers
 * ========================================================================= */

/*
 * ffmpeg_resolve_sws_threads() — pick the libswscale slice-thread count.
 *
 * Auto-select half of the online CPUs, capped at 8: measured on a 20-core
 * host, the conversion scales ~6x up to 8 threads and then plateaus (12
 * threads is measurably worse than 8 due to slice-granularity and SMT
 * contention). The cap also leaves cores for the H.264 decoder threads and
 * the MTL lcores.
 */
int ffmpeg_resolve_sws_threads(void) {
  long online = sysconf(_SC_NPROCESSORS_ONLN);
  if (online < 1) online = 1;

  int threads = (int)(online / 2);
  if (threads < 1) threads = 1;
  if (threads > 8) threads = 8;
  return threads;
}


bool is_raw_yuv(const char* filename) {
  const char* ext = strrchr(filename, '.');
  if (ext == NULL) return false;
  return (strcasecmp(ext, ".yuv") == 0 || strcasecmp(ext, ".raw") == 0);
}

/* =========================================================================
 * libswscale context construction
 * ========================================================================= */

/*
 * create_sws_ctx() — build the decode→transport colour converter.
 *
 * Built explicitly rather than with sws_getContext(): the "threads" option
 * must be set between allocation and initialisation, and sws_getContext()
 * does both in one call. Slice threading is what keeps the conversion off the
 * critical path when the source resolution or pixel format differs from the
 * transport one.
 */
static int create_sws_ctx(struct SwsContext** out_sws_ctx,
                          int src_w, int src_h, enum AVPixelFormat src_fmt,
                          int dst_w, int dst_h, enum AVPixelFormat dst_fmt,
                          const char* log_prefix) {
  char errbuf[256];

  int sws_threads = ffmpeg_resolve_sws_threads();
  *out_sws_ctx = sws_alloc_context();
  if (*out_sws_ctx == NULL) {
    LOG_ERROR("%s: sws_alloc_context failed", log_prefix);
    return -1;
  }
  av_opt_set_int(*out_sws_ctx, "srcw",       src_w,             0);
  av_opt_set_int(*out_sws_ctx, "srch",       src_h,             0);
  av_opt_set_int(*out_sws_ctx, "src_format", src_fmt,           0);
  av_opt_set_int(*out_sws_ctx, "dstw",       dst_w,             0);
  av_opt_set_int(*out_sws_ctx, "dsth",       dst_h,             0);
  av_opt_set_int(*out_sws_ctx, "dst_format", dst_fmt,           0);
  av_opt_set_int(*out_sws_ctx, "sws_flags",  SWS_FAST_BILINEAR, 0);
  av_opt_set_int(*out_sws_ctx, "threads",    sws_threads,       0);

  int ret = sws_init_context(*out_sws_ctx, NULL, NULL);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    LOG_ERROR("%s: sws_init_context failed: %s", log_prefix, errbuf);
    sws_freeContext(*out_sws_ctx);
    *out_sws_ctx = NULL;
    return -1;
  }

  /* libswscale clamps the request (e.g. to 1 when built without threading). */
  int64_t sws_threads_actual = sws_threads;
  av_opt_get_int(*out_sws_ctx, "threads", 0, &sws_threads_actual);

  LOG_INFO("%s: scaler %dx%d %s -> %dx%d %s (sws_threads=%d)", log_prefix,
           src_w, src_h, av_get_pix_fmt_name(src_fmt),
           dst_w, dst_h, ffmpeg_fmt_name(dst_fmt), (int)sws_threads_actual);
  return 0;
}

/*
 * ensure_sws_ctx() — (re)build the scaler when it does not match src.
 *
 * With hardware decoding the source pixel format is only known once the first
 * surface has been downloaded, so the context cannot be built up front. This
 * also covers a mid-stream resolution or format change.
 */
static int ensure_sws_ctx(struct SwsContext** sws_ctx, const AVFrame* src,
                          const AVFrame* dst, const char* log_prefix) {
  if (src->width <= 0 || src->height <= 0 || src->format == AV_PIX_FMT_NONE) {
    LOG_ERROR("%s: decoded frame has invalid geometry %dx%d fmt=%d",
              log_prefix, src->width, src->height, src->format);
    return -1;
  }

  if (*sws_ctx != NULL) {
    int64_t srcw = 0, srch = 0, src_format = AV_PIX_FMT_NONE;
    av_opt_get_int(*sws_ctx, "srcw",       0, &srcw);
    av_opt_get_int(*sws_ctx, "srch",       0, &srch);
    av_opt_get_int(*sws_ctx, "src_format", 0, &src_format);
    if (srcw == src->width && srch == src->height && src_format == src->format)
      return 0;

    sws_freeContext(*sws_ctx);
    *sws_ctx = NULL;
  }

  return create_sws_ctx(sws_ctx, src->width, src->height, src->format,
                        dst->width, dst->height, (enum AVPixelFormat)dst->format,
                        log_prefix);
}

/* =========================================================================
 * Hardware decode (VA-API)
 *
 * Only the compressed-bitstream decode is offloaded to the GPU's fixed
 * function video engine. Decoded surfaces are downloaded to system memory so
 * the existing scale/crop/TX path is unchanged: MTL transmits from its own
 * DMA buffers, so there is no GPU-to-NIC zero-copy path to keep the frame on
 * the device for.
 * ========================================================================= */

/* Selected via AVCodecContext.opaque so the callback stays per-context. */
static enum AVPixelFormat hwaccel_get_format(AVCodecContext* codec_ctx,
                                             const enum AVPixelFormat* fmts) {
  enum AVPixelFormat wanted = (enum AVPixelFormat)(intptr_t)codec_ctx->opaque;

  for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
    if (*p == wanted) return *p;

  /* The decoder cannot serve the surface format the device was created for
   * (unsupported profile, e.g. a 4:2:2 or 12-bit stream on a device that only
   * decodes 4:2:0). Take the software format so playback continues on the CPU. */
  LOG_WARN("Hardware decode: %s surface unavailable for this stream, decoding on CPU",
           av_get_pix_fmt_name(wanted));
  return fmts[0];
}

/*
 * hwaccel_setup() — attach a VA-API device to codec_ctx.
 *
 * Returns 0 when hardware decode is armed, -1 when it is unavailable; the
 * caller decides whether that is fatal (explicit "vaapi") or not ("auto").
 */
static int hwaccel_setup(AVCodecContext* codec_ctx, const AVCodec* codec,
                         const char* log_prefix,
                         AVBufferRef** out_hw_device_ctx,
                         enum AVPixelFormat* out_hw_pix_fmt) {
  char errbuf[256];

  enum AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
  for (int i = 0;; i++) {
    const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
    if (cfg == NULL) break;
    if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 &&
        cfg->device_type == AV_HWDEVICE_TYPE_VAAPI) {
      hw_pix_fmt = cfg->pix_fmt;
      break;
    }
  }
  if (hw_pix_fmt == AV_PIX_FMT_NONE) {
    LOG_WARN("%s: decoder '%s' has no VA-API configuration "
             "(FFmpeg built without VA-API, or codec unsupported)",
             log_prefix, codec->name);
    return -1;
  }

  AVBufferRef* hw_device_ctx = NULL;
  /* NULL device: libavutil picks the display or the first DRM render node. */
  int ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                   NULL, NULL, 0);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    LOG_WARN("%s: cannot open VA-API device: %s", log_prefix, errbuf);
    return -1;
  }

  codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
  if (codec_ctx->hw_device_ctx == NULL) {
    LOG_WARN("%s: av_buffer_ref failed for VA-API device", log_prefix);
    av_buffer_unref(&hw_device_ctx);
    return -1;
  }
  codec_ctx->opaque     = (void*)(intptr_t)hw_pix_fmt;
  codec_ctx->get_format = hwaccel_get_format;

  *out_hw_device_ctx = hw_device_ctx;
  *out_hw_pix_fmt    = hw_pix_fmt;
  return 0;
}

/*
 * hwaccel_map_to_cpu() — return a CPU-readable view of a decoded frame.
 *
 * Downloads the surface into sw_frame when the decoder produced a hardware
 * frame, otherwise returns src untouched. Returns NULL when the download
 * fails so the caller can drop the frame.
 */
static AVFrame* hwaccel_map_to_cpu(const AVBufferRef* hw_device_ctx,
                                   enum AVPixelFormat hw_pix_fmt,
                                   AVFrame* src, AVFrame* sw_frame,
                                   const char* log_prefix) {
  if (hw_device_ctx == NULL || sw_frame == NULL || src->format != hw_pix_fmt)
    return src;

  av_frame_unref(sw_frame);
  int ret = av_hwframe_transfer_data(sw_frame, src, 0);
  if (ret < 0) {
    char errbuf[256];
    av_strerror(ret, errbuf, sizeof(errbuf));
    LOG_ERROR("%s: av_hwframe_transfer_data failed: %s", log_prefix, errbuf);
    return NULL;
  }
  return sw_frame;
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

      /* Hardware decode: bring the GPU surface into system memory first.
       * No-op when decoding on the CPU. */
      AVFrame* src_frame = hwaccel_map_to_cpu(dec->hw_device_ctx, dec->hw_pix_fmt,
                                              dec->av_frame, dec->sw_frame,
                                              "Shared decode");
      if (src_frame == NULL) {
        av_frame_unref(dec->av_frame);
        continue;
      }

      /* The scaler is built here rather than at open time for the hardware
       * path, where the download format is only known now. */
      if (ensure_sws_ctx(&dec->sws_ctx, src_frame, dec->yuv_frame,
                         "Shared decode") < 0) {
        av_frame_unref(dec->av_frame);
        continue;
      }

      /* Colour-convert + chroma upsample: e.g. yuv420p (8-bit) → yuv422p10le.
       * Output goes into dec->yuv_frame — the single shared full-width (1920px)
       * buffer that all TX threads will read from simultaneously. */
      int rows = convert_frame_format(dec->sws_ctx, src_frame,
                                      src_frame->height, dec->yuv_frame);
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
 *   open2 -> sws context -> alloc frames/packet -> alloc yuv_frame buffer.
 *
 * open_ffmpeg_decoder() extracts this common logic.  The caller passes in
 * pointers to the target struct's fields.
 * ========================================================================= */
static int open_ffmpeg_decoder(
    const char* filename, const char* log_prefix,
    bool use_screen_capture, const char* screen_input, int capture_w, int capture_h, int capture_fps,
    enum AVPixelFormat target_fmt, int target_w, int target_h,
    bool hwaccel,
    AVFormatContext** out_fmt_ctx, AVCodecContext** out_codec_ctx,
    struct SwsContext** out_sws_ctx, AVFrame** out_av_frame,
    AVFrame** out_yuv_frame, AVPacket** out_av_packet,
    AVBufferRef** out_hw_device_ctx, enum AVPixelFormat* out_hw_pix_fmt,
    AVFrame** out_sw_frame,
    int* out_video_stream_idx) {
  char errbuf[256];
  int ret;

  *out_hw_device_ctx = NULL;
  *out_hw_pix_fmt    = AV_PIX_FMT_NONE;
  *out_sw_frame      = NULL;

  if (use_screen_capture == true) {
    const char* input_url = (screen_input && screen_input[0] != '\0') ? screen_input : ":0.0+0,0";
    char video_size[32];
    char framerate[16];
    snprintf(video_size, sizeof(video_size), "%dx%d", capture_w, capture_h);
    snprintf(framerate, sizeof(framerate), "%d", capture_fps > 0 ? capture_fps : 30);

    const AVInputFormat* in_fmt = av_find_input_format("x11grab");
    if (in_fmt == NULL) {
      LOG_ERROR("%s: x11grab input format not found", log_prefix);
      return -1;
    }

    AVDictionary* options = NULL;
    av_dict_set(&options, "video_size", video_size, 0);
    av_dict_set(&options, "framerate", framerate, 0);

    ret = avformat_open_input(out_fmt_ctx, input_url, in_fmt, &options);
    av_dict_free(&options);
    if (ret < 0) {
      av_strerror(ret, errbuf, sizeof(errbuf));
      LOG_ERROR("%s: cannot open x11grab source %s: %s", log_prefix, input_url, errbuf);
      return -1;
    }
  } else {
    ret = avformat_open_input(out_fmt_ctx, filename, NULL, NULL);
  }
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    LOG_ERROR("%s: cannot open %s: %s", log_prefix,
              use_screen_capture ? "x11grab input" : filename, errbuf);
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

  /* Hardware decode is armed before avcodec_open2(): the device context and
   * get_format callback must be in place when the decoder negotiates its
   * output format. */
  if (hwaccel == true) {
    if (hwaccel_setup(*out_codec_ctx, codec, log_prefix,
                      out_hw_device_ctx, out_hw_pix_fmt) < 0) {
      LOG_WARN("%s: hardware decode unavailable, using CPU decode", log_prefix);
    }
  }

  ret = avcodec_open2(*out_codec_ctx, codec, NULL);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    LOG_ERROR("%s: avcodec_open2 failed: %s", log_prefix, errbuf);
    av_buffer_unref(out_hw_device_ctx);
    avcodec_free_context(out_codec_ctx);
    avformat_close_input(out_fmt_ctx);
    return -1;
  }

  /* With hardware decode the source pixel format is only known after the
   * first surface is downloaded, so the scaler is built on the first frame
   * (ensure_sws_ctx) instead of here. */
  if (*out_hw_device_ctx == NULL) {
    ret = create_sws_ctx(out_sws_ctx,
                         (*out_codec_ctx)->width, (*out_codec_ctx)->height,
                         (*out_codec_ctx)->pix_fmt,
                         target_w, target_h, target_fmt, log_prefix);
    if (ret < 0) {
      avcodec_free_context(out_codec_ctx);
      avformat_close_input(out_fmt_ctx);
      return -1;
    }
  } else {
    *out_sws_ctx = NULL;
  }

  *out_av_frame  = av_frame_alloc();
  *out_yuv_frame = av_frame_alloc();
  *out_av_packet = av_packet_alloc();
  if (*out_hw_device_ctx != NULL) *out_sw_frame = av_frame_alloc();
  if (*out_av_frame == NULL || *out_yuv_frame == NULL || *out_av_packet == NULL ||
      (*out_hw_device_ctx != NULL && *out_sw_frame == NULL)) {
    LOG_ERROR("%s: frame/packet allocation failed", log_prefix);
    av_frame_free(out_av_frame);
    av_frame_free(out_yuv_frame);
    av_frame_free(out_sw_frame);
    av_packet_free(out_av_packet);
    sws_freeContext(*out_sws_ctx); *out_sws_ctx = NULL;
    av_buffer_unref(out_hw_device_ctx);
    avcodec_free_context(out_codec_ctx);
    avformat_close_input(out_fmt_ctx);
    return -1;
  }

  /* yuv_frame must be reference-counted (av_frame_get_buffer, not
   * av_image_alloc): sws_scale_frame() reallocates any destination whose
   * buf[0] is NULL, which would leak the original buffer and leave the
   * caller writing to storage libswscale no longer targets. */
  (*out_yuv_frame)->format = target_fmt;
  (*out_yuv_frame)->width  = target_w;
  (*out_yuv_frame)->height = target_h;
  ret = av_frame_get_buffer(*out_yuv_frame, 32);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    LOG_ERROR("%s: av_frame_get_buffer failed: %s", log_prefix, errbuf);
    av_frame_free(out_av_frame);
    av_frame_free(out_yuv_frame);
    av_frame_free(out_sw_frame);
    av_packet_free(out_av_packet);
    sws_freeContext(*out_sws_ctx); *out_sws_ctx = NULL;
    av_buffer_unref(out_hw_device_ctx);
    avcodec_free_context(out_codec_ctx);
    avformat_close_input(out_fmt_ctx);
    return -1;
  }

  LOG_INFO("%s: opened '%s' Codec=%s %dx%d %s -> %dx%d %s (hwaccel=%s)",
           log_prefix, filename, codec->name,
           (*out_codec_ctx)->width, (*out_codec_ctx)->height,
           av_get_pix_fmt_name((*out_codec_ctx)->pix_fmt),
           target_w, target_h, ffmpeg_fmt_name(target_fmt),
           (*out_hw_device_ctx != NULL) ? "vaapi" : "none");
  return 0;
}

/* Release common FFmpeg decoder resources.  NULL-safe for each field. */
static void close_ffmpeg_decoder(
    AVFormatContext** fmt_ctx, AVCodecContext** codec_ctx,
    struct SwsContext** sws_ctx, AVFrame** av_frame,
    AVFrame** yuv_frame, AVPacket** av_packet,
    AVBufferRef** hw_device_ctx, AVFrame** sw_frame) {
  if (*av_frame != NULL)  av_frame_free(av_frame);
  if (*sw_frame != NULL)  av_frame_free(sw_frame);
  if (*yuv_frame != NULL) {
    /* Reference-counted frames (av_frame_get_buffer) release their storage in
     * av_frame_free(). Only a frame whose data[] came from av_image_alloc
     * — buf[0] == NULL — needs the explicit free. */
    if ((*yuv_frame)->buf[0] == NULL)
      av_freep(&(*yuv_frame)->data[0]);
    av_frame_free(yuv_frame);
  }
  if (*av_packet != NULL) av_packet_free(av_packet);
  if (*sws_ctx != NULL)   { sws_freeContext(*sws_ctx); *sws_ctx = NULL; }
  if (*codec_ctx != NULL) avcodec_free_context(codec_ctx);
  if (*fmt_ctx != NULL)   avformat_close_input(fmt_ctx);
  /* Released last: the decoder holds its own reference until it is freed. */
  if (*hw_device_ctx != NULL) av_buffer_unref(hw_device_ctx);
}

/* =========================================================================
 * Open/close shared FFmpeg decoder (multi-session input path)
 * ========================================================================= */
int open_shared_ffmpeg(struct shared_decode_ctx* dec, const char* filename) {
  const struct dvledtx_context* app = dec->app;
  int target_w = (int)(app->scale_width  > 0 ? app->scale_width  : app->width);
  int target_h = (int)(app->scale_height > 0 ? app->scale_height : app->height);
  /* filename may be empty when screen capture is enabled (app->tx_url is
   * not required in that mode); use the screen_input descriptor instead so
   * log messages stay meaningful. */
  const char* effective_source = app->use_screen_capture ? app->screen_input : filename;
  return open_ffmpeg_decoder(
    effective_source, "Shared decode",
    app->use_screen_capture, app->screen_input, (int)app->width, (int)app->height, app->fps,
    app->fmt, target_w, target_h,
    app->hwaccel,
    &dec->fmt_ctx, &dec->codec_ctx, &dec->sws_ctx,
    &dec->av_frame, &dec->yuv_frame, &dec->av_packet,
    &dec->hw_device_ctx, &dec->hw_pix_fmt, &dec->sw_frame,
    &dec->video_stream_idx);
}

void close_shared_ffmpeg(struct shared_decode_ctx* dec) {
  close_ffmpeg_decoder(
    &dec->fmt_ctx, &dec->codec_ctx, &dec->sws_ctx,
    &dec->av_frame, &dec->yuv_frame, &dec->av_packet,
    &dec->hw_device_ctx, &dec->sw_frame);
}

/* =========================================================================
 * Per-session FFmpeg input decoder (single-session path)
 * ========================================================================= */
static int open_ffmpeg_source(struct st20p_tx_ctx* ctx, const char* filename) {
  char log_prefix[64];
  snprintf(log_prefix, sizeof(log_prefix), "ST20P TX(%d)", ctx->idx);
  int target_w = (int)(ctx->app->scale_width  > 0 ? ctx->app->scale_width  : ctx->app->width);
  int target_h = (int)(ctx->app->scale_height > 0 ? ctx->app->scale_height : ctx->app->height);
  int ret = open_ffmpeg_decoder(
    filename, log_prefix,
    ctx->app->use_screen_capture, ctx->app->screen_input, (int)ctx->app->width, (int)ctx->app->height, ctx->app->fps,
    ctx->app->fmt, target_w, target_h,
    ctx->app->hwaccel,
    &ctx->fmt_ctx, &ctx->codec_ctx, &ctx->sws_ctx,
    &ctx->av_frame, &ctx->yuv_frame, &ctx->av_packet,
    &ctx->hw_device_ctx, &ctx->hw_pix_fmt, &ctx->sw_frame,
    &ctx->video_stream_idx);
  if (ret == 0) ctx->use_ffmpeg = true;
  return ret;
}

void close_ffmpeg_source(struct st20p_tx_ctx* ctx) {
  if (ctx->use_ffmpeg == false) return;
  close_ffmpeg_decoder(
    &ctx->fmt_ctx, &ctx->codec_ctx, &ctx->sws_ctx,
    &ctx->av_frame, &ctx->yuv_frame, &ctx->av_packet,
    &ctx->hw_device_ctx, &ctx->sw_frame);
}

/* =========================================================================
 * Video source loading
 * ========================================================================= */
int load_video_source(struct st20p_tx_ctx* ctx, const char* filename) {
  if (ctx->app->use_screen_capture == true) {
    return open_ffmpeg_source(ctx, ctx->app->screen_input);
  }

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
  /* sws_ctx is not checked: with hardware decode it is built lazily by
   * ensure_sws_ctx() once the downloaded frame format is known. */
  if (ctx->fmt_ctx == NULL || ctx->codec_ctx == NULL ||
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

    char log_prefix[64];
    snprintf(log_prefix, sizeof(log_prefix), "ST20P TX(%d)", ctx->idx);

    AVFrame* src_frame = hwaccel_map_to_cpu(ctx->hw_device_ctx, ctx->hw_pix_fmt,
                                            ctx->av_frame, ctx->sw_frame, log_prefix);
    if (src_frame == NULL) {
      av_frame_unref(ctx->av_frame);
      continue;
    }

    if (ensure_sws_ctx(&ctx->sws_ctx, src_frame, ctx->yuv_frame, log_prefix) < 0) {
      av_frame_unref(ctx->av_frame);
      return false;
    }

    int rows = convert_frame_format(ctx->sws_ctx, src_frame,
                                    src_frame->height, ctx->yuv_frame);
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
