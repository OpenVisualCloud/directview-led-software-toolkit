/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include "mtl_api.h"
#include "st_pipeline_api.h"
#include "frame_converter.h"
#include "x11_handler.h"
#include "config_reader.h"
#include "session_manager.h"
#include "logger.h"

#define LOGGER_TAG "main"

/* Overridden by the build system with the meson project version. */
#ifndef DVLEDRX_VERSION
#define DVLEDRX_VERSION "1.0.0"
#endif

/* Map a plain integer fps value to the correct MTL enum.
 * MTL uses enum st_fps for RTP timestamp spacing — passing a raw
 * integer (e.g. 30) instead of ST_FPS_P30 (==8) makes the receiver
 * expect the wrong timestamp cadence and triggers rv_slot_by_tmstamp
 * errors on every packet. */
static enum st_fps int_fps_to_st_fps(int fps) {
  switch (fps) {
    case 120: return ST_FPS_P120;
    case 100: return ST_FPS_P100;
    case 60:  return ST_FPS_P60;
    case 59:  return ST_FPS_P59_94;
    case 50:  return ST_FPS_P50;
    case 30:  return ST_FPS_P30;
    case 29:  return ST_FPS_P29_97;
    case 25:  return ST_FPS_P25;
    case 24:  return ST_FPS_P24;
    default:
      log_warn(LOGGER_TAG, "Unknown FPS %d, defaulting to ST_FPS_P25", fps);
      return ST_FPS_P25;
  }
}

/* Map a format name string (as used by FFmpeg -pix_fmt) to the MTL enum.
 * Accepts both the ffmpeg short name and the MTL enum name so the
 * value in the config feels natural. */
static enum st_frame_fmt str_fmt_to_st_fmt(const char* s) {
  if (!s) return ST_FRAME_FMT_YUV422PLANAR10LE;
  /* 10-bit formats */
  if (strcmp(s, "yuv422p10le") == 0) return ST_FRAME_FMT_YUV422PLANAR10LE;
  if (strcmp(s, "yuv444p10le") == 0) return ST_FRAME_FMT_YUV444PLANAR10LE;
  if (strcmp(s, "gbrp10le")    == 0) return ST_FRAME_FMT_GBRPLANAR10LE;
  /* 12-bit formats */
  if (strcmp(s, "yuv422p12le") == 0) return ST_FRAME_FMT_YUV422PLANAR12LE;
  if (strcmp(s, "yuv444p12le") == 0) return ST_FRAME_FMT_YUV444PLANAR12LE;
  if (strcmp(s, "gbrp12le")    == 0) return ST_FRAME_FMT_GBRPLANAR12LE;
  /* 8-bit / special formats */
  if (strcmp(s, "yuv420")      == 0) return ST_FRAME_FMT_YUV420CUSTOM8;
  /* Also accept raw integer for backward compatibility */
  char* end;
  long v = strtol(s, &end, 10);
  if (*end == '\0') return (enum st_frame_fmt)v;
  log_warn(LOGGER_TAG, "Unknown format '%s', defaulting to yuv422p10le", s);
  return ST_FRAME_FMT_YUV422PLANAR10LE;
}

/* Map an output frame format to ST2110-20 transport format. */
static enum st20_fmt st_fmt_to_transport_fmt(enum st_frame_fmt frame_fmt) {
  switch (frame_fmt) {
    case ST_FRAME_FMT_YUV422PLANAR10LE:
      return ST20_FMT_YUV_422_10BIT;
    case ST_FRAME_FMT_YUV422PLANAR12LE:
      return ST20_FMT_YUV_422_12BIT;
    case ST_FRAME_FMT_YUV444PLANAR10LE:
      return ST20_FMT_YUV_444_10BIT;
    case ST_FRAME_FMT_YUV444PLANAR12LE:
      return ST20_FMT_YUV_444_12BIT;
    case ST_FRAME_FMT_GBRPLANAR10LE:
      return ST20_FMT_RGB_10BIT;
    case ST_FRAME_FMT_GBRPLANAR12LE:
      return ST20_FMT_RGB_12BIT;
    case ST_FRAME_FMT_YUV420PLANAR8:
    case ST_FRAME_FMT_YUV420CUSTOM8:
      return ST20_FMT_YUV_420_8BIT;
    default:
      log_warn(LOGGER_TAG, "Unknown frame format %d, defaulting transport to YUV_422_10BIT", frame_fmt);
      return ST20_FMT_YUV_422_10BIT;
  }
}

