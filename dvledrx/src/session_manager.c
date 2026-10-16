/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "session_manager.h"
#include "x11_handler.h"
#include "frame_converter.h"
#include "config_reader.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <strings.h>
#include <time.h>
#include <arpa/inet.h>

#define LOGGER_TAG "session"

static bool is_valid_pci_bdf(const char* pci_bdf);

static bool is_pci_virtual_function(const char* pci_bdf) {
  char path[256];

  if (!is_valid_pci_bdf(pci_bdf)) {
    return false;
  }

  snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/physfn", pci_bdf);
  return (access(path, F_OK) == 0);
}

static bool is_pci_bound_to_vfio(const char* pci_bdf) {
  char path[256];
  char link_target[256];
  ssize_t len;

  if (!is_valid_pci_bdf(pci_bdf)) {
    return false;
  }

  snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/driver", pci_bdf);
  len = readlink(path, link_target, sizeof(link_target) - 1);
  if (len <= 0) {
    return false;
  }

  link_target[len] = '\0';
  return strstr(link_target, "vfio-pci") != NULL;
}

/* Validate a PCI BDF string (e.g. "0000:03:10.3" or "03:10.3") so it is safe
 * to embed in sysfs paths and shell commands. Only the canonical forms
 * "dddd:bb:dd.f" (4-digit domain) and "bb:dd.f" (domain omitted) are
 * accepted, with function restricted to 0-7. Reformatting the parsed fields
 * and comparing back to the input rejects anything with the wrong digit
 * width (e.g. ".." or truncated/extended fields), which would otherwise
 * allow directory traversal in sysfs paths. */
static bool is_valid_pci_bdf(const char* pci_bdf) {
  unsigned domain, bus, dev, func;
  int consumed;
  char canonical[32];

  if (!pci_bdf || !pci_bdf[0]) {
    return false;
  }

  consumed = -1;
  if (sscanf(pci_bdf, "%4x:%2x:%2x.%1x%n", &domain, &bus, &dev, &func, &consumed) == 4 &&
      consumed > 0 && pci_bdf[consumed] == '\0' && func <= 7) {
    snprintf(canonical, sizeof(canonical), "%04x:%02x:%02x.%01x", domain, bus, dev, func);
    return strcasecmp(canonical, pci_bdf) == 0;
  }

  consumed = -1;
  if (sscanf(pci_bdf, "%2x:%2x.%1x%n", &bus, &dev, &func, &consumed) == 3 &&
      consumed > 0 && pci_bdf[consumed] == '\0' && func <= 7) {
    snprintf(canonical, sizeof(canonical), "%02x:%02x.%01x", bus, dev, func);
    return strcasecmp(canonical, pci_bdf) == 0;
  }

  return false;
}

/*
 * Prepare the parent PF of an SR-IOV VF for multicast RX.
 *
 * ixgbe (and similar) NICs reject a VF's DPDK promiscuous request unless the
 * parent PF is already in promiscuous mode (kernel: "Enabling VF promisc
 * requires PF in promisc"). MTL requests promiscuous once at init and never
 * retries, so we must configure the PF *before* mtl_init():
 *   - PF: promiscuous on, link up
 *   - VF: trust on, spoof checking off
 *
 * The VF itself is bound to vfio-pci (no kernel netdev), but the PF still has a
 * kernel netdev we can drive via `ip link`. Requires CAP_NET_ADMIN (run as root).
 * Best-effort: logs a warning and returns on any failure.
 */
