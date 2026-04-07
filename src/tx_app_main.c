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

/* Reference the shared exit flag defined in session_manager.c.
 * Worker threads check this flag; do not write it from a signal handler —
 * only normal code should set it (via tx_app_apply_pending_signal_exit). */
extern _Atomic bool g_tx_app_exit;

/* File-level application context pointer set before signals are installed. */
static struct tx_app_context* g_app_ptr = NULL;

/* Async-signal-safe shutdown flag: only written by the signal handler,
 * only read by tx_app_apply_pending_signal_exit() in normal context. */
static volatile sig_atomic_t g_tx_app_signal_exit = 0;

static void tx_app_sig_handler(int sig) {
  static const char msg[] = "Signal received, exit\n";
  (void)sig;
  (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
  g_tx_app_signal_exit = 1;
}

/* Propagate a pending signal-driven shutdown into the shared exit flags.
 * Must be called from non-signal context (e.g. the main polling loop). */
static void tx_app_apply_pending_signal_exit(void) {
  if (!g_tx_app_signal_exit) return;
  g_tx_app_exit = true;
  if (g_app_ptr) g_app_ptr->exit = true;
}

static void print_help(const char* prog_name) {
  printf("Usage: %s [options]\n", prog_name);
  printf("Options:\n");
  printf("  -p, --port <pci>            DPDK NIC PCI BDF (e.g. 0000:af:00.0)\n");
  printf("  -s, --sip <ip>              Source IP address\n");
  printf("  -d, --dip <ip>              Destination IP address (default: 239.168.85.20)\n");
  printf("  -u, --udp_port <port>       UDP port (default: 20000)\n");
  printf("  -w, --width <width>         Video width (default: 1920)\n");
  printf("  -h, --height <height>       Video height (default: 1080)\n");
  printf("  -f, --fps <fps>             Frame rate (default: 25)\n");
  printf("  -F, --fmt <format>          Pixel format: yuv422p10le(default), yuv420p,\n");
  printf("                              yuv422p12le, yuv444p10le, yuv444p12le,\n");
  printf("                              gbrp10le, gbrp12le\n");
  printf("  -t, --tx_url <path>         Video source file path\n");
  printf("  -2, --st20p_sessions <n>    Number of ST20P sessions (default: 1)\n");
  printf("  -3, --st30p_sessions <n>    Number of ST30P sessions (default: 0)\n");
  printf("  -T, --time <seconds>        Test duration (default: 0 = run indefinitely)\n");
  printf("  -C, --config <file>         JSON config file\n");
  printf("  -D, --dhcp                  Use DHCP for IP configuration\n");
  printf("  -P, --payload_type <pt>     RTP payload type (default: 96)\n");
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
    {"help", no_argument, 0, '?'},
    {0, 0, 0, 0}
  };

  /* Set defaults */
  strncpy(ctx->port, "0000:af:01.0", sizeof(ctx->port) - 1);  /* placeholder — must be overridden with actual NIC PCI BDF */
  ctx->port[sizeof(ctx->port) - 1] = '\0';
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
  ctx->payload_type = 96; /* default: RTP dynamic payload type */

  int c, option_index = 0;
  while ((c = getopt_long(argc, argv, "p:s:d:u:w:h:f:F:t:2:3:T:DC:P:?", long_options, &option_index)) != -1) {
    switch (c) {
      case 'p':
        strncpy(ctx->port, optarg, sizeof(ctx->port) - 1);
        ctx->port[sizeof(ctx->port) - 1] = '\0';
        break;
      case 's':
        strncpy(ctx->sip_addr_str, optarg, sizeof(ctx->sip_addr_str) - 1);
        ctx->sip_addr_str[sizeof(ctx->sip_addr_str) - 1] = '\0';
        break;
      case 'd':
        strncpy(ctx->dip_addr_str, optarg, sizeof(ctx->dip_addr_str) - 1);
        ctx->dip_addr_str[sizeof(ctx->dip_addr_str) - 1] = '\0';
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
        ctx->tx_url[sizeof(ctx->tx_url) - 1] = '\0';
        break;
      case '2': {
        int sessions = atoi(optarg);
        if (sessions < 0 || sessions > MAX_TX_SESSIONS) {
          printf("Error: --st20p_sessions must be in range 0-%d\n", MAX_TX_SESSIONS);
          return -1;
        }
        ctx->st20p_sessions = sessions;
        break;
      }
      case '3': {
        int sessions = atoi(optarg);
        if (sessions < 0 || sessions > MAX_TX_SESSIONS) {
          printf("Error: --st30p_sessions must be in range 0-%d\n", MAX_TX_SESSIONS);
          return -1;
        }
        ctx->st30p_sessions = sessions;
        break;
      }
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

      case '?':
      default:
        print_help(argv[0]);
        return -1;
    }
  }

  return 0;
}

/* Convert sip_addr_str/dip_addr_str -> binary after config is loaded */
static int resolve_ip_addrs(struct tx_app_context* ctx) {
  if (ctx->sip_addr_str[0] != '\0') {
    if (inet_pton(AF_INET, ctx->sip_addr_str, ctx->sip_addr) != 1) {
      printf("Error: Invalid source IP address %s\n", ctx->sip_addr_str);
      return -1;
    }
  } else {
    printf("Info: No source IP provided, DHCP mode\n");
  }
  if (inet_pton(AF_INET, ctx->dip_addr_str, ctx->dip_addr) != 1) {
    printf("Error: Invalid destination IP address %s\n", ctx->dip_addr_str);
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

  /* Load configuration from JSON if specified — must happen before IP resolve
   * because config supplies sip/dip when CLI args are omitted */
  if (app.config_file[0] != '\0') {
    if (load_and_apply_config(&app, app.config_file) < 0) {
      printf("Error: Failed to load config file %s\n", app.config_file);
      return -1;
    }
  }

  /* Resolve IP strings -> binary (after config may have overwritten them) */
  if (resolve_ip_addrs(&app) < 0) return -1;

  /* Install signal handler — set g_app_ptr first so the handler can set app.exit */
  g_app_ptr = &app;
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
      tx_app_apply_pending_signal_exit();
      sleep(1);
    }
  } else {
    printf("Transmitting indefinitely... Press Ctrl+C to stop\n");
    while (!app.exit) {
      tx_app_apply_pending_signal_exit();
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
