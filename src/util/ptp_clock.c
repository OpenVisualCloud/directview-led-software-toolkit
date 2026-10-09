/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

/*
 * ptp_clock.c — NIC PTP Hardware Clock (PHC) time source for MTL.
 *
 * See include/util/ptp_clock.h for the role this plays in "grandmaster" PTP
 * mode.
 *
 * Model
 * -----
 * A background thread samples the PHC every interval_ms and maintains
 *
 *     phc_ns(t) ~= anchor_phc + (mono_ns(t) - anchor_mono) * rate
 *
 * where mono_ns is CLOCK_MONOTONIC_RAW. Each sample re-anchors on the
 * *estimated* value rather than the measured one, and folds the measured
 * error into `rate` for the next interval, so the served clock stays
 * continuous and monotonic instead of stepping once per second. A measured
 * error larger than PTP_STEP_NS (ptp4l stepping the PHC, or a resumed host)
 * is applied as an explicit step, since slewing it would take minutes.
 *
 * The model is published through a small ring of slots indexed by an atomic
 * counter: readers never block and never take a lock on the dataplane path,
 * and with a >=100 ms publish interval a reader would have to stall for
 * PTP_MODEL_SLOTS intervals to observe a slot being rewritten.
 */

#include "util/ptp_clock.h"
#include "util/logger.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <net/if.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/ethtool.h>
#include <linux/sockios.h>

/* Turn a /dev/ptpN descriptor into a POSIX dynamic clock id (linux/ptp_clock.h
 * documents this encoding; it is not exported by glibc). */
#define PTP_FD_TO_CLOCKID(fd) ((~(clockid_t)(fd) << 3) | 3)

#define PTP_DEFAULT_INTERVAL_MS 1000u
#define PTP_MIN_INTERVAL_MS     100u
#define PTP_MAX_INTERVAL_MS     10000u

#define PTP_MODEL_SLOTS         8u
#define PTP_SAMPLE_TRIES        5
#define PTP_SAMPLE_MAX_LAT_NS   50000ull   /* reject a PHC read slower than 50us */
#define PTP_STEP_NS             1000000ll  /* >1ms error: step instead of slew */
#define PTP_MAX_RATE_DEV        0.001      /* clamp model rate to +/-1000 ppm */
#define PTP_RATE_EMA            0.25       /* smoothing for the measured rate */
#define PTP_LOG_PERIOD_S        30

struct ptp_model {
  uint64_t anchor_mono_ns;
  uint64_t anchor_phc_ns;
  double   rate; /* PHC nanoseconds per CLOCK_MONOTONIC_RAW nanosecond */
};

struct ptp_clock {
  int         fd;
  clockid_t   clkid;
  char        device[64];
  unsigned int interval_ms;

  /* Published model: readers load `gen`, then read slot gen % PTP_MODEL_SLOTS. */
  _Atomic uint64_t gen;
  struct ptp_model slots[PTP_MODEL_SLOTS];

  /* Sampler thread */
  pthread_t        thread;
  bool             thread_started;
  _Atomic bool     stop;

  /* Sampler-private servo state */
  double   base_rate;
  uint64_t prev_mono_ns;
  uint64_t prev_phc_ns;
  bool     have_model;

  /* Statistics (published for ptp_clock_stats) */
  _Atomic int64_t  stat_offset_ns;
  _Atomic int64_t  stat_freq_ppb;
  _Atomic uint64_t stat_samples;
};

static inline uint64_t ts_to_ns(const struct timespec* ts) {
  return (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec;
}

static inline uint64_t mono_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return ts_to_ns(&ts);
}

/* ------------------------------------------------------------------ */
/* Device resolution                                                    */
/* ------------------------------------------------------------------ */

/* "/dev/ptp" followed by digits only — the value comes from a config file, so
 * reject anything that could point the open() somewhere else. */
static bool is_ptp_device_path(const char* s) {
  const char* prefix = "/dev/ptp";
  size_t plen = strlen(prefix);
  if (strncmp(s, prefix, plen) != 0) return false;
  if (s[plen] == '\0') return false;
  for (const char* p = s + plen; *p != '\0'; p++)
    if (!isdigit((unsigned char)*p)) return false;
  return true;
}

