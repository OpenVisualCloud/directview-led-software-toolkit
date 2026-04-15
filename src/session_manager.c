/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#include "session_manager.h"
#include "ffmpeg_decoder.h"
#include "tx_app_context.h"
#include "config_reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

/* Global exit flag — set by session_manager_stop() to signal all threads to terminate.
 * _Atomic ensures reads/writes are race-free across TX threads.
 * Also referenced by ffmpeg_decoder.c and tx_app_main.c via extern. */
_Atomic bool g_tx_app_exit = false;

/* =========================================================================
 * Helpers
 * ========================================================================= */

/* Used by open_audio_output() to build the RTP URL for the audio TX stream */
static void build_output_url(char* buf, size_t bufsz,
                              const char* dip, uint16_t udp_port) {
  snprintf(buf, bufsz, "rtp://%s:%d", dip, (int)udp_port);
}

/* =========================================================================
 * Video TX thread — SHARED path (multi-session)
 * ========================================================================= */
/*
 * st20p_tx_thread_shared() — one instance per session (TX0, TX1, TX2).
 *
 * Used when sessions > 1 AND source is a video file (shared decode path).
 * Each thread is responsible for a fixed horizontal crop strip of yuv_frame:
 *   TX0: x=0,    w=640  -> UDP port 20000
 *   TX1: x=640,  w=640  -> UDP port 20002
 *   TX2: x=1280, w=640  -> UDP port 20004
 *
 * Per-frame flow:
 *   1. barrier_decoded.wait -- stall until decode thread signals yuv_frame is ready.
 *   2. send_video_frame()   -- crop strip, pack, av_write_frame -> MTL TX ring.
 *   3. barrier_copied.wait  -- signal decode thread that this session is done.
 */
static void* st20p_tx_thread_shared(void* arg) {
  struct st20p_tx_ctx* ctx = (struct st20p_tx_ctx*)arg;
  struct shared_decode_ctx* dec = ctx->shared_dec;
  int crop_x = ctx->crop_x_offset;
  int crop_y = ctx->crop_y_offset;
  int crop_w = ctx->crop_width  > 0 ? ctx->crop_width  : (int)ctx->app->width;
  int crop_h = ctx->crop_height > 0 ? ctx->crop_height : (int)ctx->app->height;

  printf("ST20P TX(%d): shared thread started (crop x=%d y=%d w=%d h=%d)\n",
         ctx->idx, crop_x, crop_y, crop_w, crop_h);

  while (1) {
    /* Always enter barrier_decoded unconditionally — even during shutdown.
     * The decode thread's cleanup path always posts barrier_decoded to release
     * TX threads; if a TX thread skips this barrier the decode thread deadlocks
     * forever in pthread_barrier_wait. */
    pthread_barrier_wait(&dec->barrier_decoded);

    bool should_exit = (dec->exit || ctx->app->exit || g_tx_app_exit);
    if (!should_exit)
      /* Crop this session's rect from yuv_frame, pack it, send via mtl_st20p */
      send_video_frame(ctx, dec->yuv_frame, crop_x, crop_y, crop_w, crop_h);

    /* Always enter barrier_copied too, then break if exiting */
    pthread_barrier_wait(&dec->barrier_copied);
    if (should_exit) break;
  }

  printf("ST20P TX(%d): thread stopped, sent %d frames\n", ctx->idx, ctx->frames_sent);
  return NULL;
}

/* =========================================================================
 * Video TX thread — SINGLE SESSION path (per-session decode or raw YUV)
 * ========================================================================= */
/*
 * st20p_tx_thread() — single-session path (sessions == 1 or raw YUV input).
 *
 * Three sub-paths based on source type:
 *   A. use_ffmpeg == true  : delegates to ffmpeg_decode_and_send() in ffmpeg_decoder.c.
 *   B. source_buffer != NULL: raw YUV pre-loaded into memory, reads frame_size bytes.
 *   C. neither A nor B     : synthetic YUV ramp test pattern.
 */
