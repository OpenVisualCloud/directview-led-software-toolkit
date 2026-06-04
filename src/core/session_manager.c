/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */
#include "core/session_manager.h"
#include "ffmpeg/ffmpeg_decoder.h"
#include "ffmpeg/ffmpeg_frame_handler.h"
#include "ffmpeg/ffmpeg_tx.h"
#include "mtl/mtl_tx.h"
#include "app_context.h"
#include "util/config_reader.h"
#include "util/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include <libavutil/imgutils.h>

#ifdef ENABLE_MTL_TX
#include "mtl_api.h"
#include "st_pipeline_api.h"
#endif /* ENABLE_MTL_TX */

/* Exit flag — set by session_manager_request_exit() / session_manager_stop()
 * to signal all threads to terminate.
 * _Atomic ensures reads/writes are race-free across TX threads.
 * Accessed only through accessor functions (Rule 28). */
static _Atomic bool g_dvledtx_exit = false;

void session_manager_request_exit(void) { g_dvledtx_exit = true; }
bool session_manager_should_exit(void)  { return g_dvledtx_exit; }
void session_manager_reset_exit(void)   { g_dvledtx_exit = false; }

/* =========================================================================
 * Video TX thread — SHARED path (multi-session)
 * ========================================================================= */
/*
 * st20p_tx_thread_shared() — one instance per session (TX0, TX1, TX2 …).
 *
 * Used when sessions > 1 AND source is a video file (shared decode path).
 * Each thread is responsible for a fixed crop rectangle of yuv_frame.
 *
 * Per-frame flow:
 *   1. barrier_decoded.wait — stall until decode thread signals yuv_frame is ready.
 *   2. Transmit this session's crop strip (via MTL or FFmpeg depending on build).
 *   3. barrier_copied.wait  — signal decode thread that this session is done.
 */
static void* st20p_tx_thread_shared(void* arg) {
  struct st20p_tx_ctx* ctx = (struct st20p_tx_ctx*)arg;
  struct shared_decode_ctx* dec = ctx->shared_dec;
  int crop_x = ctx->crop_x_offset;
  int crop_y = ctx->crop_y_offset;
  int crop_w = ctx->crop_width  > 0 ? ctx->crop_width  : (int)ctx->app->width;
  int crop_h = ctx->crop_height > 0 ? ctx->crop_height : (int)ctx->app->height;

  LOG_INFO("ST20P TX(%d): shared thread started (crop x=%d y=%d w=%d h=%d)",
           ctx->idx, crop_x, crop_y, crop_w, crop_h);

  /* Startup gate: wait until all threads are created before touching barriers */
  pthread_mutex_lock(&dec->start_mutex);
  while (dec->start_ready == false)
    pthread_cond_wait(&dec->start_cond, &dec->start_mutex);
  pthread_mutex_unlock(&dec->start_mutex);

  if (dec->exit == true) {
    LOG_INFO("ST20P TX(%d): startup aborted", ctx->idx);
    return NULL;
  }

  while (1) {
    pthread_barrier_wait(&dec->barrier_decoded);

    /* Check only dec->exit / app->exit — NOT g_dvledtx_exit directly.
     * g_dvledtx_exit causes the decode thread's while-loop to exit; the decode
     * thread then sets dec->exit = true and performs the final barrier-pair
     * sync. TX threads must participate in that final sync before breaking,
     * otherwise the decode thread deadlocks waiting at barrier_decoded. */
    bool should_exit = (dec->exit == true || ctx->app->exit == true);
    if (should_exit == false) {
#ifdef ENABLE_MTL_TX
      int ret = mtl_tx_send_yuv_frame(ctx, dec->yuv_frame,
                                      crop_x, crop_y, crop_w, crop_h);
      if (ret < 0)
        LOG_ERROR("ST20P TX(%d): mtl_tx_send_yuv_frame failed", ctx->idx);
#else
      int ret = ffmpeg_tx_send_yuv_frame(ctx, dec->yuv_frame,
                                         crop_x, crop_y, crop_w, crop_h);
      if (ret < 0)
        LOG_ERROR("ST20P TX(%d): ffmpeg_tx_send_yuv_frame failed", ctx->idx);
#endif
      dec->frame_counter++;
    }

    pthread_barrier_wait(&dec->barrier_copied);
    if (should_exit) break;
  }

  LOG_INFO("ST20P TX(%d): thread stopped, sent %d frames", ctx->idx, ctx->frames_sent);
  return NULL;
}

