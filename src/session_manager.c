/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#include "session_manager.h"
#include "tx_app_context.h"
#include "config_reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

static bool g_tx_app_exit = false;

/* =========================================================================
 * Helpers
 * ========================================================================= */

static const char* fmt_name(enum AVPixelFormat fmt) {
  const char* n = av_get_pix_fmt_name(fmt);
  return n ? n : "unknown";
}

/* Used by open_audio_output() to build the RTP URL for the audio TX stream */
static void build_output_url(char* buf, size_t bufsz,
                              const char* dip, uint16_t udp_port) {
  snprintf(buf, bufsz, "rtp://%s:%d", dip, (int)udp_port);
}

static bool is_raw_yuv(const char* filename) {
  const char* ext = strrchr(filename, '.');
  if (!ext) return false;
  return (strcasecmp(ext, ".yuv") == 0 || strcasecmp(ext, ".raw") == 0);
}

/* =========================================================================
 * FFmpeg output session: open MTL Kahawai (mtl_st20p) output device
 * No encoder; codec params are set directly on the stream.
 * ========================================================================= */
static int open_ffmpeg_output(struct st20p_tx_ctx* ctx) {
  int out_w  = ctx->crop_width > 0 ? ctx->crop_width : ctx->app->width;
  int height = ctx->app->height;

  /* Locate the MTL Kahawai muxer compiled into this FFmpeg build */
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

  /* Set MTL device and session options via AVOptions */
  av_opt_set    (ctx->out_fmt_ctx->priv_data, "p_port",  ctx->app->port,         0);
  av_opt_set    (ctx->out_fmt_ctx->priv_data, "p_sip",   ctx->app->sip_addr_str, 0);
  av_opt_set    (ctx->out_fmt_ctx->priv_data, "p_tx_ip", ctx->app->dip_addr_str, 0);
  av_opt_set_int(ctx->out_fmt_ctx->priv_data, "udp_port",
                 (int64_t)(ctx->app->udp_port + ctx->idx * 2), 0);
  av_opt_set_int(ctx->out_fmt_ctx->priv_data, "payload_type",
                 (int64_t)ctx->app->payload_type, 0);

  /* Raw-video stream — no encoder needed, codec params set directly */
  ctx->out_stream = avformat_new_stream(ctx->out_fmt_ctx, NULL);
  if (!ctx->out_stream) {
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

  /* write_header initialises the MTL session internally */
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

  /* Pre-allocate the output packet with the exact packed frame size.
   * av_image_get_buffer_size uses align=1 so no stride padding.
   * The MTL muxer validates pkt->size == its internal frame_size. */
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
         (unsigned)(ctx->app->udp_port + ctx->idx * 2),
         ctx->app->port);
  return 0;
}