static void prepare_vf_parent_pf(const char* vf_bdf) {
  char path[512];
  char link_target[512];
  ssize_t len;
  char pf_bdf[64];
  char pf_iface[64] = {0};
  int vf_index = -1;
  DIR* dir;
  struct dirent* ent;
  char cmd[512];

  if (!is_valid_pci_bdf(vf_bdf)) {
    log_warn(LOGGER_TAG, "VF setup: invalid PCI BDF '%s', skipping PF promisc setup", vf_bdf);
    return;
  }

  /* Resolve parent PF BDF via /sys/bus/pci/devices/<vf>/physfn */
  snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/physfn", vf_bdf);
  len = readlink(path, link_target, sizeof(link_target) - 1);
  if (len <= 0) {
    log_warn(LOGGER_TAG, "VF setup: cannot resolve parent PF for %s", vf_bdf);
    return;
  }
  link_target[len] = '\0';
  {
    const char* base = strrchr(link_target, '/');
    snprintf(pf_bdf, sizeof(pf_bdf), "%s", base ? base + 1 : link_target);
  }

  /* Find the PF kernel netdev name via /sys/bus/pci/devices/<pf>/net/ */
  snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/net", pf_bdf);
  dir = opendir(path);
  if (dir) {
    while ((ent = readdir(dir)) != NULL) {
      if (ent->d_name[0] == '.') continue;
      snprintf(pf_iface, sizeof(pf_iface), "%s", ent->d_name);
      break;
    }
    closedir(dir);
  }
  if (!pf_iface[0]) {
    log_warn(LOGGER_TAG, "VF setup: cannot find PF netdev for %s (PF %s)", vf_bdf, pf_bdf);
    return;
  }

  /* Determine VF index by matching virtfn* symlinks under the PF */
  snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s", pf_bdf);
  dir = opendir(path);
  if (dir) {
    while ((ent = readdir(dir)) != NULL) {
      if (strncmp(ent->d_name, "virtfn", 6) != 0) continue;
      snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/%s", pf_bdf, ent->d_name);
      len = readlink(path, link_target, sizeof(link_target) - 1);
      if (len <= 0) continue;
      link_target[len] = '\0';
      const char* base = strrchr(link_target, '/');
      base = base ? base + 1 : link_target;
      if (strcmp(base, vf_bdf) == 0) {
        vf_index = atoi(ent->d_name + 6);
        break;
      }
    }
    closedir(dir);
  }
  if (vf_index < 0) {
    log_warn(LOGGER_TAG, "VF setup: cannot resolve VF index for %s under PF %s", vf_bdf, pf_bdf);
    return;
  }

  log_info(LOGGER_TAG,
           "VF setup: configuring PF %s (%s) promisc + vf %d trust on / spoofchk off",
           pf_bdf, pf_iface, vf_index);

  /* PF promiscuous mode (required before VF promisc is honored) */
  for (const char* p = pf_iface; *p; p++) {
    if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.') {
      log_warn(LOGGER_TAG, "VF setup: unsafe PF netdev name '%s', skipping ip link setup", pf_iface);
      return;
    }
  }
  snprintf(cmd, sizeof(cmd), "ip link set %s promisc on", pf_iface);
  if (system(cmd) != 0) {
    log_warn(LOGGER_TAG, "VF setup: '%s' failed (need root/CAP_NET_ADMIN?)", cmd);
  }
  /* VF trust on */
  snprintf(cmd, sizeof(cmd), "ip link set %s vf %d trust on", pf_iface, vf_index);
  if (system(cmd) != 0) {
    log_warn(LOGGER_TAG, "VF setup: '%s' failed", cmd);
  }
  /* VF spoof checking off (allows promiscuous/multicast reception) */
  snprintf(cmd, sizeof(cmd), "ip link set %s vf %d spoofchk off", pf_iface, vf_index);
  if (system(cmd) != 0) {
    log_warn(LOGGER_TAG, "VF setup: '%s' failed", cmd);
  }
  /* Bring the PF link up; RxTxApp's nicctl.sh does this so the VF admin queue
   * works for DPDK init and traffic reaches the VF. */
  snprintf(cmd, sizeof(cmd), "ip link set %s up", pf_iface);
  if (system(cmd) != 0) {
    log_warn(LOGGER_TAG, "VF setup: '%s' failed", cmd);
  }
}

/* "per_output": one fullscreen window per session, each on its own HDMI/DP output. */
static bool is_per_output_mode(const struct rx_app_context* app) {
  return strcmp(app->json_config.display_mode, "per_output") == 0;
}

/*
 * st20p_display_thread - dedicated rendering thread (single-session mode).
 *
 * Takes the newest MTL frame handed over by the RX thread, renders it, then
 * returns it to MTL.  Runs best-effort: if rendering cannot keep up, the RX
 * thread returns superseded frames unrendered — the receive pipeline never waits.
 */
static void* st20p_display_thread(void* arg) {
  struct st20p_rx_ctx* ctx = (struct st20p_rx_ctx*)arg;

  log_info(LOGGER_TAG, "ST20P display thread %d started", ctx->idx);

  char dip[INET_ADDRSTRLEN] = "";
  inet_ntop(AF_INET, ctx->dip_addr, dip, sizeof(dip));
  char rx_label[96];
  snprintf(rx_label, sizeof(rx_label), "%s %s:%u", ctx->port_name, dip, ctx->session_udp_port);

  /* Display-rate measurement: count frames actually blitted and report the
   * achieved render fps once per second.  This is the true on-screen rate,
   * independent of the MTL receive rate. */
  struct timespec last;
  clock_gettime(CLOCK_MONOTONIC, &last);
  int rendered = 0;

  while (!ctx->app->exit) {
    struct st_frame* frame = NULL;

    pthread_mutex_lock(&ctx->disp_lock);
    while (!ctx->disp_pending && !ctx->app->exit) {
      /* Timed wait so the window stays responsive and exit is observed
       * promptly even when no new frames are arriving. */
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 20 * 1000 * 1000; /* 20ms */
      if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
      }
      pthread_cond_timedwait(&ctx->disp_cond, &ctx->disp_lock, &ts);
    }
    frame = ctx->disp_pending;
    ctx->disp_pending = NULL;
    pthread_mutex_unlock(&ctx->disp_lock);

    if (frame) {
      display_frame_x11(&ctx->display, frame);
      for (int m = 0; m < ctx->num_mirror_displays; m++)
        if (!display_copy_x11(&ctx->mirror_display[m], &ctx->display))
          display_frame_x11(&ctx->mirror_display[m], frame);
      rendered++;
      /* All puts go through disp_lock so RX and display threads never race in MTL. */
      pthread_mutex_lock(&ctx->disp_lock);
      st20p_rx_put_frame(ctx->handle, frame);
      pthread_mutex_unlock(&ctx->disp_lock);
    }
    /* X11 events are pumped by the main thread to avoid concurrent event handling. */

    /* Report achieved display fps once per second. */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
    if (elapsed >= 1.0) {
      int rx = __atomic_exchange_n(&ctx->frames_rx_window, 0, __ATOMIC_RELAXED);
      pthread_mutex_lock(&ctx->disp_lock);
      int skipped = ctx->disp_skipped;
      ctx->disp_skipped = 0;
      pthread_mutex_unlock(&ctx->disp_lock);

      char outputs[160] = "";
      int n = 0;
      const struct x11_display_ctx* wins[1 + MAX_MIRRORS] = {&ctx->display};
      for (int m = 0; m < ctx->num_mirror_displays; m++) wins[1 + m] = &ctx->mirror_display[m];
      for (int w = 0; w <= ctx->num_mirror_displays && (size_t)n < sizeof(outputs); w++) {
        if (!wins[w]->initialized) continue;
        n += snprintf(outputs + n, sizeof(outputs) - (size_t)n, "%s%s", n ? ", " : "",
                      wins[w]->output_name);
      }

      log_info(LOGGER_TAG, "Session %d [%s]: rx %.2f fps, display %.2f fps, skipped %d -> %s",
               ctx->idx, rx_label, rx / elapsed, rendered / elapsed, skipped, outputs);
      rendered = 0;
      last = now;
    }
  }

  log_info(LOGGER_TAG, "ST20P display thread %d stopped", ctx->idx);
  return NULL;
}