static bool is_iface_name(const char* s) {
  size_t len = strlen(s);
  if (len == 0 || len >= IFNAMSIZ) return false;
  for (const char* p = s; *p != '\0'; p++) {
    if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.')
      return false;
  }
  return true;
}

bool ptp_clock_valid_device_ref(const char* ref) {
  if (ref == NULL) return false;
  return is_ptp_device_path(ref) || is_iface_name(ref);
}

/* PHC index backing a kernel network interface, or -1. */
static int phc_index_for_iface(const char* iface) {
  struct ethtool_ts_info info;
  struct ifreq ifr;

  memset(&info, 0, sizeof(info));
  memset(&ifr, 0, sizeof(ifr));
  info.cmd = ETHTOOL_GET_TS_INFO;
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface);
  ifr.ifr_data = (char*)&info;

  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) return -1;
  int ret = ioctl(sock, SIOCETHTOOL, &ifr);
  close(sock);
  if (ret < 0) return -1;
  return info.phc_index;
}

/* Exactly one /dev/ptpN on the host — otherwise the choice is ambiguous and
 * the operator must name it. Returns the index or -1. */
static int phc_index_autodetect(void) {
  DIR* dir = opendir("/sys/class/ptp");
  if (dir == NULL) return -1;

  int found = -1;
  int count = 0;
  struct dirent* ent;
  while ((ent = readdir(dir)) != NULL) {
    if (strncmp(ent->d_name, "ptp", 3) != 0) continue;
    char* endp = NULL;
    long idx = strtol(ent->d_name + 3, &endp, 10);
    if (endp == NULL || *endp != '\0' || idx < 0) continue;
    count++;
    found = (int)idx;
  }
  closedir(dir);

  return count == 1 ? found : -1;
}

static int resolve_device(const char* device, char* out, size_t out_size) {
  if (device == NULL || device[0] == '\0') {
    int idx = phc_index_autodetect();
    if (idx < 0) {
      LOG_ERROR("PTP clock: cannot auto-select a PHC (zero or multiple present); "
                "set ptp.phc_device or ptp.phc_interface");
      return -1;
    }
    snprintf(out, out_size, "/dev/ptp%d", idx);
    return 0;
  }

  if (is_ptp_device_path(device)) {
    snprintf(out, out_size, "%s", device);
    return 0;
  }

  if (is_iface_name(device)) {
    int idx = phc_index_for_iface(device);
    if (idx < 0) {
      LOG_ERROR("PTP clock: interface '%s' has no PTP hardware clock "
                "(not present, or bound to a userspace driver)", device);
      return -1;
    }
    snprintf(out, out_size, "/dev/ptp%d", idx);
    return 0;
  }

  LOG_ERROR("PTP clock: invalid phc device '%s' "
            "(expected '/dev/ptpN' or a kernel interface name)", device);
  return -1;
}

/* ------------------------------------------------------------------ */
/* Model publish / load                                                 */
/* ------------------------------------------------------------------ */

static void model_publish(struct ptp_clock* clk, const struct ptp_model* m) {
  uint64_t gen = atomic_load_explicit(&clk->gen, memory_order_relaxed);
  clk->slots[(gen + 1) % PTP_MODEL_SLOTS] = *m;
  atomic_store_explicit(&clk->gen, gen + 1, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* Sampling                                                             */
/* ------------------------------------------------------------------ */

/* Best-of-N paired read of CLOCK_MONOTONIC_RAW and the PHC, keeping the pair
 * with the shortest PHC read window and midpointing it. */
static int phc_sample(clockid_t clkid, uint64_t* mono_ns, uint64_t* phc_ns) {
  uint64_t best_lat = UINT64_MAX;

  for (int i = 0; i < PTP_SAMPLE_TRIES; i++) {
    struct timespec t0, phc, t1;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &t0) != 0) return -1;
    if (clock_gettime(clkid, &phc) != 0) return -1;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &t1) != 0) return -1;

    uint64_t a = ts_to_ns(&t0), b = ts_to_ns(&t1);
    if (b < a) continue;
    uint64_t lat = b - a;
    if (lat < best_lat) {
      best_lat = lat;
      *mono_ns = a + lat / 2;
      *phc_ns  = ts_to_ns(&phc);
    }
  }

  return (best_lat <= PTP_SAMPLE_MAX_LAT_NS) ? 0 : -1;
}