static void close_ffmpeg_output(struct st20p_tx_ctx* ctx) {
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

/* Send one decoded+cropped frame to the MTL Kahawai (mtl_st20p) muxer.
 * enc_frame is a crop-width scratch buffer; enc_pkt holds the pre-allocated
 * contiguous (stride-less) frame payload that MTL copies into its TX ring. */
static void send_video_frame(struct st20p_tx_ctx* ctx, AVFrame* src,
                              int crop_x, int crop_w) {
  if (!ctx->out_fmt_ctx || !ctx->enc_frame || !ctx->enc_pkt) return;

  av_frame_make_writable(ctx->enc_frame);

  int height = ctx->app->height;

  /* Copy the cropped horizontal strip from the full-width source frame */
  for (int line = 0; line < height; line++)
    memcpy(ctx->enc_frame->data[0] + line * ctx->enc_frame->linesize[0],
           src->data[0] + line * src->linesize[0] + crop_x * 2,
           crop_w * 2);
  for (int line = 0; line < height; line++)
    memcpy(ctx->enc_frame->data[1] + line * ctx->enc_frame->linesize[1],
           src->data[1] + line * src->linesize[1] + (crop_x / 2) * 2,
           (crop_w / 2) * 2);
  for (int line = 0; line < height; line++)
    memcpy(ctx->enc_frame->data[2] + line * ctx->enc_frame->linesize[2],
           src->data[2] + line * src->linesize[2] + (crop_x / 2) * 2,
           (crop_w / 2) * 2);

  /* Pack all planes contiguously (no stride padding) into the MTL packet */
  int packed = av_image_copy_to_buffer(
      ctx->enc_pkt->data, ctx->enc_pkt->size,
      (const uint8_t* const*)ctx->enc_frame->data, ctx->enc_frame->linesize,
      ctx->app->fmt, crop_w, height, 1);
  if (packed < 0) return;

  ctx->enc_pkt->size         = packed;
  ctx->enc_pkt->pts          = ctx->pts;
  ctx->enc_pkt->dts          = ctx->pts++;
  ctx->enc_pkt->pos          = -1;
  ctx->enc_pkt->stream_index = ctx->out_stream->index;
  av_write_frame(ctx->out_fmt_ctx, ctx->enc_pkt);
  /* Restore size for next frame (packed == pre-allocated frame_sz) */
  ctx->enc_pkt->size = packed;

  ctx->frames_sent++;
  if (ctx->frames_sent % 100 == 0)
    printf("ST20P TX(%d): sent %d frames via Kahawai\n", ctx->idx, ctx->frames_sent);
}

/* =========================================================================
 * Shared decode thread
 * ========================================================================= */
static void* shared_decode_thread(void* arg) {
  struct shared_decode_ctx* dec = (struct shared_decode_ctx*)arg;

  printf("Shared decode thread started (%d sessions)\n", dec->num_sessions);

  while (!dec->exit && !g_tx_app_exit) {
    bool got_frame = false;
    while (!got_frame && !dec->exit && !g_tx_app_exit) {
      int ret = av_read_frame(dec->fmt_ctx, dec->av_packet);
      if (ret == AVERROR_EOF) {
        av_seek_frame(dec->fmt_ctx, dec->video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec->codec_ctx);
        printf("Shared decode: loop restart\n");
        continue;
      }
      if (ret < 0) continue;

      if (dec->av_packet->stream_index != dec->video_stream_idx) {
        av_packet_unref(dec->av_packet);
        continue;
      }

      avcodec_send_packet(dec->codec_ctx, dec->av_packet);
      av_packet_unref(dec->av_packet);

      ret = avcodec_receive_frame(dec->codec_ctx, dec->av_frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) continue;
      if (ret < 0) break;

      sws_scale(dec->sws_ctx,
                (const uint8_t* const*)dec->av_frame->data,
                dec->av_frame->linesize, 0, dec->codec_ctx->height,
                dec->yuv_frame->data, dec->yuv_frame->linesize);

      av_frame_unref(dec->av_frame);
      dec->frame_counter++;
      got_frame = true;
    }

    if (!got_frame) break;

    pthread_barrier_wait(&dec->barrier_decoded);
    pthread_barrier_wait(&dec->barrier_copied);
  }

  dec->exit = true;
  pthread_barrier_wait(&dec->barrier_decoded);
  pthread_barrier_wait(&dec->barrier_copied);

  printf("Shared decode thread stopped, decoded %u frames\n", dec->frame_counter);
  return NULL;
}

/* =========================================================================
 * Video TX thread — SHARED path (multi-session)
 * ========================================================================= */
static void* st20p_tx_thread_shared(void* arg) {
  struct st20p_tx_ctx* ctx = (struct st20p_tx_ctx*)arg;
  struct shared_decode_ctx* dec = ctx->shared_dec;
  int crop_w = ctx->crop_width;
  int crop_x = ctx->crop_x_offset;

  printf("ST20P TX(%d): shared thread started (crop x=%d w=%d)\n",
         ctx->idx, crop_x, crop_w);

  while (!ctx->app->exit && !g_tx_app_exit) {
    pthread_barrier_wait(&dec->barrier_decoded);

    if (dec->exit || ctx->app->exit || g_tx_app_exit) {
      pthread_barrier_wait(&dec->barrier_copied);
      break;
    }

    send_video_frame(ctx, dec->yuv_frame, crop_x, crop_w);

    pthread_barrier_wait(&dec->barrier_copied);
  }

  printf("ST20P TX(%d): thread stopped, sent %d frames\n", ctx->idx, ctx->frames_sent);
  return NULL;
}

/* =========================================================================
 * Video TX thread — SINGLE SESSION path (per-session decode or raw YUV)
 * ========================================================================= */
static void* st20p_tx_thread(void* arg) {
  struct st20p_tx_ctx* ctx = (struct st20p_tx_ctx*)arg;
  int crop_w = (ctx->crop_width > 0) ? ctx->crop_width : ctx->app->width;
  int crop_x = ctx->crop_x_offset;

  printf("ST20P TX(%d): thread started\n", ctx->idx);

  while (!ctx->app->exit && !g_tx_app_exit) {
    if (ctx->use_ffmpeg) {
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
        send_video_frame(ctx, ctx->yuv_frame, crop_x, crop_w);

    } else if (ctx->source_buffer && ctx->source_size > 0) {
      /* Raw YUV path: source file holds tightly-packed planar frames */
      if (!ctx->enc_frame || !ctx->enc_pkt) { usleep(1000); continue; }

      av_frame_make_writable(ctx->enc_frame);

      /* frame_size tracks the bytes-per-frame used when the file was loaded */
      size_t frame_bytes = ctx->frame_size;
      if (ctx->current_pos + frame_bytes > ctx->source_size)
        ctx->current_pos = 0;

      size_t copy_sz = (ctx->current_pos + frame_bytes <= ctx->source_size)
                         ? frame_bytes : (ctx->source_size - ctx->current_pos);
      /* Raw YUV files are stored plane-contiguous; data[0] is the base of
       * the contiguous allocation so copying here fills Y (+ U,V overlap). */
      memcpy(ctx->enc_frame->data[0],
             ctx->source_buffer + ctx->current_pos, copy_sz);
      ctx->current_pos += copy_sz;

      /* Pack the frame into the MTL packet and send via Kahawai */
      int packed = av_image_copy_to_buffer(
          ctx->enc_pkt->data, ctx->enc_pkt->size,
          (const uint8_t* const*)ctx->enc_frame->data, ctx->enc_frame->linesize,
          ctx->app->fmt, ctx->enc_frame->width, ctx->enc_frame->height, 1);
      if (packed > 0) {
        ctx->enc_pkt->size         = packed;
        ctx->enc_pkt->pts          = ctx->pts;
        ctx->enc_pkt->dts          = ctx->pts++;
        ctx->enc_pkt->pos          = -1;
        ctx->enc_pkt->stream_index = ctx->out_stream->index;
        av_write_frame(ctx->out_fmt_ctx, ctx->enc_pkt);
        ctx->enc_pkt->size = packed;
      }

      ctx->frames_sent++;
      if (ctx->frames_sent % 100 == 0)
        printf("ST20P TX(%d): sent %d frames via Kahawai\n", ctx->idx, ctx->frames_sent);

    } else {
      /* Test pattern (10-bit YUV422 ramp + neutral chroma) */
      if (!ctx->enc_frame || !ctx->enc_pkt) { usleep(1000); continue; }

      av_frame_make_writable(ctx->enc_frame);
      uint32_t pattern = ctx->frames_sent % 256;
      int fw = ctx->enc_frame->width;
      int fh = ctx->enc_frame->height;
      uint16_t* y = (uint16_t*)ctx->enc_frame->data[0];
      for (int i = 0; i < fw * fh; i++)
        y[i] = ((i + pattern) % 1024) << 6;
      if (ctx->enc_frame->data[1] && ctx->enc_frame->data[2]) {
        uint16_t* u = (uint16_t*)ctx->enc_frame->data[1];
        uint16_t* v = (uint16_t*)ctx->enc_frame->data[2];
        for (int i = 0; i < fw * fh / 2; i++) {
          u[i] = 512 << 6;
          v[i] = 512 << 6;
        }
      }

      /* Pack frame and send via Kahawai */
      int packed = av_image_copy_to_buffer(
          ctx->enc_pkt->data, ctx->enc_pkt->size,
          (const uint8_t* const*)ctx->enc_frame->data, ctx->enc_frame->linesize,
          ctx->app->fmt, fw, fh, 1);
      if (packed > 0) {
        ctx->enc_pkt->size         = packed;
        ctx->enc_pkt->pts          = ctx->pts;
        ctx->enc_pkt->dts          = ctx->pts++;
        ctx->enc_pkt->pos          = -1;
        ctx->enc_pkt->stream_index = ctx->out_stream->index;
        av_write_frame(ctx->out_fmt_ctx, ctx->enc_pkt);
        ctx->enc_pkt->size = packed;
      }

      ctx->frames_sent++;
      if (ctx->frames_sent % 100 == 0)
        printf("ST20P TX(%d): sent %d frames via Kahawai\n", ctx->idx, ctx->frames_sent);

      usleep(1000000 / ctx->app->fps);
    }
  }

  printf("ST20P TX(%d): thread stopped, sent %d frames\n", ctx->idx, ctx->frames_sent);
  return NULL;
}

/* =========================================================================
 * Audio TX thread (raw PCM over UDP/RTP)
 * ========================================================================= */
static void* st30p_tx_thread(void* arg) {
  struct st30p_tx_ctx* ctx = (struct st30p_tx_ctx*)arg;

  printf("ST30P TX(%d): thread started\n", ctx->idx);

  while (!ctx->app->exit && !g_tx_app_exit) {
    if (!ctx->out_fmt_ctx || !ctx->source_buffer || !ctx->source_size) {
      usleep(1000);
      continue;
    }

    size_t chunk = ctx->frame_size ? ctx->frame_size : 1920;

    if (ctx->current_pos + chunk > ctx->source_size)
      ctx->current_pos = 0;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) { usleep(1000); continue; }

    pkt->data = ctx->source_buffer + ctx->current_pos;
    pkt->size = (int)((ctx->current_pos + chunk <= ctx->source_size)
                       ? chunk : (ctx->source_size - ctx->current_pos));
    pkt->stream_index = 0;

    av_interleaved_write_frame(ctx->out_fmt_ctx, pkt);
    av_packet_free(&pkt);

    ctx->current_pos += chunk;
    ctx->frames_sent++;
    if (ctx->frames_sent % 1000 == 0)
      printf("ST30P TX(%d): sent %d chunks\n", ctx->idx, ctx->frames_sent);

    usleep(1000);
  }

  printf("ST30P TX(%d): thread stopped, sent %d chunks\n", ctx->idx, ctx->frames_sent);
  return NULL;
}