/*
 * publish_frame_to_display - hand a received MTL frame to the display thread
 * without copying it.  The display thread returns it to MTL after rendering;
 * if a previous frame is still waiting unrendered, it is returned here.
 * Returns false (caller keeps ownership) when the frame has no data.
 */
static bool publish_frame_to_display(struct st20p_rx_ctx* ctx, struct st_frame* frame) {
  if (!frame->addr[0] || frame->data_size == 0) {
    return false;
  }

  pthread_mutex_lock(&ctx->disp_lock);
  struct st_frame* stale = ctx->disp_pending;
  ctx->disp_pending = frame;
  if (stale) {
    st20p_rx_put_frame(ctx->handle, stale);
    ctx->disp_skipped++;
  }
  pthread_cond_signal(&ctx->disp_cond);
  pthread_mutex_unlock(&ctx->disp_lock);
  return true;
}

/*
 * st20p_rx_thread - per-session ST20P (video) receive thread.
 *
 * Blocks on st20p_rx_get_frame() until MTL delivers a decoded video frame.
 * For each frame received it:
 *   1. Logs detailed metadata for the first few frames (format/size sanity check).
 *   2. Renders the frame to the display:
 *        - Single-session mode : directly into a dedicated X11 window.
 *        - Multi-session mode  : writes a strip into the shared composite buffer.
 *   3. Returns the frame buffer to the MTL library (mandatory — frees the slot).
 *
 * Exits cleanly once app->exit is set (e.g. by the SIGINT handler).
 */
