/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

/* libavdevice is only needed for the FFmpeg mtl_st20p muxer TX path */
#ifndef ENABLE_MTL_TX
#include <libavdevice/avdevice.h>
#endif
#include <libavutil/log.h>
#include "app_context.h"
#include "util/config_reader.h"
#include "core/session_manager.h"
#include "util/logger.h"

#define DVLEDTX_VERSION "0.1.0"

/* File-level application context pointer set before signals are installed. */
static struct dvledtx_context* g_app_ptr = NULL;

/* Async-signal-safe shutdown flag: only written by the signal handler,
 * only read by dvledtx_apply_pending_signal_exit() in normal context. */
static volatile sig_atomic_t g_dvledtx_signal_exit = 0;

static void dvledtx_sig_handler(int sig) {
  static const char msg[] = "Signal received, exit\n";
  (void)sig;
  (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
  g_dvledtx_signal_exit = 1;
}

/* Propagate a pending signal-driven shutdown into the shared exit flags.
 * Must be called from non-signal context (e.g. the main polling loop). */
static void dvledtx_apply_pending_signal_exit(void) {
  if (g_dvledtx_signal_exit == 0) return;
  session_manager_request_exit();
  if (g_app_ptr != NULL) g_app_ptr->exit = true;
}

/* =========================================================================
 * E-5: Log file path validation — restrict to safe directories.
 *
 * Ensures the resolved log file path is under an allowed prefix to prevent
 * arbitrary file writes (e.g. attacker setting log_file=/etc/cron.d/evil).
 * ========================================================================= */
static const char* ALLOWED_LOG_PREFIXES[] = {
  "/var/log/",
  NULL  /* sentinel — also allows paths relative to cwd (resolved below) */
};

static bool validate_log_path(const char* path) {
  if (path == NULL || path[0] == '\0') return false;

  char resolved[PATH_MAX];
  /* Resolve to absolute path; realpath fails if file doesn't exist yet,
   * so resolve the directory portion instead. */
  char pathcopy[PATH_MAX];
  strncpy(pathcopy, path, sizeof(pathcopy) - 1);
  pathcopy[sizeof(pathcopy) - 1] = '\0';

  /* Find the directory part */
  char* slash = strrchr(pathcopy, '/');
  if (slash != NULL) {
    *slash = '\0';
    if (realpath(pathcopy, resolved) == NULL) {
      /* Directory doesn't exist — reject */
      return false;
    }
    /* Safely append '/' + filename */
    size_t dlen = strlen(resolved);
    size_t flen = strlen(slash + 1);
    if (dlen + 1 + flen >= sizeof(resolved)) return false;
    resolved[dlen] = '/';
    memcpy(resolved + dlen + 1, slash + 1, flen + 1);
  } else {
    /* Filename in current directory — get cwd */
    if (getcwd(resolved, sizeof(resolved)) == NULL) return false;
    size_t clen = strlen(resolved);
    size_t plen = strlen(path);
    if (clen + 1 + plen >= sizeof(resolved)) return false;
    resolved[clen] = '/';
    memcpy(resolved + clen + 1, path, plen + 1);
  }

  /* Check against allowed prefixes */
  for (int i = 0; ALLOWED_LOG_PREFIXES[i] != NULL; i++) {
    if (strncmp(resolved, ALLOWED_LOG_PREFIXES[i],
                strlen(ALLOWED_LOG_PREFIXES[i])) == 0)
      return true;
  }

  /* Also allow paths under the current working directory */
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) != NULL) {
    size_t cwdlen = strlen(cwd);
    if (strncmp(resolved, cwd, cwdlen) == 0 &&
        (resolved[cwdlen] == '/' || resolved[cwdlen] == '\0'))
      return true;
  }

  return false;
}

static void print_help(const char* prog_name) {
  LOG_INFO("Usage: %s --config <file> [options]", prog_name);
  LOG_INFO("Options:");
  LOG_INFO("  -C, --config <file>       JSON config file (required)");
  LOG_INFO("  -t, --test-time <secs>    Transmit for N seconds then exit (1..86400)");
  LOG_INFO("  -v, --version             Show version");
  LOG_INFO("  --help                    Show this help");
}

