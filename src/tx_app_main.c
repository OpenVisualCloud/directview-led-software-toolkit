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
#include "util/logger.h"

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
            LOG_ERROR("Unsupported FPS %d", fps_val);
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
          LOG_ERROR("Unsupported format %s", optarg);
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
          LOG_ERROR("--st20p_sessions must be in range 0-%d", MAX_TX_SESSIONS);
          return -1;
        }
        ctx->st20p_sessions = sessions;
        break;
      }
      case '3': {
        int sessions = atoi(optarg);
        if (sessions < 0 || sessions > MAX_TX_SESSIONS) {
          LOG_ERROR("--st30p_sessions must be in range 0-%d", MAX_TX_SESSIONS);
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
        ctx->config_file[sizeof(ctx->config_file) - 1] = '\0';
        break;
      case 'P': {
        int pt = atoi(optarg);
        if (pt < 96 || pt > 127) {
          LOG_ERROR("--payload_type must be in range 96-127 (dynamic RTP)");
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
      LOG_ERROR("Invalid source IP address %s", ctx->sip_addr_str);
      return -1;
    }
  } else {
    LOG_INFO("No source IP provided, DHCP mode");
  }
  if (inet_pton(AF_INET, ctx->dip_addr_str, ctx->dip_addr) != 1) {
    LOG_ERROR("Invalid destination IP address %s", ctx->dip_addr_str);
    return -1;
  }
  return 0;
}

/* Main application */
int main(int argc, char** argv) {
  struct tx_app_context app;
  session_manager_t session_manager;
  int ret = 0;
  FILE *log_fp = NULL;
  memset(&app, 0, sizeof(app));

  /* Phase 1: minimal console logger so parse_args errors can be reported. */
  logger_init_default();

  /* Parse arguments (determines config_file path). */
  if (parse_args(&app, argc, argv) < 0) {
    ret = -1;
    goto cleanup_logger;
  }

  /* Phase 2: resolve log file destination BEFORE the full config load so that
   * "Config loaded" and session-info messages go directly to the log file.
   * Priority: config "log_file" > LOG_FILE env variable > console only. */
  {
    char peeked_log_file[256] = {0};
    const char *log_file_path = NULL;

    /* Try to peek log_file from config before the expensive full parse */
    if (app.config_file[0] != '\0' &&
        peek_config_log_file(app.config_file, peeked_log_file, sizeof(peeked_log_file)) == 0 &&
        peeked_log_file[0] != '\0') {
      log_file_path = peeked_log_file;
    } else {
      const char *env_log = getenv("LOG_FILE");
      if (env_log && env_log[0] != '\0')
        log_file_path = env_log;
    }

    if (log_file_path) {
      bool redirected = false;
      log_fp = fopen(log_file_path, "a");
      if (log_fp) {
        int r1 = dup2(fileno(log_fp), STDOUT_FILENO);
        int r2 = dup2(fileno(log_fp), STDERR_FILENO);
        if (r1 >= 0 && r2 >= 0) {
          setvbuf(stdout, NULL, _IOLBF, 0);
          setvbuf(stderr, NULL, _IOLBF, 0);
          redirected = true;
        } else {
          fprintf(stderr, "Warning: dup2 failed for log redirection\n");
        }
      } else {
        fprintf(stderr, "Warning: Could not open log file %s\n", log_file_path);
      }

      logger_cleanup();
      logger_config_t log_config = {
        .level = LOG_LEVEL_INFO,
        .enable_console = !redirected,
        .enable_file = true,
        .enable_timestamp = true,
        .enable_colors = false,
        .log_file = log_file_path
      };
      if (logger_init(&log_config) < 0) {
        fprintf(stderr, "Warning: Could not initialize logger to %s\n", log_file_path);
        logger_init_default();
      }
    }
    /* else: keep default console logger from Phase 1 */
  }

  LOG_INFO("TxApp initializing...");

  /* Load configuration from JSON if specified — must happen before IP resolve
   * because config supplies sip/dip when CLI args are omitted. */
  if (app.config_file[0] != '\0') {
    if (load_and_apply_config(&app, app.config_file) < 0) {
      LOG_ERROR("Failed to load config file %s", app.config_file);
      ret = -1;
      goto cleanup_logger;
    }
  }

  /* Resolve IP strings -> binary (after config may have overwritten them) */
  if (resolve_ip_addrs(&app) < 0) {
    ret = -1;
    goto cleanup_logger;
  }

  /* Install signal handler — set g_app_ptr first so the handler can set app.exit */
  g_app_ptr = &app;
  signal(SIGINT, tx_app_sig_handler);
  signal(SIGTERM, tx_app_sig_handler);

  /* Register all FFmpeg devices (required for the MTL mtl_st20p muxer
   * which lives in libavdevice, not libavformat) */
  avdevice_register_all();

  /* Initialize session manager */
  if (session_manager_init(&session_manager, &app) < 0) {
    LOG_ERROR("Failed to initialize session manager");
    ret = -1;
    goto cleanup_logger;
  }

  /* Start transmission sessions */
  if (session_manager_start(&session_manager) < 0) {
    LOG_ERROR("Failed to start sessions");
    ret = -1;
    goto cleanup;
  }

  LOG_INFO("TxApp started successfully");
  LOG_INFO("Port: %s, DIP: %s, UDP: %d", app.port, app.dip_addr_str, app.udp_port);
  LOG_INFO("Video: %dx%d, ST20P sessions: %d, ST30P sessions: %d",
         app.width, app.height, app.st20p_sessions, app.st30p_sessions);

  if (app.test_time_s > 0) {
    LOG_INFO("Transmitting for %d seconds... Press Ctrl+C to stop", app.test_time_s);
    for (int i = 0; i < app.test_time_s && !app.exit; i++) {
      tx_app_apply_pending_signal_exit();
      sleep(1);
    }
  } else {
    LOG_INFO("Transmitting indefinitely... Press Ctrl+C to stop");
    while (!app.exit) {
      tx_app_apply_pending_signal_exit();
      sleep(1);
    }
  }

  LOG_INFO("Stopping...");
  app.exit = true;

cleanup:
  /* Stop and cleanup session manager */
  session_manager_cleanup(&session_manager);
  LOG_INFO("TxApp shutdown complete");
cleanup_logger:
  /* Cleanup logger */
  logger_cleanup();
  /* Close log file if opened */
  if (log_fp) {
    fclose(log_fp);
  }

  return ret;
}
