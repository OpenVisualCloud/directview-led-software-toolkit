/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libavutil/pixfmt.h>
#include <libavdevice/avdevice.h>
#include "tx_app_context.h"
#include "config_reader.h"
#include "session_manager.h"

static volatile bool g_tx_app_exit = false;

static void tx_app_sig_handler(int sig) {
  printf("Signal %d received, exit\n", sig);
  g_tx_app_exit = true;
}

static void print_help(const char* prog_name) {
  printf("Usage: %s [options]\n", prog_name);
  printf("Options:\n");
  printf("  -p, --port <iface>          Local network interface (default: eth0)\n");
  printf("  -s, --sip <ip>              Source IP address\n");
  printf("  -d, --dip <ip>              Destination IP address (default: 239.168.1.101)\n");
  printf("  -u, --udp_port <port>       UDP port (default: 20000)\n");
  printf("  -w, --width <width>         Video width (default: 1920)\n");
  printf("  -h, --height <height>       Video height (default: 1080)\n");
  printf("  -f, --fps <fps>             Frame rate (default: 59.94)\n");
  printf("  -F, --fmt <format>          Pixel format: yuv422p10le(default), yuv420p,\n");
  printf("                              yuv422p12le, yuv444p10le, yuv444p12le,\n");
  printf("                              gbrp10le, gbrp12le\n");
  printf("  -t, --tx_url <path>         Video source file path\n");
  printf("  -2, --st20p_sessions <n>    Number of ST20P sessions (default: 1)\n");
  printf("  -3, --st30p_sessions <n>    Number of ST30P sessions (default: 0)\n");
  printf("  -T, --test_time <seconds>   Test duration (default: 60)\n");
  printf("  -c, --config <file>         JSON config file\n");
  printf("  --dhcp                      Use DHCP for IP configuration\n");
  printf("  --payload_type <pt>         RTP payload type (default: 96)\n");
  printf("  --crop_idx <n>              Which vertical crop strip to send (0-based, default: 0)\n");
  printf("  --help                      Show this help\n");
}

static int parse_args(struct tx_app_context* ctx, int argc, char** argv) {
  static struct option long_options[] = {
    {"port", required_argument, 0, 'p'},
    {"sip", required_argument, 0, 's'},
    {"dip", required_argument, 0, 'd'},
    {"udp_port", required_argument, 0, 'u'},
    {"width", required_argument, 0, 'w'},
    {"height", required_argument, 0, 'h'},
    {"fps", required_argument, 0, 'f'},
    {"fmt", required_argument, 0, 'F'},
    {"tx_url", required_argument, 0, 't'},
    {"st20p_sessions", required_argument, 0, '2'},
    {"st30p_sessions", required_argument, 0, '3'},
    {"time", required_argument, 0, 'T'},
    {"dhcp", no_argument, 0, 'D'},
    {"config", required_argument, 0, 'C'},
    {"payload_type", required_argument, 0, 'P'},
    {"crop_idx", required_argument, 0, 'X'},
    {"help", no_argument, 0, '?'},
    {0, 0, 0, 0}
  };

  /* Set defaults */
  strncpy(ctx->port, "eth0", sizeof(ctx->port));
  ctx->sip_addr_str[0] = '\0'; /* No default - must be provided */
  strncpy(ctx->dip_addr_str, "239.168.85.20", sizeof(ctx->dip_addr_str));
  ctx->udp_port = 20000;
  ctx->width = 1920;
  ctx->height = 1080;
  ctx->fps = 25;
  ctx->fmt = AV_PIX_FMT_YUV422P10LE;
  ctx->st20p_sessions = 1;
  ctx->st30p_sessions = 0;
  ctx->force_dhcp = false;
  ctx->test_time_s = 0;
  ctx->tx_url[0] = '\0';
  ctx->config_file[0] = '\0';
  ctx->crop_idx = 0; /* default: first strip */
  ctx->payload_type = 96; /* default: RTP dynamic payload type */

  int c, option_index = 0;
  while ((c = getopt_long(argc, argv, "p:s:d:u:w:h:f:F:t:2:3:T:DC:P:X:?", long_options, &option_index)) != -1) {
    switch (c) {
      case 'p':
        strncpy(ctx->port, optarg, sizeof(ctx->port) - 1);
        break;
      case 's':
        strncpy(ctx->sip_addr_str, optarg, sizeof(ctx->sip_addr_str) - 1);
        break;
      case 'd':
        strncpy(ctx->dip_addr_str, optarg, sizeof(ctx->dip_addr_str) - 1);
        break;
      case 'u':
        ctx->udp_port = atoi(optarg);
        break;
      case 'w':
        ctx->width = atoi(optarg);
        break;
      case 'h':
        ctx->height = atoi(optarg);
        break;
      case 'f': {
        int fps_val = atoi(optarg);
        switch (fps_val) {
          case 25: ctx->fps = 25; break;
          case 30: ctx->fps = 30; break;
          case 50: ctx->fps = 50; break;
          case 60: ctx->fps = 60; break;
          default:
            printf("Error: Unsupported FPS %d\n", fps_val);
            return -1;
        }
        break;
      }
      case 'F':
        if (strcmp(optarg, "yuv422p10le") == 0)
          ctx->fmt = AV_PIX_FMT_YUV422P10LE;
        else if (strcmp(optarg, "yuv420p") == 0)
          ctx->fmt = AV_PIX_FMT_YUV420P;
        else if (strcmp(optarg, "yuv422p12le") == 0)
          ctx->fmt = AV_PIX_FMT_YUV422P12LE;
        else if (strcmp(optarg, "yuv444p10le") == 0)
          ctx->fmt = AV_PIX_FMT_YUV444P10LE;
        else if (strcmp(optarg, "yuv444p12le") == 0)
          ctx->fmt = AV_PIX_FMT_YUV444P12LE;
        else if (strcmp(optarg, "gbrp10le") == 0)
          ctx->fmt = AV_PIX_FMT_GBRP10LE;
        else if (strcmp(optarg, "gbrp12le") == 0)
          ctx->fmt = AV_PIX_FMT_GBRP12LE;
        else {
          printf("Error: Unsupported format %s\n", optarg);
          return -1;
        }
        break;
      case 't':
        strncpy(ctx->tx_url, optarg, sizeof(ctx->tx_url) - 1);
        break;
      case '2':
        ctx->st20p_sessions = atoi(optarg);
        break;
      case '3':
        ctx->st30p_sessions = atoi(optarg);
        break;
      case 'T':
        ctx->test_time_s = atoi(optarg);
        break;
      case 'D':
        ctx->force_dhcp = true;
        break;
      case 'C':
        strncpy(ctx->config_file, optarg, sizeof(ctx->config_file) - 1);
        break;
      case 'P': {
        int pt = atoi(optarg);
        if (pt < 96 || pt > 127) {
          printf("Error: --payload_type must be in range 96-127 (dynamic RTP)\n");
          return -1;
        }
        ctx->payload_type = (uint8_t)pt;
        break;
      }
      case 'X':
        ctx->crop_idx = atoi(optarg);
        if (ctx->crop_idx < 0) {
          printf("Error: --crop_idx must be >= 0\n");
          return -1;
        }
        break;
      case '?':
      default:
        print_help(argv[0]);
        return -1;
    }
  }

  /* Convert IP addresses - SIP is optional if using DHCP */
  if (ctx->sip_addr_str[0] != '\0') {
    /* Static IP mode - validate SIP */
    if (inet_pton(AF_INET, ctx->sip_addr_str, ctx->sip_addr) != 1) {
      printf("Error: Invalid source IP address %s\n", ctx->sip_addr_str);
      return -1;
    }
  } else {
    /* No SIP provided - will use DHCP mode */
    printf("Info: No source IP provided, will use DHCP for automatic IP configuration\n");
  }

  if (inet_pton(AF_INET, ctx->dip_addr_str, ctx->dip_addr) != 1) {
    printf("Error: Invalid IP address %s\n", ctx->dip_addr_str);
    return -1;
  }

  return 0;
}