/* =========================================================================
 * Open shared FFmpeg decoder (multi-session input path)
 * ========================================================================= */
static int open_shared_ffmpeg(struct shared_decode_ctx* dec, const char* filename) {
  char errbuf[256];
  int ret;
  struct tx_app_context* app = dec->app;

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
  dec->codec_ctx->thread_count = 4;
  ret = avcodec_open2(dec->codec_ctx, codec, NULL);
  if (ret < 0) {
    avcodec_free_context(&dec->codec_ctx);
    avformat_close_input(&dec->fmt_ctx);
    return -1;
  }

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

/* =========================================================================
 * Open per-session FFmpeg input decoder (single-session path)
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

int load_audio_source(struct st30p_tx_ctx* ctx, const char* filename) {
  if (!filename || strlen(filename) == 0) return 0;
  FILE* f = fopen(filename, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END); ctx->source_size = ftell(f); fseek(f, 0, SEEK_SET);
  if (!ctx->source_size) { fclose(f); return 0; }
  ctx->source_buffer = malloc(ctx->source_size);
  if (!ctx->source_buffer) { fclose(f); return -1; }
  ctx->source_size = fread(ctx->source_buffer, 1, ctx->source_size, f);
  fclose(f);
  ctx->current_pos   = 0;
  ctx->loop_playback = true;
  ctx->frame_size    = 1920; /* 1 ms @ 48 kHz stereo 16-bit */
  printf("ST30P TX(%d): Loaded %zu bytes from %s\n", ctx->idx, ctx->source_size, filename);
  return 0;
}

