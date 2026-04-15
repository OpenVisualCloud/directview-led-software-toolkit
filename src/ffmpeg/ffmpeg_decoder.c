/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#include "ffmpeg_decoder.h"
#include "tx_app_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

/* Defined in session_manager.c — set by session_manager_stop() */
extern _Atomic bool g_tx_app_exit;

/* =========================================================================
 * Helpers
 * ========================================================================= */

static const char* fmt_name(enum AVPixelFormat fmt) {
  const char* n = av_get_pix_fmt_name(fmt);
  return n ? n : "unknown";
}

bool is_raw_yuv(const char* filename) {
  const char* ext = strrchr(filename, '.');
  if (!ext) return false;
  return (strcasecmp(ext, ".yuv") == 0 || strcasecmp(ext, ".raw") == 0);
}

/* =========================================================================
 * FFmpeg output session: open MTL Kahawai (mtl_st20p) output device
 * No encoder; codec params are set directly on the stream.
 * ========================================================================= */
/*
 * open_ffmpeg_output() — called once per session during init.
 *
 * Creates an AVFormatContext backed by the "mtl_st20p" muxer (Intel MTL Kahawai
 * FFmpeg plugin). This is NOT a file muxer — it is AVFMT_NOFILE, meaning MTL
 * manages its own TX ring buffer internally; no URL or avio is needed.
 *
 * Steps:
 *   1. Locate the mtl_st20p muxer by name.
 *   2. Allocate output context and set NIC/IP/port options via AVOptions.
 *   3. Add a raw-video stream (no encoder — MTL accepts raw packed frames).
 *   4. Call avformat_write_header() which internally calls mtl_st20p_write_header()
 *      → this is where MTL allocates its TX session and DMA ring buffers.
 *   5. Allocate enc_frame (crop-width scratch) and enc_pkt (pre-allocated
 *      contiguous frame buffer whose size must match MTL's internal frame_size).
 */