/* Set by the signal handler; logged and acted on by the main loop (logging is not async-signal-safe). */
static volatile sig_atomic_t g_stop_signal = 0;

static void rx_app_sig_handler(int sig) {
  g_stop_signal = sig;
}

static const char* signal_name(int sig) {
  return sig == SIGINT ? "SIGINT(Ctrl+C)" : sig == SIGTERM ? "SIGTERM" :
         sig == SIGQUIT ? "SIGQUIT(Ctrl+\\)" : "UNKNOWN";
}

static void print_help(const char* prog_name) {
  log_info(LOGGER_TAG, "Usage: %s --config <file> [options]", prog_name);
  log_info(LOGGER_TAG, "Options:");
  log_info(LOGGER_TAG, "  -C, --config <file>       JSON config file (required)");
  log_info(LOGGER_TAG, "  -t, --test-time <secs>    Receive for N seconds then exit (1..86400)");
  log_info(LOGGER_TAG, "  -v, --version             Show version");
  log_info(LOGGER_TAG, "  --help                    Show this help");
}

/* Same options as dvledtx; everything else comes from the JSON config. */
static int parse_args(struct rx_app_context* app, int argc, char** argv) {
  static struct option long_options[] = {
    {"config",    required_argument, 0, 'C'},
    {"test-time", required_argument, 0, 't'},
    {"version",   no_argument,       0, 'v'},
    {"help",      no_argument,       0, '?'},
    {0, 0, 0, 0}
  };

  app->config_file[0] = '\0';

  int c = 0, option_index = 0;
  while ((c = getopt_long(argc, argv, "C:t:v?", long_options, &option_index)) != -1) {
    switch (c) {
      case 'C':
        snprintf(app->config_file, sizeof(app->config_file), "%s", optarg);
        break;
      case 't': {
        char* endptr = NULL;
        long val = strtol(optarg, &endptr, 10);
        if (endptr == optarg || *endptr != '\0' || val <= 0 || val > 86400) {
          log_error(LOGGER_TAG, "Invalid --test-time value '%s' (expected 1..86400)", optarg);
          return -1;
        }
        app->test_time_s = (int)val;
        break;
      }
      case 'v':
        printf("dvledrx version %s\n", DVLEDRX_VERSION);
        exit(EXIT_SUCCESS);
      case '?':
      default:
        print_help(argv[0]);
        return -1;
    }
  }

  if (app->config_file[0] == '\0') {
    print_help(argv[0]);
    return -1;
  }
  return 0;
}