static inline double clamp_rate(double rate) {
  if (rate < 1.0 - PTP_MAX_RATE_DEV) return 1.0 - PTP_MAX_RATE_DEV;
  if (rate > 1.0 + PTP_MAX_RATE_DEV) return 1.0 + PTP_MAX_RATE_DEV;
  return rate;
}

static void sampler_update(struct ptp_clock* clk, uint64_t mono_ns, uint64_t phc_ns) {
  struct ptp_model next;

  if (!clk->have_model) {
    next.anchor_mono_ns = mono_ns;
    next.anchor_phc_ns  = phc_ns;
    next.rate           = clk->base_rate;
    clk->have_model     = true;
    atomic_store_explicit(&clk->stat_offset_ns, 0, memory_order_relaxed);
  } else {
    const struct ptp_model* cur =
        &clk->slots[atomic_load_explicit(&clk->gen, memory_order_relaxed) % PTP_MODEL_SLOTS];
    uint64_t est = cur->anchor_phc_ns +
                   (uint64_t)llround((double)(mono_ns - cur->anchor_mono_ns) * cur->rate);
    int64_t err = (int64_t)(phc_ns - est);
    uint64_t elapsed = mono_ns - clk->prev_mono_ns;

    atomic_store_explicit(&clk->stat_offset_ns, err, memory_order_relaxed);

    if (llabs(err) > PTP_STEP_NS || elapsed == 0) {
      LOG_WARN("PTP clock: stepping local model by %" PRId64 " ns (PHC jump or stall)", err);
      next.anchor_mono_ns = mono_ns;
      next.anchor_phc_ns  = phc_ns;
      next.rate           = clk->base_rate;
    } else {
      double measured = (double)(phc_ns - clk->prev_phc_ns) / (double)elapsed;
      if (fabs(measured - 1.0) < PTP_MAX_RATE_DEV)
        clk->base_rate += PTP_RATE_EMA * (measured - clk->base_rate);
      clk->base_rate = clamp_rate(clk->base_rate);

      /* Re-anchor on the estimate, not the measurement, so the served clock is
       * continuous; the error is absorbed over the next interval instead. */
      next.anchor_mono_ns = mono_ns;
      next.anchor_phc_ns  = est;
      next.rate           = clamp_rate(clk->base_rate + (double)err / (double)elapsed);
    }
  }

  atomic_store_explicit(&clk->stat_freq_ppb,
                        (int64_t)llround((next.rate - 1.0) * 1e9), memory_order_relaxed);
  model_publish(clk, &next);

  clk->prev_mono_ns = mono_ns;
  clk->prev_phc_ns  = phc_ns;
  atomic_fetch_add_explicit(&clk->stat_samples, 1, memory_order_relaxed);
}