int open_ffmpeg_output(struct st20p_tx_ctx* ctx) {
  /* out_w = crop strip width (640px for 3 sessions); falls back to full width */
  int out_w  = ctx->crop_width > 0 ? ctx->crop_width : ctx->app->width;
  int height = ctx->crop_height > 0 ? ctx->crop_height : ctx->app->height;

  /* Locate the MTL muxer compiled into this FFmpeg build */
  const AVOutputFormat* fmt = av_guess_format("mtl_st20p", NULL, NULL);
  if (!fmt) {
    printf("ST20P TX(%d): mtl_st20p muxer not found - "
           "rebuild FFmpeg with Intel MTL plugin (--enable-mtl)\n", ctx->idx);
    return -1;
  }

  /* mtl_st20p is AVFMT_NOFILE: no URL required */
  int ret = avformat_alloc_output_context2(&ctx->out_fmt_ctx, fmt, NULL, NULL);
  if (ret < 0 || !ctx->out_fmt_ctx) {
    printf("ST20P TX(%d): cannot alloc output ctx for mtl_st20p\n", ctx->idx);
    return -1;
  }

  /* Set MTL device and session options via AVOptions.
   * p_port   = DPDK NIC PCI address (e.g. 0000:02:00.0)
   * p_sip    = source IP of this TX machine
   * p_tx_ip  = multicast destination IP (same for all sessions currently)
   * udp_port = base port + session_idx*2 → 20000, 20002, 20004 per session
   * payload_type = RTP dynamic payload type (default 96)
   * For CLI-only runs session_net[] is zero-initialised (memset in main).
   * Port 0 / payload_type 0 are not valid for RTP streaming, so 0 reliably
   * indicates "not configured via JSON" and we fall back to app-level defaults. */
  int udp_port = ctx->app->session_net[ctx->idx].udp_port;
  if (udp_port == 0) udp_port = ctx->app->udp_port + (ctx->idx * 2);

  int payload_type = ctx->app->session_net[ctx->idx].payload_type;
  if (payload_type == 0) payload_type = ctx->app->payload_type;

  av_opt_set    (ctx->out_fmt_ctx->priv_data, "p_port",  ctx->app->port,         0);
  av_opt_set    (ctx->out_fmt_ctx->priv_data, "p_sip",   ctx->app->sip_addr_str, 0);
  av_opt_set    (ctx->out_fmt_ctx->priv_data, "p_tx_ip", ctx->app->dip_addr_str, 0);
  av_opt_set_int(ctx->out_fmt_ctx->priv_data, "udp_port",     (int64_t)udp_port,     0);
  av_opt_set_int(ctx->out_fmt_ctx->priv_data, "payload_type", (int64_t)payload_type, 0);

  /* Add a raw-video stream — no encoder is used; MTL accepts raw packed pixel data.
   * codec_id = AV_CODEC_ID_RAWVIDEO signals to the muxer that packets are
   * already uncompressed and ready to copy into the TX ring. */
  ctx->out_stream = avformat_new_stream(ctx->out_fmt_ctx, NULL);
  if (!ctx->out_stream) {
    avformat_free_context(ctx->out_fmt_ctx); ctx->out_fmt_ctx = NULL;
    return -1;
  }
  ctx->out_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
  ctx->out_stream->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
  ctx->out_stream->codecpar->width      = out_w;   /* crop strip width, e.g. 640 */
  ctx->out_stream->codecpar->height     = height;  /* full frame height, e.g. 1080 */
  ctx->out_stream->codecpar->format     = ctx->app->fmt; /* e.g. AV_PIX_FMT_YUV422P10LE */
  ctx->out_stream->avg_frame_rate       = (AVRational){ctx->app->fps, 1};
  ctx->out_stream->time_base            = (AVRational){1, ctx->app->fps};

  /* avformat_write_header → mtl_st20p_write_header() inside the MTL plugin:
   * allocates the MTL TX session, DMA hugepage ring buffers, and starts the
   * DPDK TX scheduler for this session. Failure here means MTL rejected the
   * pixel format or ran out of TX queues. */
  ret = avformat_write_header(ctx->out_fmt_ctx, NULL);
  if (ret < 0) {
    char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
    printf("ST20P TX(%d): avformat_write_header (mtl_st20p) failed: %s\n",
           ctx->idx, errbuf);
    avformat_free_context(ctx->out_fmt_ctx); ctx->out_fmt_ctx = NULL;
    return -1;
  }

  /* Scratch frame used by send_video_frame to stage the cropped region */
  ctx->enc_frame = av_frame_alloc();
  if (!ctx->enc_frame) {
    avformat_free_context(ctx->out_fmt_ctx); ctx->out_fmt_ctx = NULL;
    return -1;
  }
  ctx->enc_frame->format = ctx->app->fmt;
  ctx->enc_frame->width  = out_w;
  ctx->enc_frame->height = height;
  ret = av_frame_get_buffer(ctx->enc_frame, 32);
  if (ret < 0) {
    av_frame_free(&ctx->enc_frame);
    avformat_free_context(ctx->out_fmt_ctx); ctx->out_fmt_ctx = NULL;
    return -1;
  }

  /* Pre-allocate enc_pkt with the exact packed frame size (no stride padding,
   * align=1). MTL's write_packet validates pkt->size == its internal frame_size;
   * a mismatch causes a silent drop or crash, so this must be exact. */
  int frame_sz = av_image_get_buffer_size(ctx->app->fmt, out_w, height, 1);
  if (frame_sz < 0) {
    av_frame_free(&ctx->enc_frame);
    avformat_free_context(ctx->out_fmt_ctx); ctx->out_fmt_ctx = NULL;
    return -1;
  }
  ctx->enc_pkt = av_packet_alloc();
  if (!ctx->enc_pkt || av_new_packet(ctx->enc_pkt, frame_sz) < 0) {
    av_packet_free(&ctx->enc_pkt);
    av_frame_free(&ctx->enc_frame);
    avformat_free_context(ctx->out_fmt_ctx); ctx->out_fmt_ctx = NULL;
    return -1;
  }

  ctx->pts = 0;
  printf("ST20P TX(%d): Kahawai (mtl_st20p) output opened "
         "(%dx%d %s @ %dfps) -> %s:%u via %s\n",
         ctx->idx, out_w, height, fmt_name(ctx->app->fmt), ctx->app->fps,
         ctx->app->dip_addr_str,
         (unsigned)udp_port,
         ctx->app->port);
  return 0;
}