/* Main application */
int main(int argc, char** argv) {
  struct rx_app_context app;
  session_manager_t session_manager;
  int ret = 0;

  memset(&app, 0, sizeof(app));
  /* Must precede every other Xlib call: display, RX and main threads share Display* handles. */
  XInitThreads();
  logger_init_from_env();

  /* Parse arguments */
  if (parse_args(&app, argc, argv) < 0) {
    return -1;
  }

  if (load_and_validate_rx_config(app.config_file, &app.json_config) != 0) {
    return -1;
  }
  const struct rx_config* cfg = &app.json_config;

  /* Video parameters */
  app.width = (uint32_t)cfg->video.width;
  app.height = (uint32_t)cfg->video.height;
  app.fps = int_fps_to_st_fps(cfg->video.fps);
  app.fmt = cfg->video.fmt[0] != '\0' ? str_fmt_to_st_fmt(cfg->video.fmt)
                                      : ST_FRAME_FMT_YUV422PLANAR10LE;
  app.transport_fmt = st_fmt_to_transport_fmt(app.fmt);

  /* 1 session -> single window; N sessions -> composite, or per_output windows. */
  app.st20p_sessions = cfg->num_rx_sessions;

  log_info(LOGGER_TAG, "JSON config: %d rx_session(s) -> %s display mode",
           cfg->num_rx_sessions,
           strcmp(cfg->display_mode, "per_output") == 0 ? "per_output" :
           cfg->num_rx_sessions > 1 ? "composite" : "single");

  if (cfg->log_level[0] != '\0') {
    enum log_level level;
    if (logger_parse_level(cfg->log_level, &level)) {
      logger_set_level(level);
      log_info(LOGGER_TAG, "Log level set to: %s", cfg->log_level);
    } else {
      log_warn(LOGGER_TAG, "Invalid log_level '%s' in config (use debug|info|warn|error)", cfg->log_level);
    }
  }

  /* Redirect logs to file when configured (empty = console) */
  if (cfg->log_file[0] != '\0') {
    logger_set_file(cfg->log_file, LOGGER_MAX_BYTES);
    log_info(LOGGER_TAG, "Logging to file: %s (rotates at %u MB, keeps %d .gz archives)",
             cfg->log_file, LOGGER_MAX_BYTES / (1024u * 1024u), LOGGER_MAX_ARCHIVES);
  }

  /* Install signal handler */
  signal(SIGINT, rx_app_sig_handler);
  signal(SIGTERM, rx_app_sig_handler);
  signal(SIGQUIT, rx_app_sig_handler);  /* Ctrl+\\ */

  log_info(LOGGER_TAG, "Signal handlers installed. Press Ctrl+C to stop.");

  /* Initialize session manager */
  if (session_manager_init(&session_manager, &app) < 0) {
    log_error(LOGGER_TAG, "Failed to initialize session manager");
    return -1;
  }

  /* Start all sessions */
  if (session_manager_start(&session_manager, &app) < 0) {
    log_error(LOGGER_TAG, "Failed to start sessions");
    app.exit = true; /* stop the sessions that did start */
    ret = -1;
    goto cleanup;
  }

  if (app.test_time_s > 0) {
    log_info(LOGGER_TAG, "Running for %d seconds...", app.test_time_s);
  } else {
    log_info(LOGGER_TAG, "Running indefinitely (press Ctrl+C to stop)...");
  }

  /* Main loop: pump X11 events until Ctrl+C, a window close, or --test-time. */
  time_t start_time = time(NULL);
  while (!app.exit) {
    if (session_manager.composite) {
      process_composite_x11_events(session_manager.composite);
    } else {
      for (int i = 0; i < session_manager.st20p_count; i++) {
        struct st20p_rx_ctx* ctx = &session_manager.st20p_sessions[i];
        if (ctx->display.initialized) process_x11_events(&ctx->display);
        for (int m = 0; m < ctx->num_mirror_displays; m++)
          process_x11_events(&ctx->mirror_display[m]);
      }
    }

    if (x11_close_requested()) app.exit = true;
    if (g_stop_signal) {
      log_info(LOGGER_TAG, "Signal received: %d (%s)", (int)g_stop_signal, signal_name(g_stop_signal));
      app.exit = true;
    }
    if (app.test_time_s > 0 && time(NULL) - start_time >= app.test_time_s) {
      log_info(LOGGER_TAG, "Test duration completed");
      app.exit = true;
    }

    usleep(10000); /* 10ms: responsive to signals and window events */
  }

  log_info(LOGGER_TAG, "Stopping application...");

cleanup:
  /* Cleanup session manager */
  session_manager_cleanup(&session_manager);

  log_info(LOGGER_TAG, "Application stopped");
  logger_close();
  return ret;
}