/* =========================================================================
 * Open audio output (raw PCM over UDP/RTP)
 * ========================================================================= */
static int open_audio_output(struct st30p_tx_ctx* ctx) {
  char url[256];
  build_output_url(url, sizeof(url),
                   ctx->app->dip_addr_str,
                   (uint16_t)(ctx->app->udp_port + 1 + ctx->idx * 2));

  int ret = avformat_alloc_output_context2(&ctx->out_fmt_ctx, NULL, "rtp", url);
  if (ret < 0 || !ctx->out_fmt_ctx) {
    printf("ST30P TX(%d): cannot alloc audio output ctx\n", ctx->idx);
    return -1;
  }

  ctx->out_stream = avformat_new_stream(ctx->out_fmt_ctx, NULL);
  if (!ctx->out_stream) {
    avformat_free_context(ctx->out_fmt_ctx); ctx->out_fmt_ctx = NULL;
    return -1;
  }

  ctx->out_stream->codecpar->codec_type              = AVMEDIA_TYPE_AUDIO;
  ctx->out_stream->codecpar->codec_id                = AV_CODEC_ID_PCM_S24BE;
  ctx->out_stream->codecpar->sample_rate             = 48000;
  ctx->out_stream->codecpar->ch_layout.nb_channels   = 2;
  ctx->out_stream->time_base                         = (AVRational){1, 48000};

  ctx->enc_pkt = av_packet_alloc();

  ret = avio_open(&ctx->out_fmt_ctx->pb, url, AVIO_FLAG_WRITE);
  if (ret < 0) {
    av_packet_free(&ctx->enc_pkt);
    avformat_free_context(ctx->out_fmt_ctx); ctx->out_fmt_ctx = NULL;
    return -1;
  }

  ret = avformat_write_header(ctx->out_fmt_ctx, NULL);
  if (ret < 0)
    printf("ST30P TX(%d): avformat_write_header warning\n", ctx->idx);
  printf("ST30P TX(%d): audio output opened -> %s\n", ctx->idx, url);
  return 0;
}