/* =========================================================================
 * tx_fetch_next_frame — produce one frame into ctx->yuv_frame
 * =========================================================================
 *
 * Encapsulates the Path A / Path B source decision so that the TX thread
 * body only operates on frames:
 *
 *   Path A (use_ffmpeg): decode the next H.264 frame via FFmpeg and
 *     colour-convert it into ctx->yuv_frame (full app resolution).
 *
 *   Path B (source_buffer): the raw YUV buffer is already in the target
 *     pixel format at crop dimensions.  Map the current position in the
 *     buffer directly into ctx->yuv_frame->data[] (zero-copy) and advance
 *     the position.  Wraps around automatically for loop playback.
 *
 * Returns a pointer to ctx->yuv_frame on success, NULL on error or exit.
 * ========================================================================= */
static AVFrame* tx_fetch_next_frame(struct st20p_tx_ctx* ctx) {
  if (ctx->use_ffmpeg == true) {
    /* Path A: FFmpeg decode → fills ctx->yuv_frame at full app resolution */
    if (ffmpeg_decode_next_frame(ctx) == false) return NULL;
    return ctx->yuv_frame;
  }

  if (ctx->source_buffer != NULL && ctx->source_size > 0) {
    /* Path B: raw YUV — point ctx->yuv_frame planes directly into the
     * pre-loaded buffer at the current read position (no copy). */
    if (ctx->yuv_frame == NULL) {
      LOG_ERROR("ST20P TX(%d): yuv_frame not allocated for raw YUV path", ctx->idx);
      return NULL;
    }
    int w = ctx->yuv_frame->width;
    int h = ctx->yuv_frame->height;
    int frame_bytes = av_image_get_buffer_size(ctx->app->fmt, w, h, 1);
    if (frame_bytes < 0) return NULL;

    if (ctx->current_pos + (size_t)frame_bytes > ctx->source_size)
      ctx->current_pos = 0;

    av_image_fill_arrays(ctx->yuv_frame->data, ctx->yuv_frame->linesize,
                         ctx->source_buffer + ctx->current_pos,
                         ctx->app->fmt, w, h, 1);
    ctx->current_pos += (size_t)frame_bytes;
    return ctx->yuv_frame;
  }

  LOG_ERROR("ST20P TX(%d): no valid source configured", ctx->idx);
  return NULL;
}

/* =========================================================================
 * Video TX thread — SINGLE SESSION path
 * ========================================================================= */
/*
 * st20p_tx_thread() — single-session path (sessions == 1 or raw YUV input).
 *
 * Sub-paths based on source type:
 *   A. use_ffmpeg == true   : FFmpeg demux + decode + sws_scale → transmit.
 *   B. source_buffer != NULL: raw YUV pre-loaded into memory → transmit.
 */