void close_ffmpeg_output(struct st20p_tx_ctx* ctx) {
  if (ctx->out_fmt_ctx) {
    av_write_trailer(ctx->out_fmt_ctx);
    /* mtl_st20p is AVFMT_NOFILE: no avio to close */
    avformat_free_context(ctx->out_fmt_ctx);
    ctx->out_fmt_ctx = NULL;
  }
  /* enc_ctx is not used in the Kahawai path (no encoder) */
  if (ctx->enc_frame) av_frame_free(&ctx->enc_frame);
  if (ctx->enc_pkt)   av_packet_free(&ctx->enc_pkt);
}

/* =========================================================================
 * send_video_frame — crop, pack and transmit one frame via mtl_st20p
 * ========================================================================= */
/*
 * send_video_frame() — called by each TX thread once per frame.
 *
 * src      = full-width shared yuv_frame (1920px, all 3 TX threads read this)
 * crop_x   = horizontal pixel offset for this session (0, 640, 1280)
 * crop_w   = width of this session's strip (640px)
 *
 * Three steps:
 *   1. memcpy: extract the vertical strip from each YUV plane of src into
 *      enc_frame (the per-session 640px scratch buffer).
 *   2. av_image_copy_to_buffer: pack the planar enc_frame into enc_pkt->data
 *      as a contiguous byte stream (no per-line stride padding) that MTL expects.
 *   3. av_write_frame: hand enc_pkt to the mtl_st20p muxer which copies it
 *      into the MTL DMA TX ring → DPDK sends it as ST 2110-20 RTP packets.
 */
