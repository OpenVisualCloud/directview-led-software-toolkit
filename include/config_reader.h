/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

// Forward declaration
struct tx_app_context;

// TX Configuration structure to hold parsed JSON values
struct tx_config {
  // Interface configuration
  char interface_name[64];
  char interface_ip[32];

  // Session base configuration
  char dip[32];
  uint16_t start_port;

  // Video configuration
  bool video_enable;
  char video_codec[16];
  uint32_t width;
  uint32_t height;
  char fps_str[8];
  int fps_value;
  bool interlaced;
  char input_format[32];
  char transport_format[32];
  char st20p_url[256];

  // Audio configuration (optional)
  bool audio_enable;
  char audio_codec[16];
  uint32_t audio_channel;
  char audio_sampling[16];
  char audio_ptime[16];

  // Payload type
  uint8_t payload_type;

  // Device type
  char device[16];
};

// Function to parse TX JSON configuration file
int parse_tx_config(const char* config_file, struct tx_config* config);

// Helper function to convert fps string to enum value
int fps_string_to_value(const char* fps_str);

// Helper function to validate TX configuration
int validate_tx_config(const struct tx_config* config);

// Function to load and apply JSON configuration to app context
int load_and_apply_config(struct tx_app_context* app, const char* config_file);