static void* st20p_tx_thread(void* arg) {
  struct st20p_tx_ctx* ctx = (struct st20p_tx_ctx*)arg;

  LOG_INFO("ST20P TX(%d): thread started", ctx->idx);

  /* Crop origin: Path A yuv_frame is full-res → use the session's x/y offset.
   * Path B yuv_frame is already crop-sized (strip) → no offset needed. */
  int crop_x = (ctx->use_ffmpeg == true) ? ctx->crop_x_offset : 0;
  int crop_y = (ctx->use_ffmpeg == true) ? ctx->crop_y_offset : 0;
  int crop_w = ctx->crop_width  > 0 ? ctx->crop_width  : (int)ctx->app->width;
  int crop_h = ctx->crop_height > 0 ? ctx->crop_height : (int)ctx->app->height;

  while (ctx->app->exit == false && session_manager_should_exit() == false) {
    const AVFrame* frame = tx_fetch_next_frame(ctx);
    if (frame == NULL) {
      if (ctx->app->exit == false && session_manager_should_exit() == false)
        LOG_ERROR("ST20P TX(%d): tx_fetch_next_frame failed", ctx->idx);
      break;
    }

#ifdef ENABLE_MTL_TX
    int ret = mtl_tx_send_yuv_frame(ctx, frame, crop_x, crop_y, crop_w, crop_h);
    if (ret < 0)
      LOG_ERROR("ST20P TX(%d): mtl_tx_send_yuv_frame failed", ctx->idx);
#else
    int ret = ffmpeg_tx_send_yuv_frame(ctx, frame, crop_x, crop_y, crop_w, crop_h);
    if (ret < 0)
      LOG_ERROR("ST20P TX(%d): ffmpeg_tx_send_yuv_frame failed", ctx->idx);
#endif
  }

  LOG_INFO("ST20P TX(%d): thread stopped, sent %d frames", ctx->idx, ctx->frames_sent);
  return NULL;
}

/* =========================================================================
 * Session creation
 * ========================================================================= */
/*
 * create_st20p_tx_session() — initialise one ST20P video TX session.
 */
int create_st20p_tx_session(session_manager_t* manager, struct dvledtx_context* app,
                             int session_idx) {
  struct st20p_tx_ctx* ctx = &manager->st20p_sessions[session_idx];

  memset(ctx, 0, sizeof(*ctx));
  ctx->idx = session_idx;
  ctx->app = app;

  ctx->crop_x_offset = app->session_net[session_idx].crop_x;
  ctx->crop_y_offset = app->session_net[session_idx].crop_y;
  ctx->crop_width    = app->session_net[session_idx].crop_w;
  ctx->crop_height   = app->session_net[session_idx].crop_h;

  /* Fallback: divide the frame width evenly across all sessions */
  if (ctx->crop_width == 0 || ctx->crop_height == 0) {
    int total   = app->st20p_sessions > 0 ? app->st20p_sessions : 1;
    int strip_w = (int)app->width / total;
    ctx->crop_x_offset = session_idx * strip_w;
    ctx->crop_y_offset = 0;
    ctx->crop_width  = (session_idx == total - 1)
                       ? ((int)app->width - ctx->crop_x_offset)
                       : strip_w;
    ctx->crop_height = (int)app->height;
  }
  LOG_INFO("ST20P TX session %d: crop rect x=%d y=%d w=%d h=%d",
           session_idx, ctx->crop_x_offset, ctx->crop_y_offset,
           ctx->crop_width, ctx->crop_height);

  snprintf(ctx->session_name, sizeof(ctx->session_name), "st20p_tx_%d", session_idx);

#ifdef ENABLE_MTL_TX
  if (mtl_tx_session_create(manager, ctx, app, session_idx) < 0)
    return -1;
#else
  if (open_ffmpeg_tx(ctx) < 0) {
    LOG_ERROR("Failed to open FFmpeg TX output for ST20P session %d", session_idx);
    return -1;
  }

  int fsize = av_image_get_buffer_size(app->fmt, ctx->crop_width, ctx->crop_height, 1);
  ctx->frame_size = (fsize > 0) ? (size_t)fsize : 0;
#endif /* ENABLE_MTL_TX */

  if (manager->shared_dec) {
    ctx->shared_dec = manager->shared_dec;
    LOG_INFO("ST20P TX session %d: using shared decoder", session_idx);
  } else if (strlen(app->tx_url) > 0) {
    if (load_video_source(ctx, app->tx_url) < 0) {
      LOG_ERROR("ST20P TX session %d: load_video_source failed", session_idx);
      return -1;
    }
  }

  return 0;
}

/* =========================================================================
 * session_manager_init / start / stop / cleanup
 * ========================================================================= */