void send_video_frame(struct st20p_tx_ctx* ctx, AVFrame* src,
                      int crop_x, int crop_y, int crop_w, int crop_h) {
  if (!ctx->out_fmt_ctx || !ctx->enc_frame || !ctx->enc_pkt) return;

  /* Ensure enc_frame's buffer is not shared/ref-counted before writing */
  av_frame_make_writable(ctx->enc_frame);

  /* Compute per-plane copy parameters from AVPixFmtDescriptor so all
   * planar formats (YUV 4:2:0, 4:2:2, 4:4:4, GBRP, etc.) are handled
   * correctly regardless of bit depth. */
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(ctx->app->fmt);
  if (!desc) return;

  /* bytes per sample (e.g. 1 for 8-bit, 2 for 10/12/16-bit packed in 16-bit) */
  int bytes_per_sample = (desc->comp[0].depth + 7) / 8;
  int chroma_w_shift   = desc->log2_chroma_w; /* 0 = full width, 1 = half (4:2:x) */
  int chroma_h_shift   = desc->log2_chroma_h; /* 0 = full height, 1 = half (4:2:0) */

  /* Step 1a: Copy luma (Y / G) plane */
  for (int line = 0; line < crop_h; line++)
    memcpy(ctx->enc_frame->data[0] + line * ctx->enc_frame->linesize[0],
           src->data[0] + (crop_y + line) * src->linesize[0] + crop_x * bytes_per_sample,
           crop_w * bytes_per_sample);

  /* Step 1b: Copy Cb (U / B) chroma plane */
  int chroma_h = crop_h >> chroma_h_shift;
  int chroma_y = crop_y >> chroma_h_shift;
  int chroma_x = crop_x >> chroma_w_shift;
  int chroma_w = crop_w >> chroma_w_shift;
  for (int line = 0; line < chroma_h; line++)
    memcpy(ctx->enc_frame->data[1] + line * ctx->enc_frame->linesize[1],
           src->data[1] + (chroma_y + line) * src->linesize[1] + chroma_x * bytes_per_sample,
           chroma_w * bytes_per_sample);

  /* Step 1c: Copy Cr (V / R) chroma plane */
  for (int line = 0; line < chroma_h; line++)
    memcpy(ctx->enc_frame->data[2] + line * ctx->enc_frame->linesize[2],
           src->data[2] + (chroma_y + line) * src->linesize[2] + chroma_x * bytes_per_sample,
           chroma_w * bytes_per_sample);

  /* Step 2: Pack planar enc_frame (Y/U/V separate planes with stride) into
   * enc_pkt->data as a single contiguous buffer (align=1, no padding).
   * MTL's write_packet validates size == frame_size allocated at open time. */
  int packed = av_image_copy_to_buffer(
      ctx->enc_pkt->data, ctx->enc_pkt->size,
      (const uint8_t* const*)ctx->enc_frame->data, ctx->enc_frame->linesize,
      ctx->app->fmt, crop_w, crop_h, 1);
  if (packed < 0) return;

  /* Step 3: Set packet metadata and hand to mtl_st20p muxer.
   * av_write_frame → mtl_st20p_write_packet() → copies data into MTL TX ring.
   * MTL's DPDK scheduler then packetizes and transmits as RTP/UDP. */
  ctx->enc_pkt->size         = packed;
  ctx->enc_pkt->pts          = ctx->pts;      /* presentation timestamp */
  ctx->enc_pkt->dts          = ctx->pts++;    /* decode timestamp, post-increment */
  ctx->enc_pkt->pos          = -1;            /* no file position (live stream) */
  ctx->enc_pkt->stream_index = ctx->out_stream->index;
  av_write_frame(ctx->out_fmt_ctx, ctx->enc_pkt);
  /* Restore size: av_write_frame may modify pkt->size; restore for next frame */
  ctx->enc_pkt->size = packed;

  ctx->frames_sent++;
  if (ctx->frames_sent % 100 == 0)
    printf("ST20P TX(%d): sent %d frames\n", ctx->idx, ctx->frames_sent);
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

  printf("Shared decode thread started (%d sessions)\n", dec->num_sessions);

  /* Frame period in nanoseconds for FPS-based pacing (e.g. 33 333 333 ns @ 30fps).
   * The decode thread sleeps after each barrier_copied to ensure frames are
   * fed into the MTL TX ring at the configured rate. Without pacing, FFmpeg
   * decodes much faster than the network can transmit, causing MTL's 3-deep
   * frame ring to fill up and log st20p_tx_get_frame timeout on every frame. */
  int fps = dec->app->fps > 0 ? dec->app->fps : 30;
  long frame_period_ns = 1000000000L / fps;
  struct timespec last_frame_ts;
  clock_gettime(CLOCK_MONOTONIC, &last_frame_ts);

  while (!dec->exit && !g_tx_app_exit) {
    bool got_frame = false;

    /* Inner loop: keep reading packets until one decoded frame is produced.
     * H.264 requires multiple packets before the first frame due to B-frames. */
    while (!got_frame && !dec->exit && !g_tx_app_exit) {
      /* Read next compressed packet from the MP4 container */
      int ret = av_read_frame(dec->fmt_ctx, dec->av_packet);
      if (ret == AVERROR_EOF) {
        /* End of file — seek back to start and flush decoder for looping */
        av_seek_frame(dec->fmt_ctx, dec->video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec->codec_ctx);
        printf("Shared decode: loop restart\n");
        continue;
      }
      if (ret < 0) continue;

      /* MP4 interleaves video+audio packets; skip non-video streams */
      if (dec->av_packet->stream_index != dec->video_stream_idx) {
        av_packet_unref(dec->av_packet);
        continue;
      }

      /* Push compressed H.264 NAL unit into the decoder's input queue */
      avcodec_send_packet(dec->codec_ctx, dec->av_packet);
      av_packet_unref(dec->av_packet); /* release compressed buffer immediately */

      /* Try to pull a decoded raw frame from the decoder's output queue.
       * EAGAIN = decoder needs more packets (B-frame reorder) → loop again.
       * 0      = got a full decoded frame in dec->av_frame (yuv420p). */
      ret = avcodec_receive_frame(dec->codec_ctx, dec->av_frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) continue;
      if (ret < 0) break;

      /* Colour-convert + chroma upsample: yuv420p (8-bit) → yuv422p10le (10-bit).
       * Output goes into dec->yuv_frame — the single shared full-width (1920px)
       * buffer that all TX threads will read from simultaneously. */
      sws_scale(dec->sws_ctx,
                (const uint8_t* const*)dec->av_frame->data,
                dec->av_frame->linesize, 0, dec->codec_ctx->height,
                dec->yuv_frame->data, dec->yuv_frame->linesize);

      av_frame_unref(dec->av_frame); /* return decoded frame back to FFmpeg pool */
      dec->frame_counter++;
      got_frame = true;
    }

    if (!got_frame) break; /* error or exit — leave main loop */

    /* SYNC POINT 1 — barrier_decoded (count = N+1: decode thread + N TX threads).
     * Decode thread arrives here after sws_scale is complete.
     * All N TX threads arrive after their previous barrier_copied.
     * Once all N+1 arrive → all are released → TX threads start reading yuv_frame. */
    pthread_barrier_wait(&dec->barrier_decoded);

    /* SYNC POINT 2 — barrier_copied (count = N+1).
     * Decode thread arrives here immediately after barrier_decoded.
     * N TX threads arrive after send_video_frame() completes.
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

  printf("Shared decode thread stopped, decoded %u frames\n", dec->frame_counter);
  return NULL;
}

/* =========================================================================
 * Open shared FFmpeg decoder (multi-session input path)
 * ========================================================================= */
/*
 * open_shared_ffmpeg() — opens ONE decoder used by all N TX sessions.
 *
 * Allocates:
 *   fmt_ctx    = AVFormatContext  (demuxer for the MP4/MKV container)
 *   codec_ctx  = AVCodecContext   (H.264 decoder, thread_count=4)
 *   sws_ctx    = SwsContext       (yuv420p → yuv422p10le colour converter)
 *   av_frame   = scratch for raw decoded frames (yuv420p, temporary per-frame)
 *   yuv_frame  = permanent full-width output buffer (yuv422p10le, 1920×1080)
 *                shared read-only by all TX threads between the two barriers
 *   av_packet  = scratch for compressed packets
 */
int open_shared_ffmpeg(struct shared_decode_ctx* dec, const char* filename) {
  char errbuf[256];
  int ret;
  struct tx_app_context* app = dec->app;

  /* Open the container file (MP4, MKV, etc.) and read stream headers */
  ret = avformat_open_input(&dec->fmt_ctx, filename, NULL, NULL);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    printf("Shared decode: cannot open %s: %s\n", filename, errbuf);
    return -1;
  }
  avformat_find_stream_info(dec->fmt_ctx, NULL);

  dec->video_stream_idx = av_find_best_stream(dec->fmt_ctx, AVMEDIA_TYPE_VIDEO,
                                               -1, -1, NULL, 0);
  if (dec->video_stream_idx < 0) {
    printf("Shared decode: no video stream in %s\n", filename);
    avformat_close_input(&dec->fmt_ctx);
    return -1;
  }

  AVStream* stream = dec->fmt_ctx->streams[dec->video_stream_idx];
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) { avformat_close_input(&dec->fmt_ctx); return -1; }

  dec->codec_ctx = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(dec->codec_ctx, stream->codecpar);
  dec->codec_ctx->thread_count = 4; /* use 4 CPU threads for H.264 slice decoding */
  ret = avcodec_open2(dec->codec_ctx, codec, NULL);
  if (ret < 0) {
    avcodec_free_context(&dec->codec_ctx);
    avformat_close_input(&dec->fmt_ctx);
    return -1;
  }

  /* Create colour-space converter:
   *   input:  codec native size + format (e.g. 1920×1080 yuv420p)
   *   output: app target size + format   (e.g. 1920×1080 yuv422p10le)
   * SWS_FAST_BILINEAR: fast bilinear chroma upsampler (420→422) */
  dec->sws_ctx = sws_getContext(
    dec->codec_ctx->width,  dec->codec_ctx->height, dec->codec_ctx->pix_fmt,
    app->width,             app->height,             app->fmt,
    SWS_FAST_BILINEAR, NULL, NULL, NULL);
  if (!dec->sws_ctx) {
    avcodec_free_context(&dec->codec_ctx);
    avformat_close_input(&dec->fmt_ctx);
    return -1;
  }

  dec->av_frame  = av_frame_alloc();
  dec->yuv_frame = av_frame_alloc();
  dec->av_packet = av_packet_alloc();

  dec->yuv_frame->format = app->fmt;
  dec->yuv_frame->width  = app->width;
  dec->yuv_frame->height = app->height;
  /* Allocate the shared output buffer (yuv_frame) for full-width scaled frames.
   * align=32 for SIMD-friendly access. This buffer is written by the decode
   * thread and read concurrently by all N TX threads (protected by barriers). */
  ret = av_image_alloc(dec->yuv_frame->data, dec->yuv_frame->linesize,
                       app->width, app->height, app->fmt, 32);
  if (ret < 0) {
    av_frame_free(&dec->av_frame);
    av_frame_free(&dec->yuv_frame);
    av_packet_free(&dec->av_packet);
    sws_freeContext(dec->sws_ctx);
    avcodec_free_context(&dec->codec_ctx);
    avformat_close_input(&dec->fmt_ctx);
    return -1;
  }

  printf("Shared decode: opened '%s' Codec=%s %dx%d %s -> %dx%d %s\n",
         filename, codec->name,
         dec->codec_ctx->width, dec->codec_ctx->height,
         av_get_pix_fmt_name(dec->codec_ctx->pix_fmt),
         app->width, app->height, fmt_name(app->fmt));
  return 0;
}