/* =========================================================================
 * Session creation
 * ========================================================================= */
int create_st20p_tx_session(session_manager_t* manager, struct tx_app_context* app,
                             int session_idx) {
  struct st20p_tx_ctx* ctx = &manager->st20p_sessions[session_idx];

  memset(ctx, 0, sizeof(*ctx));
  ctx->idx = session_idx;
  ctx->app = app;

  ctx->crop_width    = app->width / app->st20p_sessions;
  ctx->crop_x_offset = session_idx * ctx->crop_width;
  printf("ST20P TX session %d: crop strip x=%d w=%d (%d / %d sessions)\n",
         session_idx, ctx->crop_x_offset, ctx->crop_width,
         app->width, app->st20p_sessions);

  snprintf(ctx->session_name, sizeof(ctx->session_name), "st20p_tx_%d", session_idx);

  if (open_ffmpeg_output(ctx) < 0) {
    printf("Error: Failed to open FFmpeg output for ST20P session %d\n", session_idx);
    return -1;
  }

  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(app->fmt);
  ctx->frame_size = (size_t)ctx->crop_width * app->height
                    * (desc ? desc->comp[0].depth / 8 : 2) * 2;

  if (manager->shared_dec) {
    ctx->shared_dec = manager->shared_dec;
    printf("ST20P TX session %d: using shared decoder\n", session_idx);
  } else if (strlen(app->tx_url) > 0) {
    load_video_source(ctx, app->tx_url);
  }

  return 0;
}