int session_manager_init(session_manager_t* manager, struct dvledtx_context* app) {
  memset(manager, 0, sizeof(*manager));

#ifdef ENABLE_MTL_TX
  if (mtl_tx_init(manager, app) < 0)
    return -1;
#endif /* ENABLE_MTL_TX */

  /* Use shared decoder only when > 1 session and source needs decoding */
  bool use_shared = (app->st20p_sessions > 1 &&
                     strlen(app->tx_url) > 0 &&
                     is_raw_yuv(app->tx_url) == false);

  if (use_shared) {
    manager->shared_dec = calloc(1, sizeof(struct shared_decode_ctx));
    if (manager->shared_dec == NULL) return -1;

    manager->shared_dec->app          = app;
    manager->shared_dec->num_sessions = app->st20p_sessions;
    manager->shared_dec->exit         = false;
    manager->shared_dec->start_ready  = false;
    pthread_mutex_init(&manager->shared_dec->start_mutex, NULL);
    pthread_cond_init(&manager->shared_dec->start_cond, NULL);

    if (pthread_barrier_init(&manager->shared_dec->barrier_decoded,
                             NULL, (unsigned)(app->st20p_sessions + 1)) != 0) {
      LOG_ERROR("Failed to init barrier_decoded");
      free(manager->shared_dec);
      manager->shared_dec = NULL;
      return -1;
    }
    if (pthread_barrier_init(&manager->shared_dec->barrier_copied,
                             NULL, (unsigned)(app->st20p_sessions + 1)) != 0) {
      LOG_ERROR("Failed to init barrier_copied");
      pthread_barrier_destroy(&manager->shared_dec->barrier_decoded);
      free(manager->shared_dec);
      manager->shared_dec = NULL;
      return -1;
    }

    if (open_shared_ffmpeg(manager->shared_dec, app->tx_url) < 0) {
      LOG_ERROR("Failed to open shared FFmpeg source");
      pthread_barrier_destroy(&manager->shared_dec->barrier_decoded);
      pthread_barrier_destroy(&manager->shared_dec->barrier_copied);
      free(manager->shared_dec);
      manager->shared_dec = NULL;
      return -1;
    }
    LOG_INFO("Shared decoder ready for %d sessions", app->st20p_sessions);
  }

  if (app->st20p_sessions > 0) {
    manager->st20p_sessions = calloc((size_t)app->st20p_sessions,
                                     sizeof(struct st20p_tx_ctx));
    if (manager->st20p_sessions == NULL) { session_manager_cleanup(manager); return -1; }
    manager->st20p_count = app->st20p_sessions;

    for (int i = 0; i < app->st20p_sessions; i++) {
      if (create_st20p_tx_session(manager, app, i) < 0) {
        session_manager_cleanup(manager);
        return -1;
      }
    }
  }

  LOG_INFO("TX Session Manager: %d video sessions, shared_dec=%s",
           manager->st20p_count,
           manager->shared_dec != NULL ? "YES" : "NO");
  return 0;
}

int session_manager_start(session_manager_t* manager) {
  session_manager_reset_exit();

  if (manager->shared_dec != NULL) {
    if (pthread_create(&manager->shared_dec->decode_thread, NULL,
                       shared_decode_thread, manager->shared_dec) != 0) {
      LOG_ERROR("Failed to create shared decode thread");
      return -1;
    }
  }

  for (int i = 0; i < manager->st20p_count; i++) {
    struct st20p_tx_ctx* ctx = &manager->st20p_sessions[i];
    void *(*thread_fn)(void *) = (ctx->shared_dec != NULL)
                                 ? st20p_tx_thread_shared
                                 : st20p_tx_thread;

    if (pthread_create(&ctx->thread, NULL, thread_fn, ctx) != 0) {
      LOG_ERROR("Failed to create ST20P TX thread %d", i);
      session_manager_request_exit();
      if (manager->shared_dec != NULL) {
        /* Signal all threads to exit via the startup gate — threads have not
         * touched barriers yet, so no barrier deadlock is possible. */
        manager->shared_dec->exit = true;
        pthread_mutex_lock(&manager->shared_dec->start_mutex);
        manager->shared_dec->start_ready = true;
        pthread_cond_broadcast(&manager->shared_dec->start_cond);
        pthread_mutex_unlock(&manager->shared_dec->start_mutex);

        if (manager->shared_dec->decode_thread != 0) {
          pthread_join(manager->shared_dec->decode_thread, NULL);
          manager->shared_dec->decode_thread = 0;
        }
      }
      for (int j = 0; j < i; j++) {
        struct st20p_tx_ctx* c = &manager->st20p_sessions[j];
        if (c->thread != 0) { pthread_join(c->thread, NULL); c->thread = 0; }
      }
      return -1;
    }
  }

  /* All threads created successfully — release the startup gate */
  if (manager->shared_dec != NULL) {
    pthread_mutex_lock(&manager->shared_dec->start_mutex);
    manager->shared_dec->start_ready = true;
    pthread_cond_broadcast(&manager->shared_dec->start_cond);
    pthread_mutex_unlock(&manager->shared_dec->start_mutex);
  }

  manager->running = true;
  LOG_INFO("TX Session Manager started");
  return 0;
}

