/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <libavutil/pixfmt.h>

/* Fixed field-width constants (not array size limits) */
#define PORT_NAME_LEN     64
#define IP_STR_LEN        INET_ADDRSTRLEN
#define IP_ADDR_BYTES     4

/* Per-NIC configuration (one element per interface) */
struct nic_config {
  char    port[PORT_NAME_LEN];        /* DPDK NIC PCI BDF */
  char    sip_addr_str[IP_STR_LEN];   /* source IP string */
  uint8_t sip_addr[IP_ADDR_BYTES];    /* source IP binary */
  char    dip_addr_str[IP_STR_LEN];   /* destination IP string */
  uint8_t dip_addr[IP_ADDR_BYTES];    /* destination IP binary */
};

/* Per-session network and crop parameters (populated from JSON tx_sessions[]) */
struct tx_session_net {
  uint16_t udp_port;
  uint8_t  payload_type;
  int      crop_x;
  int      crop_y;
  int      crop_w;
  int      crop_h;
  int      nic_index;   /* which NIC this session uses (index into nics[]) */
};

/* Application context for TX sessions */
struct dvledtx_context {
  /* NIC configuration — dynamically allocated array [0..nic_count-1] */
  int              nic_count;          /* number of active NICs */
  struct nic_config* nics;             /* heap-allocated, nic_count elements */

  char tx_url[256];
  char config_file[256];
  uint16_t udp_port;
  uint8_t  payload_type;  /* RTP dynamic payload type (default: 96) */

  /* Video parameters */
  uint32_t width;
  uint32_t height;
  uint32_t scale_width;       /* output width after scaling (0 = no scaling) */
  uint32_t scale_height;      /* output height after scaling (0 = no scaling) */
  int fps;                    /* frames per second: 25, 30, 50, 60 */
  enum AVPixelFormat fmt;     /* e.g. AV_PIX_FMT_YUV422P10LE */

  /* Session controls */
  int st20p_sessions;
  bool exit;
  bool force_dhcp;
  int test_time_s;

  /* Per-session network + crop config — dynamically allocated, st20p_sessions elements */
  struct tx_session_net* session_net;

  /* Optional log file path from config (empty = console logging only) */
  char log_file[256];

};

/* Free dynamically allocated members of dvledtx_context (nics, session_net).
 * Does NOT free the struct itself (it may be stack-allocated). */
static inline void dvledtx_context_free(struct dvledtx_context* ctx) {
  if (ctx == NULL) return;
  free(ctx->nics);        ctx->nics = NULL;
  free(ctx->session_net); ctx->session_net = NULL;
  ctx->nic_count = 0;
  ctx->st20p_sessions = 0;
}

/* Allocate NIC and session arrays inside an already zeroed context.
 * calloc(0, ...) is permitted by the C standard to return NULL, which is
 * not an allocation failure, so a zero count is treated as "nothing to
 * allocate" rather than an error (avoids spurious failures, e.g. in tests
 * that call dvledtx_context_alloc(ctx, 1, 0)). On failure, ctx is left
 * fully zeroed (no dangling nics pointer with a stale nic_count) so it
 * remains safe to pass to dvledtx_context_free().
 * Returns 0 on success, -1 on allocation failure. */
static inline int dvledtx_context_alloc(struct dvledtx_context* ctx,
                                        int nic_count, int session_count) {
  if (nic_count > 0) {
    ctx->nics = calloc((size_t)nic_count, sizeof(struct nic_config));
    if (ctx->nics == NULL) return -1;
  }
  ctx->nic_count = nic_count;

  if (session_count > 0) {
    ctx->session_net = calloc((size_t)session_count, sizeof(struct tx_session_net));
    if (ctx->session_net == NULL) {
      free(ctx->nics); ctx->nics = NULL; ctx->nic_count = 0;
      return -1;
    }
  }
  ctx->st20p_sessions = session_count;
  return 0;
}