static void* sampler_thread(void* arg) {
  struct ptp_clock* clk = (struct ptp_clock*)arg;
  unsigned int since_log_ms = 0;
  unsigned int consecutive_failures = 0;

  while (!atomic_load_explicit(&clk->stop, memory_order_acquire)) {
    uint64_t mono_ns = 0, phc_ns = 0;
    if (phc_sample(clk->clkid, &mono_ns, &phc_ns) == 0) {
      consecutive_failures = 0;
      sampler_update(clk, mono_ns, phc_ns);
    } else if (++consecutive_failures == 5) {
      LOG_WARN("PTP clock: %s unreadable or read latency too high; "
               "TX timing is coasting on the last known rate", clk->device);
    }

    since_log_ms += clk->interval_ms;
    if (since_log_ms >= PTP_LOG_PERIOD_S * 1000u) {
      since_log_ms = 0;
      LOG_INFO("PTP clock: %s offset=%" PRId64 " ns freq=%" PRId64 " ppb samples=%" PRIu64,
               clk->device,
               atomic_load_explicit(&clk->stat_offset_ns, memory_order_relaxed),
               atomic_load_explicit(&clk->stat_freq_ppb, memory_order_relaxed),
               atomic_load_explicit(&clk->stat_samples, memory_order_relaxed));
    }

    struct timespec req = {.tv_sec  = clk->interval_ms / 1000,
                           .tv_nsec = (long)(clk->interval_ms % 1000) * 1000000L};
    nanosleep(&req, NULL);
  }

  return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

struct ptp_clock* ptp_clock_open(const char* device, unsigned int interval_ms) {
  char path[64];
  if (resolve_device(device, path, sizeof(path)) != 0) return NULL;

  if (interval_ms == 0) interval_ms = PTP_DEFAULT_INTERVAL_MS;
  if (interval_ms < PTP_MIN_INTERVAL_MS) interval_ms = PTP_MIN_INTERVAL_MS;
  if (interval_ms > PTP_MAX_INTERVAL_MS) interval_ms = PTP_MAX_INTERVAL_MS;

  struct ptp_clock* clk = calloc(1, sizeof(*clk));
  if (clk == NULL) {
    LOG_ERROR("PTP clock: allocation failed");
    return NULL;
  }

  clk->fd = open(path, O_RDONLY | O_CLOEXEC);
  if (clk->fd < 0) {
    LOG_ERROR("PTP clock: cannot open %s (%s)", path, strerror(errno));
    free(clk);
    return NULL;
  }

  clk->clkid       = PTP_FD_TO_CLOCKID(clk->fd);
  clk->interval_ms = interval_ms;
  clk->base_rate   = 1.0;
  snprintf(clk->device, sizeof(clk->device), "%s", path);
  atomic_init(&clk->gen, 0);
  atomic_init(&clk->stop, false);
  atomic_init(&clk->stat_offset_ns, 0);
  atomic_init(&clk->stat_freq_ppb, 0);
  atomic_init(&clk->stat_samples, 0);

  uint64_t mono_ns = 0, phc_ns = 0;
  if (phc_sample(clk->clkid, &mono_ns, &phc_ns) != 0) {
    LOG_ERROR("PTP clock: %s is not readable as a POSIX clock", path);
    close(clk->fd);
    free(clk);
    return NULL;
  }
  clk->slots[0] = (struct ptp_model){
      .anchor_mono_ns = mono_ns, .anchor_phc_ns = phc_ns, .rate = 1.0};
  clk->have_model   = true;
  clk->prev_mono_ns = mono_ns;
  clk->prev_phc_ns  = phc_ns;
  atomic_store_explicit(&clk->stat_samples, 1, memory_order_relaxed);

  if (pthread_create(&clk->thread, NULL, sampler_thread, clk) != 0) {
    LOG_ERROR("PTP clock: cannot start sampler thread (%s)", strerror(errno));
    close(clk->fd);
    free(clk);
    return NULL;
  }
  clk->thread_started = true;

  LOG_INFO("PTP clock: using %s as TX time source (resample %u ms, start %" PRIu64 " ns)",
           path, interval_ms, phc_ns);
  return clk;
}

void ptp_clock_close(struct ptp_clock* clk) {
  if (clk == NULL) return;

  if (clk->thread_started) {
    atomic_store_explicit(&clk->stop, true, memory_order_release);
    pthread_join(clk->thread, NULL);
  }
  if (clk->fd >= 0) close(clk->fd);
  free(clk);
}

uint64_t ptp_clock_get_time_ns(void* priv) {
  struct ptp_clock* clk = (struct ptp_clock*)priv;
  if (clk == NULL) return 0;

  uint64_t gen = atomic_load_explicit(&clk->gen, memory_order_acquire);
  const struct ptp_model* m = &clk->slots[gen % PTP_MODEL_SLOTS];

  uint64_t mono = mono_now_ns();
  if (mono <= m->anchor_mono_ns) return m->anchor_phc_ns;
  return m->anchor_phc_ns + (uint64_t)((double)(mono - m->anchor_mono_ns) * m->rate);
}

const char* ptp_clock_device(const struct ptp_clock* clk) {
  return clk != NULL ? clk->device : "";
}

int ptp_clock_stats(const struct ptp_clock* clk, int64_t* offset_ns,
                    int64_t* freq_ppb, uint64_t* samples) {
  if (clk == NULL) return -1;
  if (offset_ns != NULL)
    *offset_ns = atomic_load_explicit(&clk->stat_offset_ns, memory_order_relaxed);
  if (freq_ppb != NULL)
    *freq_ppb = atomic_load_explicit(&clk->stat_freq_ppb, memory_order_relaxed);
  if (samples != NULL)
    *samples = atomic_load_explicit(&clk->stat_samples, memory_order_relaxed);
  return 0;
}