int session_manager_stop(session_manager_t* manager) {
  session_manager_request_exit();
  manager->running = false;

  /* Do NOT set shared_dec->exit here.  The decode thread's while-loop already
   * checks session_manager_should_exit() and will set dec->exit = true itself
   * just before its final barrier-pair sync.  Setting it early from this
   * thread races with TX threads that check dec->exit after barrier_decoded:
   * they would break after the first barrier round, leaving the decode thread
   * stuck waiting for participants in its final-sync barrier_decoded — a
   * deadlock. */

#ifdef ENABLE_MTL_TX
  for (int i = 0; i < manager->st20p_count; i++) {
    struct st20p_tx_ctx* ctx = &manager->st20p_sessions[i];
    if (ctx->handle != NULL) st20p_tx_wake_block(ctx->handle);
  }
#endif

  /* Join TX threads FIRST so they can participate in the decode thread's
   * final barrier-pair sync.  Joining decode first would deadlock if a TX
   * thread is delayed before reaching the barrier. */
  for (int i = 0; i < manager->st20p_count; i++) {
    struct st20p_tx_ctx* ctx = &manager->st20p_sessions[i];
    if (ctx->thread != 0) { pthread_join(ctx->thread, NULL); ctx->thread = 0; }
  }

  if (manager->shared_dec != NULL && manager->shared_dec->decode_thread != 0) {
    pthread_join(manager->shared_dec->decode_thread, NULL);
    manager->shared_dec->decode_thread = 0;
  }

  LOG_INFO("TX Session Manager stopped");
  return 0;
}

void session_manager_cleanup(session_manager_t* manager) {
  if (manager->running == true) session_manager_stop(manager);

  if (manager->st20p_sessions != NULL) {
    for (int i = 0; i < manager->st20p_count; i++) {
      struct st20p_tx_ctx* ctx = &manager->st20p_sessions[i];
#ifdef ENABLE_MTL_TX
      mtl_tx_session_free(ctx);
#else
      close_ffmpeg_tx(ctx);
#endif
      close_ffmpeg_source(ctx);
      if (ctx->source_buffer != NULL) { free(ctx->source_buffer); ctx->source_buffer = NULL; }
      /* Raw YUV path allocates yuv_frame without an AVBuffer (planes point into
       * source_buffer); av_frame_free releases only the AVFrame struct itself. */
      if (ctx->use_ffmpeg == false && ctx->yuv_frame != NULL) av_frame_free(&ctx->yuv_frame);
    }
    free(manager->st20p_sessions);
    manager->st20p_sessions = NULL;
  }

  if (manager->shared_dec != NULL) {
    close_shared_ffmpeg(manager->shared_dec);
    pthread_barrier_destroy(&manager->shared_dec->barrier_decoded);
    pthread_barrier_destroy(&manager->shared_dec->barrier_copied);
    pthread_mutex_destroy(&manager->shared_dec->start_mutex);
    pthread_cond_destroy(&manager->shared_dec->start_cond);
    free(manager->shared_dec);
    manager->shared_dec = NULL;
  }

#ifdef ENABLE_MTL_TX
  mtl_tx_uninit(manager);
#endif

  manager->st20p_count = 0;
}

bool session_manager_is_running(const session_manager_t* manager) {
  return manager->running;
}