static void* st20p_rx_thread(void* arg) {
  struct st20p_rx_ctx* ctx = (struct st20p_rx_ctx*)arg;
  struct st_frame* frame;

  log_info(LOGGER_TAG, "ST20P RX thread %d started - waiting for incoming frames", ctx->idx);

  /* Single-session display runs on a dedicated thread so the heavy YUV->RGB
   * conversion and X11 blit never block the receive loop.  Composite
   * (multi-session) mode keeps its existing in-loop barrier-synchronised path.
   * Mirrors count as displays: the primary output may be disconnected. */
  bool has_window = ctx->display.initialized;
  for (int m = 0; m < ctx->num_mirror_displays; m++)
    has_window |= ctx->mirror_display[m].initialized;
  bool use_disp_thread = !ctx->composite && has_window;
  if (use_disp_thread) {
    pthread_mutex_init(&ctx->disp_lock, NULL);
    pthread_cond_init(&ctx->disp_cond, NULL);
    ctx->disp_pending = NULL;
    if (pthread_create(&ctx->disp_thread, NULL, st20p_display_thread, ctx) == 0) {
      ctx->disp_thread_started = true;
    } else {
      log_error(LOGGER_TAG, "Failed to start display thread %d; rendering inline", ctx->idx);
      pthread_mutex_destroy(&ctx->disp_lock);
      pthread_cond_destroy(&ctx->disp_cond);
      use_disp_thread = false;
    }
  }

  while (!ctx->app->exit) {
    frame = st20p_rx_get_frame(ctx->handle);
    if (!frame) {
      usleep(1000); /* 1ms — avoid busy-spinning when no frame is ready */
      continue;
    }

    ctx->frames_received++;
    __atomic_fetch_add(&ctx->frames_rx_window, 1, __ATOMIC_RELAXED);
    bool handed_off = false;

    /* --- Diagnostic: log frame metadata for the first few frames ----------
     * Helps verify that sender format/resolution matches the MTL RX output. */
    /* DEBUG: Print detailed frame information for first few frames */
    if (ctx->frames_received <= 5) {
      log_debug(LOGGER_TAG, "ST20P RX session %d: Frame %d received", ctx->idx, ctx->frames_received);
      log_debug(LOGGER_TAG, "  Format: %d, Size: %zu bytes", frame->fmt, frame->data_size);
      log_debug(LOGGER_TAG, "  Width: %d, Height: %d", frame->width, frame->height);
      log_debug(LOGGER_TAG, "  Addresses: Y=%p, U=%p, V=%p", frame->addr[0], frame->addr[1], frame->addr[2]);
      log_debug(LOGGER_TAG, "  Linesizes: Y=%zu, U=%zu, V=%zu", frame->linesize[0], frame->linesize[1], frame->linesize[2]);
    }

    /* Render the frame.
     * Two render paths — selected by whether a composite context was wired in:
     *
     * ctx->composite != NULL  (multi-session / legacy single-machine mode):
     *   Copy this session's strip into the shared composite buffer and wait
     *   at the pthread_barrier so all strips are present before session 0
     *   flushes the full frame to the screen.
     *
     * ctx->composite == NULL  (single-session / standard wall deployment):
     *   Render the complete 640x1080 frame directly into this receiver's own
     *   X11 window.  No synchronisation with other machines is needed — each
     *   receiver is independent. */
    if (ctx->composite) {
      /* Multi-session: write strip/crop and synchronise with peer sessions */
      if (ctx->crop.w > 0 && ctx->crop.h > 0) {
        /* Crop-aware composite: place frame at specific coordinates */
        composite_write_crop(ctx->composite, frame, &ctx->crop);
      } else {
        /* Legacy equal-width strip composite */
        composite_write_strip(ctx->composite, frame, ctx->idx);
      }
    } else if (has_window) {
      /* Single-session: hand the frame to the dedicated display thread,
       * which returns it to MTL after rendering, so receive never stalls. */
      if (use_disp_thread) {
        handed_off = publish_frame_to_display(ctx, frame);
      } else {
        /* Fallback: display thread unavailable, render inline. */
        display_frame_x11(&ctx->display, frame);
        for (int m = 0; m < ctx->num_mirror_displays; m++)
          if (!display_copy_x11(&ctx->mirror_display[m], &ctx->display))
            display_frame_x11(&ctx->mirror_display[m], frame);
        process_x11_events(&ctx->display);
        for (int m = 0; m < ctx->num_mirror_displays; m++)
          process_x11_events(&ctx->mirror_display[m]);
      }
    }

    /* Return the frame buffer to MTL so it can be reused for the next frame.
     * Frames handed to the display thread are returned by it instead. */
    if (!handed_off) st20p_rx_put_frame(ctx->handle, frame);

    /* Periodic progress log — printed every 50 frames to avoid console spam */
    if (ctx->frames_received % 50 == 0) {
      log_info(LOGGER_TAG, "ST20P session %d: received %d frames",
               ctx->idx, ctx->frames_received);
    }
  }

  log_info(LOGGER_TAG, "ST20P RX thread %d stopped, received %d frames", ctx->idx, ctx->frames_received);

  /* Stop and clean up the dedicated display thread (single-session mode). */
  if (ctx->disp_thread_started) {
    pthread_mutex_lock(&ctx->disp_lock);
    pthread_cond_signal(&ctx->disp_cond); /* wake it so it observes app->exit */
    pthread_mutex_unlock(&ctx->disp_lock);
    pthread_join(ctx->disp_thread, NULL);
    ctx->disp_thread_started = false;
    if (ctx->disp_pending) {
      st20p_rx_put_frame(ctx->handle, ctx->disp_pending);
      ctx->disp_pending = NULL;
    }
    pthread_mutex_destroy(&ctx->disp_lock);
    pthread_cond_destroy(&ctx->disp_cond);
  }

  return NULL;
}

/*
 * get_transport_format - map a decoded pixel format to the ST2110-20
 * on-wire transport format.
 *
 * MTL requires callers to specify the compressed transport packing separately
 * from the decoded output format.  This helper centralises that mapping so
 * it doesn't have to be repeated at each session creation call site.
 */
static enum st20_fmt get_transport_format(enum st_frame_fmt frame_fmt) {
  switch (frame_fmt) {
    case ST_FRAME_FMT_YUV422PLANAR10LE:
      return ST20_FMT_YUV_422_10BIT;
    case ST_FRAME_FMT_YUV420PLANAR8:
      return ST20_FMT_YUV_420_8BIT;
    case ST_FRAME_FMT_YUV420CUSTOM8:
      return ST20_FMT_YUV_420_8BIT;
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
    default:
      log_warn(LOGGER_TAG, "Unknown frame format %d, using YUV_422_10BIT", frame_fmt);
      return ST20_FMT_YUV_422_10BIT;
  }
}