int create_st30p_tx_session(session_manager_t* manager, struct tx_app_context* app,
                             int session_idx) {
  struct st30p_tx_ctx* ctx = &manager->st30p_sessions[session_idx];

  memset(ctx, 0, sizeof(*ctx));
  ctx->idx = session_idx;
  ctx->app = app;

  snprintf(ctx->session_name, sizeof(ctx->session_name), "st30p_tx_%d", session_idx);

  if (open_audio_output(ctx) < 0) {
    printf("Error: Failed to open audio output for ST30P session %d\n", session_idx);
    return -1;
  }

  ctx->frame_size = 1920;
  printf("ST30P TX session %d created\n", session_idx);
  return 0;
}

/* =========================================================================
 * session_manager_init / start / stop / cleanup
 * ========================================================================= */
int session_manager_init(session_manager_t* manager, struct tx_app_context* app) {
  memset(manager, 0, sizeof(*manager));

  bool use_shared = (app->st20p_sessions > 1 &&
                     strlen(app->tx_url) > 0 &&
                     !is_raw_yuv(app->tx_url));

  if (use_shared) {
    manager->shared_dec = calloc(1, sizeof(struct shared_decode_ctx));
    if (!manager->shared_dec) return -1;

    manager->shared_dec->app          = app;
    manager->shared_dec->num_sessions = app->st20p_sessions;
    manager->shared_dec->exit         = false;

    pthread_barrier_init(&manager->shared_dec->barrier_decoded,
                         NULL, app->st20p_sessions + 1);
    pthread_barrier_init(&manager->shared_dec->barrier_copied,
                         NULL, app->st20p_sessions + 1);

    if (open_shared_ffmpeg(manager->shared_dec, app->tx_url) < 0) {
      printf("Error: Failed to open shared FFmpeg source\n");
      free(manager->shared_dec);
      manager->shared_dec = NULL;
      return -1;
    }
    printf("Shared decoder ready for %d sessions\n", app->st20p_sessions);
  }

  if (app->st20p_sessions > 0) {
    manager->st20p_sessions = calloc(app->st20p_sessions, sizeof(struct st20p_tx_ctx));
    if (!manager->st20p_sessions) { session_manager_cleanup(manager); return -1; }
    manager->st20p_count = app->st20p_sessions;

    for (int i = 0; i < app->st20p_sessions; i++) {
      if (create_st20p_tx_session(manager, app, i) < 0) {
        session_manager_cleanup(manager);
        return -1;
      }
    }
  }

  if (app->st30p_sessions > 0) {
    manager->st30p_sessions = calloc(app->st30p_sessions, sizeof(struct st30p_tx_ctx));
    if (!manager->st30p_sessions) { session_manager_cleanup(manager); return -1; }
    manager->st30p_count = app->st30p_sessions;

    for (int i = 0; i < app->st30p_sessions; i++) {
      if (create_st30p_tx_session(manager, app, i) < 0) {
        session_manager_cleanup(manager);
        return -1;
      }
    }
  }

  printf("TX Session Manager: %d video, %d audio sessions, shared_dec=%s\n",
         manager->st20p_count, manager->st30p_count,
         manager->shared_dec ? "YES" : "NO");
  return 0;
}

int session_manager_start(session_manager_t* manager) {
  g_tx_app_exit = false;

  if (manager->shared_dec) {
    if (pthread_create(&manager->shared_dec->decode_thread, NULL,
                       shared_decode_thread, manager->shared_dec) != 0) {
      printf("Error: Failed to create shared decode thread\n");
      return -1;
    }
  }

  for (int i = 0; i < manager->st20p_count; i++) {
    struct st20p_tx_ctx* ctx = &manager->st20p_sessions[i];
    void* thread_fn = ctx->shared_dec ? st20p_tx_thread_shared : st20p_tx_thread;

    if (pthread_create(&ctx->thread, NULL, thread_fn, ctx) != 0) {
      printf("Error: Failed to create ST20P TX thread %d\n", i);
      return -1;
    }
  }

  for (int i = 0; i < manager->st30p_count; i++) {
    struct st30p_tx_ctx* ctx = &manager->st30p_sessions[i];
    if (pthread_create(&ctx->thread, NULL, st30p_tx_thread, ctx) != 0) {
      printf("Error: Failed to create ST30P TX thread %d\n", i);
      return -1;
    }
  }

  manager->running = true;
  printf("TX Session Manager started\n");
  return 0;
}

