/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "fake_mtl.h"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POOL 3

struct fake_mtl_state fake_mtl;

struct fake_rx {
  struct st20p_rx_ops ops;
  struct st_frame frames[POOL];
  bool in_use[POOL];
  int served;
  bool woken;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_main_token;

static int env_int(const char* name, int def) {
  const char* v = getenv(name);
  return v && v[0] ? atoi(v) : def;
}

void fake_mtl_reset(void) {
  memset(&fake_mtl, 0, sizeof(fake_mtl));
  fake_mtl.fail_init = env_int("FAKE_MTL_FAIL_INIT", 0) != 0;
  fake_mtl.fail_create_at = env_int("FAKE_MTL_FAIL_CREATE", -1);
  fake_mtl.frames_per_session = env_int("FAKE_MTL_FRAMES", -1);
  fake_mtl.empty_every = env_int("FAKE_MTL_EMPTY_EVERY", 0);
  fake_mtl.raise_after = env_int("FAKE_MTL_RAISE_AFTER", 0);
}

__attribute__((constructor)) static void fake_mtl_ctor(void) { fake_mtl_reset(); }

static bool is_planar16(enum st_frame_fmt f) {
  return f == ST_FRAME_FMT_YUV422PLANAR10LE || f == ST_FRAME_FMT_YUV422PLANAR12LE ||
         f == ST_FRAME_FMT_YUV444PLANAR10LE || f == ST_FRAME_FMT_YUV444PLANAR12LE ||
         f == ST_FRAME_FMT_GBRPLANAR10LE || f == ST_FRAME_FMT_GBRPLANAR12LE;
}

/* Allocate one frame of the session's output format, filled with a mid-level ramp. */
static void frame_init(struct st_frame* f, enum st_frame_fmt fmt, uint32_t w, uint32_t h) {
  memset(f, 0, sizeof(*f));
  f->fmt = fmt;
  f->width = w;
  f->height = h;
  if (is_planar16(fmt)) {
    bool sub = fmt == ST_FRAME_FMT_YUV422PLANAR10LE || fmt == ST_FRAME_FMT_YUV422PLANAR12LE;
    for (int p = 0; p < 3; p++) {
      uint32_t pw = (p > 0 && sub) ? w / 2 : w;
      uint16_t* plane = malloc((size_t)pw * h * 2);
      for (size_t i = 0; i < (size_t)pw * h; i++) plane[i] = (uint16_t)(256 + i % 256);
      f->addr[p] = plane;
      f->linesize[p] = (size_t)pw * 2;
      f->data_size += (size_t)pw * h * 2;
    }
  } else if (fmt == ST_FRAME_FMT_YUV420PLANAR8) {
    size_t sz[3] = {(size_t)w * h, (size_t)w * h / 4, (size_t)w * h / 4};
    for (int p = 0; p < 3; p++) {
      f->addr[p] = calloc(1, sz[p] ? sz[p] : 1);
      f->linesize[p] = p ? w / 2 : w;
      f->data_size += sz[p];
    }
  } else {
    size_t sz = (size_t)w * h * 4; /* covers YUV420CUSTOM8 and packed formats */
    f->addr[0] = calloc(1, sz ? sz : 1);
    f->linesize[0] = (size_t)w * 4;
    f->data_size = sz;
  }
  f->buffer_size = f->data_size;
}

mtl_handle mtl_init(struct mtl_init_params* p) {
  fake_mtl.init_calls++;
  if (p) fake_mtl.last_init = *p;
  if (fake_mtl.fail_init) return NULL;
  return (mtl_handle)&g_main_token;
}

int mtl_uninit(mtl_handle mt) {
  (void)mt;
  fake_mtl.uninit_calls++;
  return 0;
}

enum mtl_pmd_type mtl_pmd_by_port_name(const char* port) {
  (void)port;
  return MTL_PMD_DPDK_USER;
}

st20p_rx_handle st20p_rx_create(mtl_handle mt, struct st20p_rx_ops* ops) {
  (void)mt;
  int idx = fake_mtl.create_calls++;
  if (idx < FAKE_MAX_RX) {
    fake_mtl.ops[idx] = *ops;
    snprintf(fake_mtl.names[idx], sizeof(fake_mtl.names[idx]), "%s", ops->name ? ops->name : "");
    fake_mtl.ops[idx].name = fake_mtl.names[idx];
  }
  if (idx == fake_mtl.fail_create_at) return NULL;

  struct fake_rx* rx = calloc(1, sizeof(*rx));
  rx->ops = *ops;
  for (int i = 0; i < POOL; i++) frame_init(&rx->frames[i], ops->output_fmt, ops->width, ops->height);
  return (st20p_rx_handle)rx;
}

size_t st20p_rx_frame_size(st20p_rx_handle handle) {
  return ((struct fake_rx*)handle)->frames[0].data_size;
}

struct st_frame* st20p_rx_get_frame(st20p_rx_handle handle) {
  struct fake_rx* rx = (struct fake_rx*)handle;
  struct st_frame* out = NULL;
  bool do_raise = false;

  pthread_mutex_lock(&g_lock);
  if (!rx->woken &&
      (fake_mtl.frames_per_session < 0 || rx->served < fake_mtl.frames_per_session)) {
    for (int i = 0; i < POOL && !out; i++) {
      if (rx->in_use[i]) continue;
      rx->in_use[i] = true;
      out = &rx->frames[i];
    }
  }
  if (out) {
    rx->served++;
    out->data_size =
        (fake_mtl.empty_every > 0 && rx->served % fake_mtl.empty_every == 0) ? 0 : out->buffer_size;
    fake_mtl.frames_got++;
    do_raise = fake_mtl.raise_after > 0 && fake_mtl.frames_got == fake_mtl.raise_after;
  }
  pthread_mutex_unlock(&g_lock);

  if (do_raise) raise(SIGTERM);
  return out;
}

int st20p_rx_put_frame(st20p_rx_handle handle, struct st_frame* frame) {
  struct fake_rx* rx = (struct fake_rx*)handle;
  int ret = -1;
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < POOL; i++) {
    if (frame == &rx->frames[i] && rx->in_use[i]) {
      rx->in_use[i] = false;
      fake_mtl.frames_put++;
      ret = 0;
    }
  }
  if (ret) fake_mtl.bad_puts++;
  pthread_mutex_unlock(&g_lock);
  return ret;
}

int st20p_rx_wake_block(st20p_rx_handle handle) {
  pthread_mutex_lock(&g_lock);
  ((struct fake_rx*)handle)->woken = true;
  fake_mtl.wake_calls++;
  pthread_mutex_unlock(&g_lock);
  return 0;
}

int st20p_rx_free(st20p_rx_handle handle) {
  struct fake_rx* rx = (struct fake_rx*)handle;
  pthread_mutex_lock(&g_lock);
  fake_mtl.free_calls++;
  for (int i = 0; i < POOL; i++) {
    if (rx->in_use[i]) fake_mtl.leaked++;
    for (int p = 0; p < ST_MAX_PLANES; p++) free(rx->frames[i].addr[p]);
  }
  pthread_mutex_unlock(&g_lock);
  free(rx);
  return 0;
}
