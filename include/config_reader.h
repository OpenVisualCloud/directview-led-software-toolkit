/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "tx_app_context.h"

// Forward declaration
struct tx_app_context;

/* Per-session network + crop configuration (one entry per tx_sessions[] item) */
struct tx_session_config {
  uint16_t udp_port;
  uint8_t  payload_type;
  int      crop_x;
  int      crop_y;
  int      crop_w;
  int      crop_h;
};

/* Full application configuration parsed from JSON */
struct tx_app_config {
  /* interfaces[0] */
  char interface_name[64];  /* PCI address, e.g. "0000:02:00.0" */
  char interface_sip[32];   /* source IP, e.g. "192.168.50.29" */
  char interface_dip[32];   /* destination IP, e.g. "239.168.85.20" */

  /* video block */
  uint32_t width;
  uint32_t height;
  int      fps;
  char     fmt[32];         /* e.g. "yuv422p10le" */
  char     tx_url[256];

  /* tx_sessions array — count drives st20p_sessions */
  int session_count;
  struct tx_session_config sessions[MAX_TX_SESSIONS];
};

/* Parse JSON config file into tx_app_config */
int parse_tx_config(const char* config_file, struct tx_app_config* config);

/* Validate parsed configuration */
int validate_tx_config(const struct tx_app_config* config);

/* Load JSON config and apply it to the app context */
int load_and_apply_config(struct tx_app_context* app, const char* config_file);
