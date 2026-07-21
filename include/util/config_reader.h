/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "app_context.h"

// Forward declaration
struct dvledtx_context;

/* Per-session network + crop configuration (one entry per tx_sessions[] item) */
struct tx_session_config {
  uint16_t udp_port;
  uint8_t  payload_type;
  int      crop_x;
  int      crop_y;
  int      crop_w;
  int      crop_h;
  int      nic_index;  /* which NIC (index into interface arrays); default 0 */
};

/* Full application configuration parsed from JSON */
struct dvledtx_config {
  /* interfaces array — dynamically allocated */
  int  nic_count;                              /* number of interfaces parsed */
  int  nic_cap;                                /* allocated capacity */
  char (*interface_name)[64];                  /* PCI address per NIC */
  char (*interface_sip)[32];                   /* source IP per NIC */
  char (*interface_dip)[32];                   /* destination multicast IP per NIC */

  /* video block */
  uint32_t width;
  uint32_t height;
  uint32_t scale_width;   /* 0 = no scaling (use source width) */
  uint32_t scale_height;  /* 0 = no scaling (use source height) */
  int      fps;
  char     fmt[32];         /* e.g. "yuv422p10le" */
  char     tx_url[256];

  /* optional log file path (empty = console only) */
  char log_file[256];

  /* tx_sessions array — dynamically allocated */
  int session_count;
  int session_cap;                             /* allocated capacity */
  struct tx_session_config* sessions;
};

/* Quickly extract only the "log_file" value from a config file without a full parse.
 * Returns 0 and fills out_buf on success, -1 if not found or on error. */
int peek_config_log_file(const char* config_file, char* out_buf, size_t out_size);

/* Parse JSON config file into dvledtx_config */
int parse_tx_config(const char* config_file, struct dvledtx_config* config);

/* Validate parsed configuration */
int validate_tx_config(const struct dvledtx_config* config);

/* Load JSON config and apply it to the app context */
int load_and_apply_config(struct dvledtx_context* app, const char* config_file);

/* Convert sip_addr_str / dip_addr_str to packed binary (struct in_addr).
 * Call once after load_and_apply_config().  Returns 0 on success, -1 on error. */
int resolve_ip_addrs(struct dvledtx_context* ctx);

/* Free dynamically allocated members of dvledtx_config (interface arrays, sessions). */
void dvledtx_config_free(struct dvledtx_config* config);
