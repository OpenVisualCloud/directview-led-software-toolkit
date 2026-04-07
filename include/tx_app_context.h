/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <libavutil/pixfmt.h>

#define MAX_TX_SESSIONS 8

/* Per-session network and crop parameters (populated from JSON tx_sessions[]) */
struct tx_session_net {
  uint16_t udp_port;
  uint8_t  payload_type;
  int      crop_x;
  int      crop_y;
  int      crop_w;
  int      crop_h;
};

/* Application context for TX sessions */
struct tx_app_context {
  /* Configuration */
  char port[64];        /* DPDK NIC PCI BDF/address for MTL output (e.g. 0000:af:00.0), not a Linux interface name */
  char tx_url[256];
  char config_file[256];
  char sip_addr_str[INET_ADDRSTRLEN];
  uint8_t sip_addr[4];
  char dip_addr_str[INET_ADDRSTRLEN];
  uint8_t dip_addr[4];
  uint16_t udp_port;
  uint8_t  payload_type;  /* RTP dynamic payload type (default: 96) */

  /* Video parameters */
  uint32_t width;
  uint32_t height;
  int fps;                    /* frames per second: 25, 30, 50, 60 */
  enum AVPixelFormat fmt;     /* e.g. AV_PIX_FMT_YUV422P10LE */

  /* Session controls */
  int st20p_sessions;
  int st30p_sessions;
  bool exit;
  bool force_dhcp;
  int test_time_s;

  /* Per-session network + crop config (from JSON tx_sessions[]) */
  struct tx_session_net session_net[MAX_TX_SESSIONS];

};
