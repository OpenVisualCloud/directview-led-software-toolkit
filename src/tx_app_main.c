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
  LOG_INFO("Usage: %s --config <file>", prog_name);
  LOG_INFO("Options:");
  LOG_INFO("  -C, --config <file>   JSON config file (required)");
  LOG_INFO("  --help                Show this help");
}

static int parse_args(struct tx_app_context* ctx, int argc, char** argv) {
  static struct option long_options[] = {
    {"config", required_argument, 0, 'C'},
    {"help",   no_argument,       0, '?'},
    {0, 0, 0, 0}
  };

  ctx->config_file[0] = '\0';

  int c, option_index = 0;
  while ((c = getopt_long(argc, argv, "C:?", long_options, &option_index)) != -1) {
    switch (c) {
      case 'C':
        strncpy(ctx->config_file, optarg, sizeof(ctx->config_file) - 1);
        ctx->config_file[sizeof(ctx->config_file) - 1] = '\0';
        break;
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

  if (app.config_file[0] == '\0') {
    print_help(argv[0]);
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
          LOG_WARN("dup2 failed for log redirection");
        }
      } else {
        LOG_WARN("Could not open log file %s", log_file_path);
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
        LOG_WARN("Could not initialize logger to %s", log_file_path);
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
  LOG_INFO("Video: %dx%d, ST20P sessions: %d",
         app.width, app.height, app.st20p_sessions);

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
