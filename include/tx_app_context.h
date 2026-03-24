/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <libavutil/pixfmt.h>

/* Application context for TX sessions */
struct tx_app_context {
  /* Configuration */
  char port[64];        /* local network interface name (e.g. eth0) */
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

  /* Crop strip index (0-3) for 4-way vertical crop; session 0 sends this strip */
  int crop_idx;
};