void close_shared_ffmpeg(struct shared_decode_ctx* dec) {
  if (dec->av_frame)  av_frame_free(&dec->av_frame);
  if (dec->yuv_frame) {
    av_freep(&dec->yuv_frame->data[0]);
    av_frame_free(&dec->yuv_frame);
  }
  if (dec->av_packet) av_packet_free(&dec->av_packet);
  if (dec->sws_ctx)   { sws_freeContext(dec->sws_ctx); dec->sws_ctx = NULL; }
  if (dec->codec_ctx) avcodec_free_context(&dec->codec_ctx);
  if (dec->fmt_ctx)   avformat_close_input(&dec->fmt_ctx);
}

/* =========================================================================
 * Per-session FFmpeg input decoder (single-session path)
 * ========================================================================= */
static int open_ffmpeg_source(struct st20p_tx_ctx* ctx, const char* filename) {
  char errbuf[256];
  int ret;

  ret = avformat_open_input(&ctx->fmt_ctx, filename, NULL, NULL);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    printf("ST20P TX(%d): cannot open %s: %s\n", ctx->idx, filename, errbuf);
    return -1;
  }
  avformat_find_stream_info(ctx->fmt_ctx, NULL);

  ctx->video_stream_idx = av_find_best_stream(ctx->fmt_ctx, AVMEDIA_TYPE_VIDEO,
                                               -1, -1, NULL, 0);
  if (ctx->video_stream_idx < 0) {
    avformat_close_input(&ctx->fmt_ctx);
    return -1;
  }

  AVStream* stream = ctx->fmt_ctx->streams[ctx->video_stream_idx];
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) { avformat_close_input(&ctx->fmt_ctx); return -1; }

  ctx->codec_ctx = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(ctx->codec_ctx, stream->codecpar);
  ctx->codec_ctx->thread_count = 4;
  ret = avcodec_open2(ctx->codec_ctx, codec, NULL);
  if (ret < 0) {
    avcodec_free_context(&ctx->codec_ctx);
    avformat_close_input(&ctx->fmt_ctx);
    return -1;
  }

  ctx->sws_ctx = sws_getContext(
    ctx->codec_ctx->width,  ctx->codec_ctx->height,  ctx->codec_ctx->pix_fmt,
    ctx->app->width,        ctx->app->height,         ctx->app->fmt,
    SWS_FAST_BILINEAR, NULL, NULL, NULL);
  if (!ctx->sws_ctx) {
    avcodec_free_context(&ctx->codec_ctx);
    avformat_close_input(&ctx->fmt_ctx);
    return -1;
  }

  ctx->av_frame  = av_frame_alloc();
  ctx->yuv_frame = av_frame_alloc();
  ctx->av_packet = av_packet_alloc();

  ctx->yuv_frame->format = ctx->app->fmt;
  ctx->yuv_frame->width  = ctx->app->width;
  ctx->yuv_frame->height = ctx->app->height;
  ret = av_image_alloc(ctx->yuv_frame->data, ctx->yuv_frame->linesize,
                       ctx->app->width, ctx->app->height, ctx->app->fmt, 32);
  if (ret < 0) {
    av_frame_free(&ctx->av_frame);
    av_frame_free(&ctx->yuv_frame);
    av_packet_free(&ctx->av_packet);
    sws_freeContext(ctx->sws_ctx);
    avcodec_free_context(&ctx->codec_ctx);
    avformat_close_input(&ctx->fmt_ctx);
    return -1;
  }

  ctx->use_ffmpeg = true;
  printf("ST20P TX(%d): input opened '%s' Codec=%s %dx%d %s -> %dx%d %s\n",
         ctx->idx, filename, codec->name,
         ctx->codec_ctx->width, ctx->codec_ctx->height,
         av_get_pix_fmt_name(ctx->codec_ctx->pix_fmt),
         ctx->app->width, ctx->app->height, fmt_name(ctx->app->fmt));
  return 0;
}