int session_manager_stop(session_manager_t* manager) {
  g_tx_app_exit = true;
  manager->running = false;

  if (manager->shared_dec)
    manager->shared_dec->exit = true;

  if (manager->shared_dec && manager->shared_dec->decode_thread) {
    pthread_join(manager->shared_dec->decode_thread, NULL);
    manager->shared_dec->decode_thread = 0;
  }

  for (int i = 0; i < manager->st20p_count; i++) {
    struct st20p_tx_ctx* ctx = &manager->st20p_sessions[i];
    if (ctx->thread) { pthread_join(ctx->thread, NULL); ctx->thread = 0; }
  }

  for (int i = 0; i < manager->st30p_count; i++) {
    struct st30p_tx_ctx* ctx = &manager->st30p_sessions[i];
    if (ctx->thread) { pthread_join(ctx->thread, NULL); ctx->thread = 0; }
  }

  printf("TX Session Manager stopped\n");
  return 0;
}

void session_manager_cleanup(session_manager_t* manager) {
  if (manager->running) session_manager_stop(manager);

  if (manager->st20p_sessions) {
    for (int i = 0; i < manager->st20p_count; i++) {
      struct st20p_tx_ctx* ctx = &manager->st20p_sessions[i];

      close_ffmpeg_output(ctx);

      if (ctx->use_ffmpeg) {
        if (ctx->av_frame)  av_frame_free(&ctx->av_frame);
        if (ctx->yuv_frame) { av_freep(&ctx->yuv_frame->data[0]); av_frame_free(&ctx->yuv_frame); }
        if (ctx->av_packet) av_packet_free(&ctx->av_packet);
        if (ctx->sws_ctx)   { sws_freeContext(ctx->sws_ctx); ctx->sws_ctx = NULL; }
        if (ctx->codec_ctx) avcodec_free_context(&ctx->codec_ctx);
        if (ctx->fmt_ctx)   avformat_close_input(&ctx->fmt_ctx);
      }
      if (ctx->source_buffer) { free(ctx->source_buffer); ctx->source_buffer = NULL; }
    }
    free(manager->st20p_sessions);
    manager->st20p_sessions = NULL;
  }

  if (manager->shared_dec) {
    struct shared_decode_ctx* dec = manager->shared_dec;
    if (dec->av_frame)  av_frame_free(&dec->av_frame);
    if (dec->yuv_frame) { av_freep(&dec->yuv_frame->data[0]); av_frame_free(&dec->yuv_frame); }
    if (dec->av_packet) av_packet_free(&dec->av_packet);
    if (dec->sws_ctx)   { sws_freeContext(dec->sws_ctx); dec->sws_ctx = NULL; }
    if (dec->codec_ctx) avcodec_free_context(&dec->codec_ctx);
    if (dec->fmt_ctx)   avformat_close_input(&dec->fmt_ctx);
    pthread_barrier_destroy(&dec->barrier_decoded);
    pthread_barrier_destroy(&dec->barrier_copied);
    free(dec);
    manager->shared_dec = NULL;
  }

  if (manager->st30p_sessions) {
    for (int i = 0; i < manager->st30p_count; i++) {
      struct st30p_tx_ctx* ctx = &manager->st30p_sessions[i];
      if (ctx->out_fmt_ctx) {
        av_write_trailer(ctx->out_fmt_ctx);
        avio_closep(&ctx->out_fmt_ctx->pb);
        avformat_free_context(ctx->out_fmt_ctx);
        ctx->out_fmt_ctx = NULL;
      }
      if (ctx->enc_pkt)       av_packet_free(&ctx->enc_pkt);
      if (ctx->source_buffer) { free(ctx->source_buffer); ctx->source_buffer = NULL; }
    }
    free(manager->st30p_sessions);
    manager->st30p_sessions = NULL;
  }

  manager->st20p_count = 0;
  manager->st30p_count = 0;
}