/* Main application */
int main(int argc, char** argv) {
  struct tx_app_context app;
  session_manager_t session_manager;
  int ret = 0;

  memset(&app, 0, sizeof(app));

  /* Parse arguments */
  if (parse_args(&app, argc, argv) < 0) {
    return -1;
  }

  /* Load configuration from JSON if specified */
  if (load_and_apply_config(&app, app.config_file) < 0) {
    printf("Warning: Failed to load config file, using defaults\n");
  }

  /* Install signal handler */
  signal(SIGINT, tx_app_sig_handler);
  signal(SIGTERM, tx_app_sig_handler);

  /* Register all FFmpeg devices (required for the MTL Kahawai mtl_st20p muxer
   * which lives in libavdevice, not libavformat) */
  avdevice_register_all();

  /* Initialize session manager */
  if (session_manager_init(&session_manager, &app) < 0) {
    printf("Error: Failed to initialize session manager\n");
    return -1;
  }

  /* Start transmission sessions */
  if (session_manager_start(&session_manager) < 0) {
    printf("Error: Failed to start sessions\n");
    ret = -1;
    goto cleanup;
  }

  printf("TxApp started successfully\n");
  printf("Port: %s, DIP: %s, UDP: %d\n", app.port, app.dip_addr_str, app.udp_port);
  printf("Video: %dx%d, ST20P sessions: %d, ST30P sessions: %d\n",
         app.width, app.height, app.st20p_sessions, app.st30p_sessions);

  if (app.test_time_s > 0) {
    printf("Transmitting for %d seconds... Press Ctrl+C to stop\n", app.test_time_s);
    for (int i = 0; i < app.test_time_s && !app.exit; i++) {
      sleep(1);
    }
  } else {
    printf("Transmitting indefinitely... Press Ctrl+C to stop\n");
    while (!app.exit) {
      sleep(1);
    }
  }

  printf("Stopping...\n");
  app.exit = true;

cleanup:
  /* Stop and cleanup session manager */
  session_manager_cleanup(&session_manager);

  printf("TxApp shutdown complete\n");
  return ret;
}