void close_ffmpeg_source(struct st20p_tx_ctx* ctx) {
  if (!ctx->use_ffmpeg) return;
  if (ctx->av_frame)  av_frame_free(&ctx->av_frame);
  if (ctx->yuv_frame) {
    av_freep(&ctx->yuv_frame->data[0]);
    av_frame_free(&ctx->yuv_frame);
  }
  if (ctx->av_packet) av_packet_free(&ctx->av_packet);
  if (ctx->sws_ctx)   { sws_freeContext(ctx->sws_ctx); ctx->sws_ctx = NULL; }
  if (ctx->codec_ctx) avcodec_free_context(&ctx->codec_ctx);
  if (ctx->fmt_ctx)   avformat_close_input(&ctx->fmt_ctx);
}

/* =========================================================================
 * Video source loading
 * ========================================================================= */
int load_video_source(struct st20p_tx_ctx* ctx, const char* filename) {
  if (!filename || strlen(filename) == 0) {
    printf("ST20P TX(%d): No source file, will use test pattern\n", ctx->idx);
    return 0;
  }
  if (is_raw_yuv(filename)) {
    FILE* f = fopen(filename, "rb");
    if (!f) { printf("ST20P TX(%d): Cannot open %s\n", ctx->idx, filename); return 0; }
    fseek(f, 0, SEEK_END); ctx->source_size = ftell(f); fseek(f, 0, SEEK_SET);
    if (!ctx->source_size) { fclose(f); return 0; }
    ctx->source_buffer = malloc(ctx->source_size);
    if (!ctx->source_buffer) { fclose(f); return -1; }
    ctx->source_size = fread(ctx->source_buffer, 1, ctx->source_size, f);
    fclose(f);
    ctx->current_pos   = 0;
    ctx->loop_playback = true;
    ctx->use_ffmpeg    = false;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(ctx->app->fmt);
    ctx->frame_size = (size_t)ctx->app->width * ctx->app->height
                      * (desc ? desc->comp[0].depth / 8 : 2) * 2 /* yuv422 */;
    printf("ST20P TX(%d): Loaded %zu bytes from RAW YUV: %s\n",
           ctx->idx, ctx->source_size, filename);
    return 0;
  }
  return open_ffmpeg_source(ctx, filename);
}

