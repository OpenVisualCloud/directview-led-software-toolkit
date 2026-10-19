/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "mtl_api.h"
#include "st_pipeline_api.h"
#include "x11_handler.h"
#include "config_reader.h"

/* Application state shared by main and the session manager. */
struct rx_app_context {
  mtl_handle mtl;
  uint32_t width;   /* full video size */
  uint32_t height;
  enum st_fps fps;
  enum st_frame_fmt fmt;
  enum st20_fmt transport_fmt;
  int st20p_sessions;
  bool exit;
  int test_time_s;  /* 0 = run until stopped */
  char config_file[256];
  struct rx_config json_config;
};

/* ST20P RX session context */
struct st20p_rx_ctx {
  struct rx_app_context* app;
  st20p_rx_handle handle;
  pthread_t thread;
  int idx;
  size_t frame_size;
  char session_name[32];

  /* Frame statistics */
  int frames_received;
  int frames_rx_window;   /* frames since the last per-second stats line (atomic) */

  /* Per-session display (used when st20p_count == 1) */
  struct x11_display_ctx display;

  /* Same frames mirrored onto further outputs (per_output mode) */
  struct x11_display_ctx mirror_display[MAX_MIRRORS];
  int num_mirror_displays;

  /* Composite display (used when st20p_count > 1, shared across sessions) */
  struct composite_x11_ctx* composite;

  /* Per-session crop region (from JSON config; valid when composite != NULL) */
  struct crop_config crop;

  /* Per-session network overrides (from JSON rx_sessions) */
  uint16_t session_udp_port;
  uint8_t  session_payload_type;
  char     port_name[MTL_PORT_MAX_LEN];     /* NIC this session receives on */
  uint8_t  dip_addr[MTL_IP_ADDR_LEN];       /* multicast group on that NIC */

  /* Decoupled display pipeline (single-session mode, composite == NULL).
   * The RX thread hands the newest MTL frame to this dedicated thread, which
   * converts/blits it and then returns it to MTL.  A frame superseded before
   * it is rendered is returned by the RX thread, so receive never stalls. */
  pthread_t       disp_thread;
  bool            disp_thread_started;
  pthread_mutex_t disp_lock;
  pthread_cond_t  disp_cond;
  struct st_frame* disp_pending;    /* newest MTL frame awaiting render (owned until put) */
  int             disp_skipped;     /* frames superseded before render (under disp_lock) */
};

/* Session Manager Interface */
typedef struct {
  mtl_handle mtl;
  struct st20p_rx_ctx* st20p_sessions;
  int st20p_count;
  bool running;

  /* Composite display (non-NULL in composite mode with st20p_count > 1) */
  struct composite_x11_ctx* composite;
} session_manager_t;

/* Initialize MTL and create session manager */
int session_manager_init(session_manager_t* manager, struct rx_app_context* app);

/* Start all sessions */
int session_manager_start(session_manager_t* manager, struct rx_app_context* app);

/* Stop all sessions */
void session_manager_stop(session_manager_t* manager);

/* Cleanup session manager */
void session_manager_cleanup(session_manager_t* manager);

/* Check if sessions are running */
bool session_manager_is_running(const session_manager_t* manager);