static void* st20p_tx_thread(void* arg) {
  struct st20p_tx_ctx* ctx = (struct st20p_tx_ctx*)arg;

  printf("ST20P TX(%d): thread started\n", ctx->idx);

  while (!ctx->app->exit && !g_tx_app_exit) {
    if (ctx->use_ffmpeg) {
      /* Path A: delegate to ffmpeg_decoder.c — demux -> decode -> sws_scale -> send */
      ffmpeg_decode_and_send(ctx);

    } else if (ctx->source_buffer && ctx->source_size > 0) {
      /* Path B: raw YUV — source file pre-loaded into source_buffer at init.
       * Read frame_size bytes sequentially, wrap around at end (loop playback). */
      if (!ctx->enc_frame || !ctx->enc_pkt) { usleep(1000); continue; }

      av_frame_make_writable(ctx->enc_frame);

      size_t frame_bytes = ctx->frame_size;
      if (ctx->current_pos + frame_bytes > ctx->source_size)
        ctx->current_pos = 0;

      size_t copy_sz = (ctx->current_pos + frame_bytes <= ctx->source_size)
                         ? frame_bytes : (ctx->source_size - ctx->current_pos);

      /* Raw YUV files are packed (no per-line stride padding). Use
       * av_image_fill_arrays with align=1 to map the packed source buffer
       * onto the plane pointers/linesizes that av_image_copy_to_buffer expects.
       * This correctly handles all formats and avoids writing a packed blob
       * into enc_frame->data[0] which has padded (aligned) linesizes. */
      uint8_t* planes[AV_NUM_DATA_POINTERS];
      int      linesizes[AV_NUM_DATA_POINTERS];
      av_image_fill_arrays(planes, linesizes,
                           ctx->source_buffer + ctx->current_pos,
                           ctx->app->fmt,
                           ctx->enc_frame->width, ctx->enc_frame->height, 1);
      av_image_copy(ctx->enc_frame->data, ctx->enc_frame->linesize,
                    (const uint8_t**)planes, linesizes,
                    ctx->app->fmt, ctx->enc_frame->width, ctx->enc_frame->height);
      ctx->current_pos += copy_sz;

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
      /* Path C: synthetic test pattern — incrementing luma ramp + neutral chroma.
       * Rate-limited by usleep(1e6/fps). */
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

    size_t chunk = ctx->frame_size ? ctx->frame_size : 288;

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
 * Audio source loading
 * ========================================================================= */
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
  ctx->frame_size = 288; /* 1 ms @ 48 kHz stereo 24-bit (PCM_S24BE): 48 * 2 * 3 = 288 bytes */
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
/*
 * create_st20p_tx_session() — initialise one ST20P video TX session.
 *
 * Calculates the horizontal crop strip for this session, opens the MTL
 * output (avformat_write_header -> MTL TX session created), then
 * attaches either the shared decoder or loads a per-session video source.
 */
int create_st20p_tx_session(session_manager_t* manager, struct tx_app_context* app,
                             int session_idx) {
  struct st20p_tx_ctx* ctx = &manager->st20p_sessions[session_idx];

  memset(ctx, 0, sizeof(*ctx));
  ctx->idx = session_idx;
  ctx->app = app;

  ctx->crop_x_offset = app->session_net[session_idx].crop_x;
  ctx->crop_y_offset = app->session_net[session_idx].crop_y;
  ctx->crop_width    = app->session_net[session_idx].crop_w;
  ctx->crop_height   = app->session_net[session_idx].crop_h;

  /* Fallback for CLI-only runs where session_net[] is zero-initialised.
   * Divide the frame width evenly across all sessions; height is always full. */
  if (ctx->crop_width == 0 || ctx->crop_height == 0) {
    int total = app->st20p_sessions > 0 ? app->st20p_sessions : 1;
    int strip_w = (int)app->width / total;
    ctx->crop_x_offset = session_idx * strip_w;
    ctx->crop_y_offset = 0;
    /* Last session gets any remaining pixels so strips cover the full width */
    ctx->crop_width  = (session_idx == total - 1)
                       ? ((int)app->width - ctx->crop_x_offset)
                       : strip_w;
    ctx->crop_height = (int)app->height;
  }
  printf("ST20P TX session %d: crop rect x=%d y=%d w=%d h=%d\n",
         session_idx, ctx->crop_x_offset, ctx->crop_y_offset,
         ctx->crop_width, ctx->crop_height);

  snprintf(ctx->session_name, sizeof(ctx->session_name), "st20p_tx_%d", session_idx);

  if (open_ffmpeg_output(ctx) < 0) {
    printf("Error: Failed to open FFmpeg output for ST20P session %d\n", session_idx);
    return -1;
  }

  /* Use av_image_get_buffer_size (align=1) for the exact packed frame size.
   * This correctly handles all formats (4:2:0, 4:2:2, 4:4:4, 10/12-bit) without
   * relying on the broken depth/8 integer division or hardcoded * 2 chroma factor. */
  int fsize = av_image_get_buffer_size(app->fmt, ctx->crop_width, ctx->crop_height, 1);
  ctx->frame_size = (fsize > 0) ? (size_t)fsize : 0;

  if (manager->shared_dec) {
    /* Multi-session path: attach shared decoder -- TX thread will use barriers */
    ctx->shared_dec = manager->shared_dec;
    printf("ST20P TX session %d: using shared decoder\n", session_idx);
  } else if (strlen(app->tx_url) > 0) {
    /* Single-session path: each session owns its own decoder or raw buffer */
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

  ctx->frame_size = 288; /* 1 ms @ 48 kHz stereo 24-bit (PCM_S24BE): 48 * 2 * 3 = 288 bytes */
  printf("ST30P TX session %d created\n", session_idx);
  return 0;
}

/* =========================================================================
 * session_manager_init / start / stop / cleanup
 * ========================================================================= */
/*
 * session_manager_init() — allocate and configure all TX sessions.
 *
 * Decision: use_shared = true when sessions > 1 and source is a video file.
 *   -> one shared decoder + N TX threads sharing one yuv_frame via barriers.
 *   -> avoids N decoders running in parallel (saves CPU and memory bandwidth).
 *
 * If use_shared = false (single session or raw YUV):
 *   -> each session runs its own decode loop independently (no barriers).
 */
int session_manager_init(session_manager_t* manager, struct tx_app_context* app) {
  memset(manager, 0, sizeof(*manager));

  /* Use shared decoder only when > 1 session and source needs decoding */
  bool use_shared = (app->st20p_sessions > 1 &&
                     strlen(app->tx_url) > 0 &&
                     !is_raw_yuv(app->tx_url));

  if (use_shared) {
    manager->shared_dec = calloc(1, sizeof(struct shared_decode_ctx));
    if (!manager->shared_dec) return -1;

    manager->shared_dec->app          = app;
    manager->shared_dec->num_sessions = app->st20p_sessions;
    manager->shared_dec->exit         = false;

    /* barrier count = N TX threads + 1 decode thread.
     * Both barriers must be released by ALL threads simultaneously. */
    pthread_barrier_init(&manager->shared_dec->barrier_decoded,
                         NULL, app->st20p_sessions + 1);
    pthread_barrier_init(&manager->shared_dec->barrier_copied,
                         NULL, app->st20p_sessions + 1);

    if (open_shared_ffmpeg(manager->shared_dec, app->tx_url) < 0) {
      printf("Error: Failed to open shared FFmpeg source\n");
      pthread_barrier_destroy(&manager->shared_dec->barrier_decoded);
      pthread_barrier_destroy(&manager->shared_dec->barrier_copied);
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

/*
 * session_manager_start() — spawn all worker threads.
 *
 * Thread launch order matters for barrier correctness:
 *   1. shared_decode_thread first -- it will block on barrier_decoded waiting
 *      for all TX threads to arrive.
 *   2. st20p_tx_thread_shared xN -- each blocks on barrier_decoded too.
 *   All N+1 threads must be running before any frame can be processed.
 */
int session_manager_start(session_manager_t* manager) {
  g_tx_app_exit = false;

  if (manager->shared_dec) {
    /* Launch the single shared decode thread (runs shared_decode_thread()) */
    if (pthread_create(&manager->shared_dec->decode_thread, NULL,
                       shared_decode_thread, manager->shared_dec) != 0) {
      printf("Error: Failed to create shared decode thread\n");
      return -1;
    }
  }

  for (int i = 0; i < manager->st20p_count; i++) {
    struct st20p_tx_ctx* ctx = &manager->st20p_sessions[i];
    /* Select thread function: shared barrier path or independent single-session path */
    void *(*thread_fn)(void *) = ctx->shared_dec ? st20p_tx_thread_shared : st20p_tx_thread;

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
      close_ffmpeg_source(ctx);
      if (ctx->source_buffer) { free(ctx->source_buffer); ctx->source_buffer = NULL; }
    }
    free(manager->st20p_sessions);
    manager->st20p_sessions = NULL;
  }

  if (manager->shared_dec) {
    close_shared_ffmpeg(manager->shared_dec);
    pthread_barrier_destroy(&manager->shared_dec->barrier_decoded);
    pthread_barrier_destroy(&manager->shared_dec->barrier_copied);
    free(manager->shared_dec);
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

bool session_manager_is_running(const session_manager_t* manager) {
  return manager->running;
}