/* =========================================================================
 * Per-session decode-and-send (single-session FFmpeg path)
 * ========================================================================= */
/*
 * ffmpeg_decode_and_send() — called from st20p_tx_thread() for Path A.
 *
 * Reads compressed packets until one full decoded frame is produced, then
 * colour-converts and sends it via send_video_frame().  Handles H.264
 * B-frame reorder (EAGAIN) and EOF loop-restart internally.
 *
 * Returns true when a frame was successfully sent, false on error/exit.
 */
bool ffmpeg_decode_and_send(struct st20p_tx_ctx* ctx) {
  int crop_x = ctx->crop_x_offset;
  int crop_y = ctx->crop_y_offset;
  int crop_w = (ctx->crop_width > 0) ? ctx->crop_width : ctx->app->width;
  int crop_h = (ctx->crop_height > 0) ? ctx->crop_height : ctx->app->height;
  bool got_frame = false;

  while (!got_frame && !ctx->app->exit && !g_tx_app_exit) {
    int ret = av_read_frame(ctx->fmt_ctx, ctx->av_packet);
    if (ret == AVERROR_EOF) {
      av_seek_frame(ctx->fmt_ctx, ctx->video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(ctx->codec_ctx);
      printf("ST20P TX(%d): FFmpeg loop restart\n", ctx->idx);
      continue;
    }
    if (ret < 0) continue;
    if (ctx->av_packet->stream_index != ctx->video_stream_idx) {
      av_packet_unref(ctx->av_packet);
      continue;
    }
    avcodec_send_packet(ctx->codec_ctx, ctx->av_packet);
    av_packet_unref(ctx->av_packet);

    ret = avcodec_receive_frame(ctx->codec_ctx, ctx->av_frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) continue;
    if (ret < 0) break;

    sws_scale(ctx->sws_ctx,
              (const uint8_t* const*)ctx->av_frame->data,
              ctx->av_frame->linesize, 0, ctx->codec_ctx->height,
              ctx->yuv_frame->data, ctx->yuv_frame->linesize);

    av_frame_unref(ctx->av_frame);
    got_frame = true;
  }

  if (got_frame)
    send_video_frame(ctx, ctx->yuv_frame, crop_x, crop_y, crop_w, crop_h);

  return got_frame;
}