/* Create ST20P session */
static int create_st20p_session(struct rx_app_context* app, struct st20p_rx_ctx* ctx, int session_idx) {
  struct st20p_rx_ops ops;
  bool vf_mode = is_pci_virtual_function(ctx->port_name);

  memset(&ops, 0, sizeof(ops));

  /* Session identification */
  snprintf(ctx->session_name, sizeof(ctx->session_name), "st20p_rx_%d", session_idx);
  ops.name = ctx->session_name;
  ops.priv = ctx;

  /* Network configuration (from the session's JSON rx_sessions entry). */
  ops.port.num_port = 1;
  memcpy(ops.port.ip_addr[MTL_SESSION_PORT_P], ctx->dip_addr, MTL_IP_ADDR_LEN);
  snprintf(ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s", ctx->port_name);
  ops.port.udp_port[MTL_SESSION_PORT_P] = ctx->session_udp_port;
  ops.port.payload_type = ctx->session_payload_type;

  /* Video parameters.
   * When a crop region is defined, the stream carries crop.w × crop.h. */
  if (ctx->crop.w > 0 && ctx->crop.h > 0) {
    ops.width  = (uint32_t)ctx->crop.w;
    ops.height = (uint32_t)ctx->crop.h;
  } else {
    ops.width  = app->width;
    ops.height = app->height;
  }
  ops.fps = app->fps;
  ops.transport_fmt = (app->transport_fmt != 0)
                        ? (enum st20_fmt)app->transport_fmt
                        : get_transport_format(app->fmt);
  ops.output_fmt = app->fmt;

  /* Device and flags */
  ops.device = ST_PLUGIN_DEVICE_AUTO;
  ops.framebuff_cnt = 8;
  ops.flags = ST20P_RX_FLAG_BLOCK_GET | ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;

  /* Create RX handle */
  ctx->handle = st20p_rx_create(app->mtl, &ops);
  if (!ctx->handle) {
    log_error(LOGGER_TAG, "Failed to create ST20P RX session %d", session_idx);
    log_error(LOGGER_TAG, "Possible causes: insufficient RX queues, network configuration, or DPDK setup");
    if (vf_mode) {
      log_error(LOGGER_TAG,
                "Detected PCI VF on %s. Current MTL build may not support rte_flow for ixgbe VF.",
                ctx->port_name);
      log_error(LOGGER_TAG,
                "Use PF/iavf-capable NIC path, or rebuild/install MTL with explicit net_ixgbe_vf no-flow handling.");
    }
    log_error(LOGGER_TAG, "Check MTL logs above for detailed error information");
    return -1;
  }

  ctx->frame_size = st20p_rx_frame_size(ctx->handle);
  log_info(LOGGER_TAG, "ST20P RX session %d created, frame size: %zu bytes", session_idx, ctx->frame_size);

  /* Display initialisation — two mutually exclusive code paths:
   *
   * Single-session (st20p_sessions == 1):
   *   Normal deployment on a 3-receiver wall.  Each receiver process has
   *   exactly one ST20P session and opens its own dedicated X11 window
   *   sized to the panel resolution (e.g. 640x1080).  Frames are rendered
   *   directly via display_frame_x11() in st20p_rx_thread().
   *
   * Multi-session (st20p_sessions > 1):
   *   Legacy single-machine composite mode.  A shared composite_x11_ctx is
   *   allocated in session_manager_start() and wired into every session's
   *   ctx->composite pointer.  Each thread writes its strip via
   *   composite_write_strip() and a pthread_barrier synchronises the flush.
   *   The per-session x11_display_ctx is not used in this path.
   *
   * display_mode "per_output" (any session count):
   *   Each session gets its own borderless fullscreen window on the XRandR
   *   output named in rx_sessions[i].output, or the i-th active monitor.
   *   rx_sessions[i].window switches it to a fixed-size window on that output. */
  bool per_output = is_per_output_mode(app);
  if (app->st20p_sessions == 1 || per_output) {
    /* For single-session: use crop dimensions if available, else full video */
    uint32_t disp_w = (ctx->crop.w > 0) ? (uint32_t)ctx->crop.w : app->width;
    uint32_t disp_h = (ctx->crop.h > 0) ? (uint32_t)ctx->crop.h : app->height;
    const char* output_name = NULL;
    struct crop_config window_rect;
    const struct crop_config* window_ptr = NULL;
    if (session_idx < app->json_config.num_rx_sessions) {
      const struct rx_session_config* sc = &app->json_config.rx_sessions[session_idx];
      bool has_scale = sc->scale_w > 0 && sc->scale_h > 0;
      output_name = sc->output;
      if (has_scale) {
        disp_w = (uint32_t)sc->scale_w;
        disp_h = (uint32_t)sc->scale_h;
      }
      if (sc->has_window || has_scale) {
        /* window gives the position; scale (if set) gives the size. */
        window_rect = sc->has_window ? sc->window : (struct crop_config){0, 0, 0, 0};
        if (has_scale) {
          window_rect.w = sc->scale_w;
          window_rect.h = sc->scale_h;
        }
        if (window_rect.w == 0) window_rect.w = (int)disp_w;
        if (window_rect.h == 0) window_rect.h = (int)disp_h;
        window_ptr = &window_rect;
      }
    }
    if (init_x11_display(&ctx->display, disp_w, disp_h, session_idx,
                         output_name, per_output, window_ptr) < 0) {
      log_warn(LOGGER_TAG, "Failed to initialize X11 display for session %d", session_idx);
      /* Continue without display */
    }

    if (per_output && session_idx < app->json_config.num_rx_sessions) {
      const struct rx_session_config* sc = &app->json_config.rx_sessions[session_idx];
      for (int m = 0; m < sc->num_mirrors; m++) {
        const struct display_placement* pl = &sc->mirrors[m];
        struct crop_config rect = pl->window;
        if (pl->scale_w > 0 && pl->scale_h > 0) {
          rect.w = pl->scale_w;
          rect.h = pl->scale_h;
        }
        if (rect.w == 0) rect.w = (ctx->crop.w > 0) ? ctx->crop.w : (int)app->width;
        if (rect.h == 0) rect.h = (ctx->crop.h > 0) ? ctx->crop.h : (int)app->height;
        struct x11_display_ctx* md = &ctx->mirror_display[ctx->num_mirror_displays];
        if (init_x11_display(md, (uint32_t)rect.w, (uint32_t)rect.h, session_idx,
                             pl->output, true, &rect) == 0) {
          ctx->num_mirror_displays++;
        } else {
          log_warn(LOGGER_TAG, "Failed to mirror session %d on %s", session_idx, pl->output);
        }
      }
    }
  }

  return 0;
}

/*
 * session_manager_init - initialise the MTL library and prepare the session
 * manager structure.
 *
 * Binds MTL to every NIC in the JSON "interfaces" array, each with its own
 * source IP, and provisions RX queues per NIC for the sessions on it plus a
 * small reserve for internal MTL use.
 *
 * Must be called before session_manager_start().
 * Returns 0 on success, -1 on failure.
 */
int session_manager_init(session_manager_t* manager, struct rx_app_context* app) {
  struct mtl_init_params mtl_params;
  bool any_vf = false;
  int num_ports;
  uint16_t sessions_on_port[MTL_PORT_MAX] = {0};

  if (!manager || !app) {
    return -1;
  }

  memset(manager, 0, sizeof(*manager));

  /* One MTL port per JSON interface (MTL port index == position in the array). */
  const struct rx_config* cfg = &app->json_config;
  num_ports = cfg->num_interfaces;

  for (int i = 0; i < app->st20p_sessions; i++) {
    int p = rx_config_port_for_nic(cfg, cfg->rx_sessions[i].nic_index);
    if (p >= 0 && p < num_ports) sessions_on_port[p]++;
  }

  memset(&mtl_params, 0, sizeof(mtl_params));
  mtl_params.num_ports = (uint8_t)num_ports;
  mtl_params.log_level = MTL_LOG_LEVEL_INFO;
  mtl_params.flags |= MTL_FLAG_DEV_AUTO_START_STOP;
  mtl_params.flags |= MTL_FLAG_RX_VIDEO_MIGRATE;
  if (cfg->tasklet_sleep) {
    /* Scheduler sleeps when idle; RX sessions cap it at ~128 packet times. */
    mtl_params.flags |= MTL_FLAG_TASKLET_SLEEP;
    log_info(LOGGER_TAG, "MTL tasklet sleep enabled");
  }
  mtl_params.nb_rx_desc = 4096;

  for (int p = 0; p < num_ports; p++) {
    const char* name = cfg->interfaces[p].name;
    uint8_t sip[MTL_IP_ADDR_LEN];
    inet_pton(AF_INET, cfg->interfaces[p].sip, sip); /* validated at load */

    snprintf(mtl_params.port[p], MTL_PORT_MAX_LEN, "%s", name);
    memcpy(mtl_params.sip_addr[p], sip, MTL_IP_ADDR_LEN);
    mtl_params.pmd[p] = mtl_pmd_by_port_name(name);

    bool vf = is_pci_virtual_function(name);
    if (vf) {
      any_vf = true;
      log_warn(LOGGER_TAG, "VF NIC detected on %s%s", name,
               is_pci_bound_to_vfio(name) ? " (vfio-pci)" : "");
      /* ixgbe rejects the VF's promiscuous request unless the parent PF is
       * already promiscuous, so prepare it before mtl_init(). */
      prepare_vf_parent_pf(name);
    }

    /* One RX queue per session + 2 for MTL housekeeping (ARP, PTP, ...).
     * VF/CNI mode uses only the system queue; extra queues would let RSS
     * scatter packets away from it. */
    uint16_t rx_queues = vf ? 0 : (uint16_t)(sessions_on_port[p] + 2);
    mtl_para_rx_queues_cnt_set(&mtl_params, (enum mtl_port)p, rx_queues);

    log_info(LOGGER_TAG, "MTL port %d: %s sip %s, %u session(s), %u RX queues%s",
             p, name, cfg->interfaces[p].sip,
             sessions_on_port[p], rx_queues, vf ? " (VF)" : "");
  }

  if (any_vf) {
    /* Mirror RxTxApp VF-friendly path: force CNI RX instead of per-session rte_flow. */
    mtl_params.flags |= MTL_FLAG_RX_USE_CNI;
    mtl_params.flags |= MTL_FLAG_RX_UDP_PORT_ONLY;
    mtl_params.flags |= MTL_FLAG_NIC_RX_PROMISCUOUS;
    log_warn(LOGGER_TAG, "VF NIC(s) present: enabling rx_use_cni + rx_udp_port_only + promiscuous");
  }

  manager->mtl = mtl_init(&mtl_params);
  if (!manager->mtl) {
    log_error(LOGGER_TAG, "MTL initialization failed");
    return -1;
  }

  app->mtl = manager->mtl; /* Back-fill app context so session creators can reach the handle */

  log_info(LOGGER_TAG, "MTL initialized successfully with %d port(s)", num_ports);
  log_info(LOGGER_TAG, "Video format: %dx%d, FPS: %d", app->width, app->height,
           (app->fps == ST_FPS_P25) ? 25 : (app->fps == ST_FPS_P30) ? 30 :
           (app->fps == ST_FPS_P50) ? 50 : 60);
  log_info(LOGGER_TAG, "ST20P sessions: %d", app->st20p_sessions);

  manager->st20p_count = app->st20p_sessions;

  return 0;
}

static const struct x11_display_ctx* session_window(const struct st20p_rx_ctx* s, int w) {
  return w == 0 ? &s->display : &s->mirror_display[w - 1];
}

/* One block per display output listing every session window placed on it. */
static void log_display_layout(const session_manager_t* manager) {
  const char* seen[MAX_RX_SESSIONS * (MAX_MIRRORS + 1)];
  int nseen = 0;

  log_info(LOGGER_TAG, "Display layout:");
  for (int i = 0; i < manager->st20p_count; i++) {
    const struct st20p_rx_ctx* s = &manager->st20p_sessions[i];
    for (int w = 0; w <= s->num_mirror_displays; w++) {
      const struct x11_display_ctx* d = session_window(s, w);
      if (!d->initialized) continue;
      bool dup = false;
      for (int k = 0; k < nseen && !dup; k++) dup = strcmp(seen[k], d->output_name) == 0;
      if (dup || nseen >= (int)(sizeof(seen) / sizeof(seen[0]))) continue;
      seen[nseen++] = d->output_name;

      log_info(LOGGER_TAG, "  Output %s:", d->output_name);
      for (int j = 0; j < manager->st20p_count; j++) {
        const struct st20p_rx_ctx* t = &manager->st20p_sessions[j];
        for (int v = 0; v <= t->num_mirror_displays; v++) {
          const struct x11_display_ctx* e = session_window(t, v);
          if (!e->initialized || strcmp(e->output_name, d->output_name) != 0) continue;
          log_info(LOGGER_TAG, "    session %d: window %ux%u at (%d,%d) <- stream %dx%d (crop %d,%d)%s",
                   j, e->width, e->height, e->out_x, e->out_y, t->crop.w, t->crop.h,
                   t->crop.x, t->crop.y, v ? " [mirror]" : "");
        }
      }
    }
  }
  for (int i = 0; i < manager->st20p_count; i++) {
    if (!manager->st20p_sessions[i].display.initialized)
      log_warn(LOGGER_TAG, "  session %d: no window (display init failed)", i);
  }
}

/*
 * session_manager_start - allocate and launch all ST20P RX sessions.
 *
 * For ST20P sessions:
 *   - In single-session mode (st20p_count == 1, normal wall deployment):
 *       Creates one session, opens a dedicated per-receiver X11 window.
 *   - In multi-session mode (st20p_count > 1, legacy composite mode):
 *       Creates N sessions and a shared composite X11 window.
 *
 * Each session gets its own receive thread that blocks on st20p_rx_get_frame().
 * Returns 0 on success, -1 on any failure.
 */
int session_manager_start(session_manager_t* manager, struct rx_app_context* app) {
  if (!manager || !app) {
    return -1;
  }

  /* --- ST20P (video) sessions ------------------------------------------- */
  if (manager->st20p_count > 0) {
    manager->st20p_sessions = calloc(manager->st20p_count, sizeof(struct st20p_rx_ctx));
    if (!manager->st20p_sessions) {
      log_error(LOGGER_TAG, "Failed to allocate ST20P contexts");
      return -1;
    }
    /* Set early so session_manager_stop() also joins sessions started before a failure. */
    manager->running = true;

    /* Display setup — chosen based on session count:
     *
     * st20p_count == 1  (single display mode):
     *   composite stays NULL.  Dedicated X11 window initialised inside
     *   create_st20p_session().
     *
     * st20p_count > 1  (multi-display / crop mode from JSON config):
     *   Compute the bounding box of all crop regions and create a composite
     *   window of exactly that size.  Each session thread writes its crop
     *   region, waits at a barrier, then one thread flushes the display. */
    if (manager->st20p_count > 1 && !is_per_output_mode(app)) {
      manager->composite = calloc(1, sizeof(struct composite_x11_ctx));
      if (!manager->composite) {
        log_error(LOGGER_TAG, "Failed to allocate composite display context");
        return -1;
      }

      /* Compute bounding box from all crop regions */
      uint32_t comp_width  = app->width;
      uint32_t comp_height = app->height;

      if (app->json_config.num_rx_sessions > 0) {
        uint32_t max_x = 0, max_y = 0;
        for (int i = 0; i < app->json_config.num_rx_sessions; i++) {
          const struct crop_config* c = &app->json_config.rx_sessions[i].crop;
          uint32_t right  = (uint32_t)(c->x + c->w);
          uint32_t bottom = (uint32_t)(c->y + c->h);
          if (right  > max_x) max_x = right;
          if (bottom > max_y) max_y = bottom;
        }
        if (max_x > 0) comp_width  = max_x;
        if (max_y > 0) comp_height = max_y;
        log_info(LOGGER_TAG, "Composite bounding box: %ux%u (from %d crop regions)",
           comp_width, comp_height, app->json_config.num_rx_sessions);
      }

      if (init_composite_x11_display(manager->composite,
                                      comp_width, comp_height,
                                      manager->st20p_count) < 0) {
        log_warn(LOGGER_TAG, "Failed to initialise composite X11 display");
        free(manager->composite);
        manager->composite = NULL;
      }
    }

    for (int i = 0; i < manager->st20p_count; i++) {
      manager->st20p_sessions[i].app = app;
      manager->st20p_sessions[i].idx = i;
      /* Wire composite pointer (NULL for single-session) */
      manager->st20p_sessions[i].composite = manager->composite;

      /* NIC, multicast group, port and crop from the JSON rx_sessions entry. */
      const struct rx_session_config* ts = &app->json_config.rx_sessions[i];
      int p = rx_config_port_for_nic(&app->json_config, ts->nic_index);
      const struct interface_config* ifc = &app->json_config.interfaces[p]; /* validated */
      snprintf(manager->st20p_sessions[i].port_name, MTL_PORT_MAX_LEN, "%s", ifc->name);
      inet_pton(AF_INET, ifc->dip, manager->st20p_sessions[i].dip_addr);
      manager->st20p_sessions[i].session_udp_port     = (uint16_t)ts->udp_port;
      manager->st20p_sessions[i].session_payload_type  = (uint8_t)ts->payload_type;
      manager->st20p_sessions[i].crop = ts->crop;
      log_info(LOGGER_TAG, "Session %d: nic %d (%s) %s:%d, payload %d, crop (%d,%d %dx%d)",
         i, ts->nic_index, ifc->name, ifc->dip, ts->udp_port, ts->payload_type,
         ts->crop.x, ts->crop.y, ts->crop.w, ts->crop.h);

      if (create_st20p_session(app, &manager->st20p_sessions[i], i) < 0) {
        return -1;
      }

      if (pthread_create(&manager->st20p_sessions[i].thread, NULL, st20p_rx_thread, &manager->st20p_sessions[i]) != 0) {
        log_error(LOGGER_TAG, "Failed to create ST20P thread %d", i);
        return -1;
      }
    }
  }

  manager->running = true;
  if (!manager->composite) log_display_layout(manager);
  log_info(LOGGER_TAG, "All RX sessions started successfully");

  return 0;
}

/*
 * session_manager_stop - gracefully stop all running sessions.
 *
 * Wakes any threads blocked in st20p_rx_get_frame(), waits for each
 * thread to exit, then frees the MTL session handles and display contexts.
 * Safe to call more than once (guarded by manager->running).
 */
void session_manager_stop(session_manager_t* manager) {
  if (!manager || !manager->running) {
    return;
  }

  manager->running = false;

  /* A session may wait at the composite barrier for a peer that stopped or never started. */
  composite_abort(manager->composite);

  /* --- Stop ST20P (video) sessions -------------------------------------- */
  if (manager->st20p_sessions) {
    for (int i = 0; i < manager->st20p_count; i++) {
      if (manager->st20p_sessions[i].handle) {
        /* Unblock st20p_rx_get_frame() so the thread observes app->exit */
        st20p_rx_wake_block(manager->st20p_sessions[i].handle);

        if (manager->st20p_sessions[i].thread) {
          pthread_join(manager->st20p_sessions[i].thread, NULL);
        }

        st20p_rx_free(manager->st20p_sessions[i].handle);

        /* Per-session display (single-session mode only) */
        cleanup_x11_display(&manager->st20p_sessions[i].display);
        for (int m = 0; m < manager->st20p_sessions[i].num_mirror_displays; m++)
          cleanup_x11_display(&manager->st20p_sessions[i].mirror_display[m]);
      }
    }

    /* Composite display (multi-session mode) */
    if (manager->composite) {
      cleanup_composite_x11_display(manager->composite);
      free(manager->composite);
      manager->composite = NULL;
    }
  }
}

/*
 * session_manager_cleanup - full teardown of the session manager.
 *
 * Calls session_manager_stop() to join threads and free session handles,
 * then releases the session context arrays and shuts down the MTL library
 * (which releases DPDK resources and hugepage mappings).
 */
void session_manager_cleanup(session_manager_t* manager) {
  if (!manager) {
    return;
  }

  /* Stop and join all session threads; free handles and display contexts */
  session_manager_stop(manager);

  /* Release the context arrays that were allocated in session_manager_start() */
  if (manager->st20p_sessions) {
    free(manager->st20p_sessions);
    manager->st20p_sessions = NULL;
  }

  /* Shut down MTL — releases DPDK port, hugepages, and kernel NIC binding */
  if (manager->mtl) {
    mtl_uninit(manager->mtl);
    manager->mtl = NULL;
  }

  manager->st20p_count = 0;
  manager->running = false;
}

/* Check if sessions are running */
bool session_manager_is_running(const session_manager_t* manager) {
  return manager && manager->running;
}