static int parse_args(struct dvledtx_context* ctx, int argc, char** argv) {
  static struct option long_options[] = {
    {"config",    required_argument, 0, 'C'},
    {"test-time", required_argument, 0, 't'},
    {"version",   no_argument,       0, 'v'},
    {"help",      no_argument,       0, '?'},
    {0, 0, 0, 0}
  };

  ctx->config_file[0] = '\0';

  int c = 0, option_index = 0;
  while ((c = getopt_long(argc, argv, "C:t:v?", long_options, &option_index)) != -1) { /* flawfinder: ignore */
    switch (c) {
      case 'C':
        strncpy(ctx->config_file, optarg, sizeof(ctx->config_file) - 1);
        ctx->config_file[sizeof(ctx->config_file) - 1] = '\0';
        break;
      case 't': {
        char *endptr = NULL;
        long val = strtol(optarg, &endptr, 10);
        if (endptr == optarg || *endptr != '\0' || val <= 0 || val > 86400) {
          LOG_ERROR("Invalid --test-time value '%s' (expected 1..86400)", optarg);
          return -1;
        }
        ctx->test_time_s = (int)val;
        break;
      }
      case 'v':
        printf("dvledtx version %s\n", DVLEDTX_VERSION);
        exit(0);
      case '?':
      default:
        print_help(argv[0]);
        return -1;
    }
  }

  return 0;
}

/* Main application */
int main(int argc, char** argv) {
  struct dvledtx_context app;
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

  /* Reject symlinked config files to prevent symlink-based attacks */
  {
    struct stat lst;
    if (lstat(app.config_file, &lst) == 0 && S_ISLNK(lst.st_mode)) {
      LOG_ERROR("Config file '%s' is a symbolic link — rejected for security",
                app.config_file);
      ret = -1;
      goto cleanup_logger;
    }
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
      if (env_log != NULL && env_log[0] != '\0')
        log_file_path = env_log;
    }

    if (log_file_path != NULL) {
      /* E-5: Validate log file path before opening */
      if (validate_log_path(log_file_path) == false) {
        LOG_WARN("Log file path '%s' rejected — not under allowed directory. "
                 "Using console logging.", log_file_path);
        log_file_path = NULL;
      }
    }

    if (log_file_path != NULL) {
      bool redirected = false;
      log_fp = fopen(log_file_path, "a");
      if (log_fp != NULL) {
        int r1 = dup2(fileno(log_fp), STDOUT_FILENO);
        int r2 = dup2(fileno(log_fp), STDERR_FILENO);
        if (r1 >= 0 && r2 >= 0) {
          if (setvbuf(stdout, NULL, _IOLBF, 0) != 0)
            LOG_WARN("setvbuf(stdout) failed");
          if (setvbuf(stderr, NULL, _IOLBF, 0) != 0)
            LOG_WARN("setvbuf(stderr) failed");
          logger_set_stdout_redirected(true);
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
        .enable_console = (redirected == false),
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

  LOG_INFO("dvledtx initializing...");

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
  if (signal(SIGINT, dvledtx_sig_handler) == SIG_ERR)
    LOG_WARN("Failed to install SIGINT handler");
  if (signal(SIGTERM, dvledtx_sig_handler) == SIG_ERR)
    LOG_WARN("Failed to install SIGTERM handler");

  /* Register all FFmpeg devices (required for the MTL mtl_st20p muxer
   * which lives in libavdevice, not libavformat) */
#ifndef ENABLE_MTL_TX
  avdevice_register_all();
#endif

  /* I-3: Suppress verbose FFmpeg internal logging in production to avoid
   * leaking internal paths or memory addresses via av_strerror output. */
  av_log_set_level(AV_LOG_ERROR);

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

  LOG_INFO("dvledtx started successfully");
  if (app.nic_count > 0)
    LOG_INFO("Port[0]: %s, DIP[0]: %s, UDP: %d",
             app.nics[0].port, app.nics[0].dip_addr_str, app.udp_port);
  LOG_INFO("Video: %dx%d, ST20P sessions: %d",
         app.width, app.height, app.st20p_sessions);

  if (app.test_time_s > 0) {
    LOG_INFO("Transmitting for %d seconds... Press Ctrl+C to stop", app.test_time_s);
    for (int i = 0; i < app.test_time_s && app.exit == false; i++) {
      dvledtx_apply_pending_signal_exit();
      sleep(1);
    }
  } else {
    LOG_INFO("Transmitting indefinitely... Press Ctrl+C to stop");
    while (app.exit == false) {
      dvledtx_apply_pending_signal_exit();
      sleep(1);
    }
  }

  LOG_INFO("Stopping...");
  app.exit = true;

  /* R-2: Log shutdown reason for audit trail */
  LOG_INFO("Shutdown reason: %s",
           (g_dvledtx_signal_exit != 0) ? "signal (SIGINT/SIGTERM)" :
           (app.test_time_s > 0 ? "test_time elapsed" : "application exit"));

cleanup:
  /* Stop and cleanup session manager */
  session_manager_cleanup(&session_manager);
  dvledtx_context_free(&app);
  LOG_INFO("dvledtx shutdown complete");
cleanup_logger:
  /* Cleanup logger */
  logger_cleanup();
  /* Close log file if opened */
  if (log_fp != NULL) {
    fclose(log_fp);
  }

  return ret;
}
