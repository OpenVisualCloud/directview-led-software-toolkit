/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

/*
 * ptp_auto.c — automatic PTP time-source selection. See include/util/ptp_auto.h.
 *
 * Start-up sequence
 * -----------------
 *  1. Map the DPDK ports in interfaces[] to a kernel interface with a PHC:
 *     the port itself, its SR-IOV PF, or a sibling port of the same NIC that
 *     sits on the port's IPv4 subnet. None found -> system time.
 *  2. If a ptp4l/ts2phc already steers that PHC, use it as-is.
 *  3. Listen for PTP Announce messages (L2 and UDPv4) for a few seconds to see
 *     whether a grandmaster is already present, and on which transport/domain.
 *  4. Start ptp4l on the interface with a deliberately poor BMCA priority.
 *     With a grandmaster present it becomes SLAVE and locks the PHC to it;
 *     without one it becomes MASTER and serves the PHC. The resulting state
 *     decides SWITCH vs NIC. ptp4l failing to start -> system time.
 *
 * ptp4l is configured entirely through command-line long options, so nothing
 * is written to disk. It runs in its own process group so the terminal's
 * SIGINT does not kill it before MTL has shut down, and with PR_SET_PDEATHSIG
 * so it does not outlive a crashed dvledtx.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "util/ptp_auto.h"
#include "util/logger.h"
#include "util/ptp_clock.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef ETH_P_1588
#define ETH_P_1588 0x88F7
#endif

/* Encoding of a /dev/ptpN descriptor as a POSIX dynamic clock id. */
#define PTP_FD_TO_CLOCKID(fd) ((~(clockid_t)(fd) << 3) | 3)

#define PTP_AUTO_PROBE_MS        4000u  /* listen for an existing grandmaster */
#define PTP_AUTO_WAIT_SLAVE_MS   20000u /* ptp4l: LISTENING -> locked SLAVE */
#define PTP_AUTO_WAIT_MASTER_MS  5000u  /* ptp4l: LISTENING -> MASTER */
#define PTP_AUTO_STOP_MS         2000u  /* SIGTERM grace period before SIGKILL */
#define PTP_AUTO_MAX_CANDIDATES  16
#define PTP_AUTO_MAX_IFACES      4      /* netdevs per PCI function */
#define PTP_AUTO_PROC_PATH       512    /* "<proc root>/<pid>" */

/* BMCA priority1/priority2 for our ptp4l. Any grandmaster configured with a
 * lower (better) priority — every switch default — wins and we follow it; with
 * none on the LAN we become the grandmaster. */
#define PTP_AUTO_PRIORITY        250
#define PTP_AUTO_NIC_DOMAIN      0
#define PTP_AUTO_NIC_LOG_ANNOUNCE (-2)
#define PTP_AUTO_UTC_OFFSET_S    37     /* TAI - UTC; ptp4l's default utc_offset */

#define PTP_MSG_ANNOUNCE         0x0B
#define PTP_ANNOUNCE_LEN         64
#define PTP_GENERAL_PORT         320
#define PTP_PRIMARY_MCAST_IP     "224.0.1.129"

#define PTP_TRANSPORT_L2         2
#define PTP_TRANSPORT_UDPV4      4

static const uint8_t ptp_primary_mcast_mac[6] = {0x01, 0x1B, 0x19, 0x00, 0x00, 0x00};

enum ptp4l_state {
  P4L_UNKNOWN = 0,
  P4L_LISTENING,
  P4L_UNCALIBRATED,
  P4L_SLAVE,
  P4L_MASTER,
  P4L_PASSIVE,
  P4L_FAULTY,
};

#define P4L_BIT(s) (1u << (s))

struct announce_info {
  int     transport;     /* PTP_TRANSPORT_L2 or PTP_TRANSPORT_UDPV4 */
  uint8_t domain;
  int8_t  log_announce;  /* sender's logMessageInterval */
  uint8_t priority1;
  uint8_t clock_class;
  uint8_t gm_id[8];
};

struct candidate {
  char        iface[IFNAMSIZ];
  int         nic;          /* index into nics[] it was derived from */
  const char* relation;     /* for logging */
  bool        needs_subnet; /* sibling port: only if it shares the port's subnet */
};

struct ptp_auto {
  enum ptp_auto_source source;
  char iface[IFNAMSIZ];
  char phc[32];

  pid_t     pid;   /* ptp4l started by us; 0 if none */
  int       out_fd;
  pthread_t reader;
  bool      reader_started;

  pthread_mutex_t  lock;
  pthread_cond_t   cond;
  enum ptp4l_state state;   /* guarded by lock */
  bool             exited;  /* guarded by lock: ptp4l exited, not yet reaped */
  bool             running; /* guarded by lock: start-up finished, ptp4l in use */
  bool             stopping;/* guarded by lock */
};

/* ------------------------------------------------------------------ */
/* Test seams                                                           */
/* ------------------------------------------------------------------ */

static bool iface_on_subnet(const char* iface, const uint8_t ip[IP_ADDR_BYTES]);
static int  probe_announce(const char* iface, unsigned int timeout_ms, struct announce_info* out);
static int  seed_phc_from_system(int phc_index);

static const char* const ptp4l_default_paths[] = {
    "/usr/sbin/ptp4l", "/usr/local/sbin/ptp4l", "/sbin/ptp4l", NULL};

/* Absolute paths only: dvledtx runs as root, so the binary must not come from
 * a caller-controlled PATH. */
static const char* const* g_ptp4l_paths = ptp4l_default_paths;
static const char* g_sysfs_root = "/sys";
static const char* g_proc_root  = "/proc";
static unsigned int g_probe_ms  = PTP_AUTO_PROBE_MS;
static unsigned int g_wait_slave_ms  = PTP_AUTO_WAIT_SLAVE_MS;
static unsigned int g_wait_master_ms = PTP_AUTO_WAIT_MASTER_MS;
static int  (*g_phc_index_fn)(const char*) = ptp_clock_phc_index;
static bool (*g_iface_on_subnet_fn)(const char*, const uint8_t*) = iface_on_subnet;
static int  (*g_probe_fn)(const char*, unsigned int, struct announce_info*) = probe_announce;
static int  (*g_seed_fn)(int) = seed_phc_from_system;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

const char* ptp_auto_source_name(enum ptp_auto_source src) {
  switch (src) {
    case PTP_AUTO_SRC_SWITCH:   return "switch grandmaster";
    case PTP_AUTO_SRC_NIC:      return "NIC grandmaster";
    case PTP_AUTO_SRC_EXTERNAL: return "external ptp4l";
    case PTP_AUTO_SRC_SYSTEM:
    default:                    return "system time";
  }
}

static const char* ptp4l_state_name(enum ptp4l_state s) {
  switch (s) {
    case P4L_LISTENING:    return "LISTENING";
    case P4L_UNCALIBRATED: return "UNCALIBRATED";
    case P4L_SLAVE:        return "SLAVE";
    case P4L_MASTER:       return "MASTER";
    case P4L_PASSIVE:      return "PASSIVE";
    case P4L_FAULTY:       return "FAULTY";
    case P4L_UNKNOWN:
    default:               return "UNKNOWN";
  }
}

static uint64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void deadline_after_ms(struct timespec* ts, unsigned int ms) {
  clock_gettime(CLOCK_MONOTONIC, ts);
  ts->tv_sec  += ms / 1000u;
  ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
  if (ts->tv_nsec >= 1000000000L) {
    ts->tv_sec++;
    ts->tv_nsec -= 1000000000L;
  }
}

/* "DDDD:BB:DD.F" in hex — the value is used to build sysfs paths. */
static bool is_pci_bdf(const char* s) {
  static const char pattern[] = "xxxx:xx:xx.x";
  if (s == NULL || strlen(s) != sizeof(pattern) - 1) return false;
  for (size_t i = 0; i < sizeof(pattern) - 1; i++) {
    if (pattern[i] == 'x') {
      if (!isxdigit((unsigned char)s[i])) return false;
    } else if (s[i] != pattern[i]) {
      return false;
    }
  }
  return true;
}

static bool read_line_file(const char* path, char* out, size_t out_size) {
  FILE* f = fopen(path, "re");
  if (f == NULL) return false;
  bool ok = fgets(out, (int)out_size, f) != NULL;
  fclose(f);
  if (ok) out[strcspn(out, "\n")] = '\0';
  return ok;
}

/* ------------------------------------------------------------------ */
/* Port -> kernel interface discovery                                   */
/* ------------------------------------------------------------------ */

static int pci_ifaces(const char* bdf, char names[][IFNAMSIZ], int max) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/bus/pci/devices/%s/net", g_sysfs_root, bdf);
  DIR* dir = opendir(path);
  if (dir == NULL) return 0;

  int n = 0;
  struct dirent* ent;
  while (n < max && (ent = readdir(dir)) != NULL) {
    size_t len = strlen(ent->d_name);
    if (ent->d_name[0] == '.' || len >= IFNAMSIZ) continue;
    memcpy(names[n++], ent->d_name, len + 1);
  }
  closedir(dir);
  return n;
}

/* BDF of the SR-IOV physical function behind a VF. */
static bool pci_physfn(const char* bdf, char* out, size_t out_size) {
  char link[PATH_MAX];
  char target[PATH_MAX];
  snprintf(link, sizeof(link), "%s/bus/pci/devices/%s/physfn", g_sysfs_root, bdf);
  ssize_t len = readlink(link, target, sizeof(target) - 1);
  if (len <= 0) return false;
  target[len] = '\0';

  const char* base = strrchr(target, '/');
  base = (base != NULL) ? base + 1 : target;
  if (!is_pci_bdf(base)) return false;
  snprintf(out, out_size, "%s", base);
  return true;
}

static int add_candidate(struct candidate* c, int n, int max, const char* iface,
                         int nic, const char* relation, bool needs_subnet) {
  for (int i = 0; i < n; i++)
    if (strcmp(c[i].iface, iface) == 0) return n;
  if (n >= max) return n;
  snprintf(c[n].iface, sizeof(c[n].iface), "%s", iface);
  c[n].nic          = nic;
  c[n].relation     = relation;
  c[n].needs_subnet = needs_subnet;
  return n + 1;
}

/* Candidates in preference order: every port's own interface and SR-IOV PF
 * first, then sibling functions of the same NIC. */
static int find_candidates(const struct nic_config* nics, int nic_count,
                           struct candidate* out, int max) {
  char names[PTP_AUTO_MAX_IFACES][IFNAMSIZ];
  int n = 0;

  for (int i = 0; i < nic_count; i++) {
    if (!is_pci_bdf(nics[i].port)) continue;

    int k = pci_ifaces(nics[i].port, names, PTP_AUTO_MAX_IFACES);
    for (int j = 0; j < k; j++)
      n = add_candidate(out, n, max, names[j], i, "kernel interface", false);

    char pf[16];
    if (pci_physfn(nics[i].port, pf, sizeof(pf))) {
      k = pci_ifaces(pf, names, PTP_AUTO_MAX_IFACES);
      for (int j = 0; j < k; j++)
        n = add_candidate(out, n, max, names[j], i, "SR-IOV PF", false);
    }
  }

  char devdir[PATH_MAX];
  snprintf(devdir, sizeof(devdir), "%s/bus/pci/devices", g_sysfs_root);
  for (int i = 0; i < nic_count; i++) {
    if (!is_pci_bdf(nics[i].port)) continue;

    char base[PORT_NAME_LEN];
    if (!pci_physfn(nics[i].port, base, sizeof(base)))
      snprintf(base, sizeof(base), "%s", nics[i].port);
    /* Same domain:bus:device, any function — "0000:4b:00." */
    const size_t prefix_len = 11;

    DIR* dir = opendir(devdir);
    if (dir == NULL) continue;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
      if (!is_pci_bdf(ent->d_name)) continue;
      if (strncmp(ent->d_name, base, prefix_len) != 0) continue;
      if (strcmp(ent->d_name, base) == 0 || strcmp(ent->d_name, nics[i].port) == 0) continue;
      int k = pci_ifaces(ent->d_name, names, PTP_AUTO_MAX_IFACES);
      for (int j = 0; j < k; j++)
        n = add_candidate(out, n, max, names[j], i, "sibling port", true);
    }
    closedir(dir);
  }
  return n;
}

static bool iface_is_up(const char* iface) {
  char path[PATH_MAX];
  char state[32];
  snprintf(path, sizeof(path), "%s/class/net/%s/operstate", g_sysfs_root, iface);
  if (!read_line_file(path, state, sizeof(state))) return false;
  return strcmp(state, "up") == 0 || strcmp(state, "unknown") == 0;
}

static bool iface_on_subnet(const char* iface, const uint8_t ip[IP_ADDR_BYTES]) {
  uint32_t addr;
  memcpy(&addr, ip, sizeof(addr));
  if (addr == 0) return false;

  struct ifaddrs* list = NULL;
  if (getifaddrs(&list) != 0) return false;

  bool match = false;
  for (struct ifaddrs* ifa = list; ifa != NULL && !match; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == NULL || ifa->ifa_netmask == NULL) continue;
    if (ifa->ifa_addr->sa_family != AF_INET || strcmp(ifa->ifa_name, iface) != 0) continue;
    uint32_t a = ((const struct sockaddr_in*)ifa->ifa_addr)->sin_addr.s_addr;
    uint32_t m = ((const struct sockaddr_in*)ifa->ifa_netmask)->sin_addr.s_addr;
    match = (m != 0) && ((a & m) == (addr & m));
  }
  freeifaddrs(list);
  return match;
}

/* ------------------------------------------------------------------ */
/* Already-running PTP daemon                                           */
/* ------------------------------------------------------------------ */

static bool cmdline_has_iface(const char* buf, size_t len, const char* iface) {
  const char* end = buf + len;
  for (const char* arg = buf; arg < end; arg += strlen(arg) + 1) {
    if (strcmp(arg, "-i") == 0) {
      const char* next = arg + strlen(arg) + 1;
      if (next < end && strcmp(next, iface) == 0) return true;
    } else if (strncmp(arg, "-i", 2) == 0 && strcmp(arg + 2, iface) == 0) {
      return true;
    }
  }
  return false;
}

/* True if the process has `phc_path` open. ptp4l (hardware timestamping) and
 * ts2phc keep the PHC open for as long as they steer it. */
static bool proc_holds_phc(const char* pid_dir, const char* phc_path) {
  char fd_dir[PTP_AUTO_PROC_PATH + 8];
  snprintf(fd_dir, sizeof(fd_dir), "%s/fd", pid_dir);
  DIR* dir = opendir(fd_dir);
  if (dir == NULL) return false;

  bool held = false;
  struct dirent* ent;
  while (!held && (ent = readdir(dir)) != NULL) {
    if (ent->d_name[0] == '.') continue;
    char link[PATH_MAX];
    char target[64];
    snprintf(link, sizeof(link), "%s/%s", fd_dir, ent->d_name);
    ssize_t len = readlink(link, target, sizeof(target) - 1);
    if (len <= 0) continue;
    target[len] = '\0';
    held = strcmp(target, phc_path) == 0;
  }
  closedir(dir);
  return held;
}

/* A ptp4l/ts2phc that already steers this PHC — bound to the interface on its
 * command line, or holding the PHC open (config-file interfaces, or another
 * port sharing the PHC). phc2sys is deliberately not matched: reading the PHC
 * into CLOCK_REALTIME is a common companion and does not steer the PHC. */
static bool ptp_daemon_running_on(const char* iface, int phc_index, long* pid_out,
                                  char* comm_out, size_t comm_size) {
  DIR* dir = opendir(g_proc_root);
  if (dir == NULL) return false;

  char phc_path[32];
  snprintf(phc_path, sizeof(phc_path), "/dev/ptp%d", phc_index);

  bool found = false;
  struct dirent* ent;
  while (!found && (ent = readdir(dir)) != NULL) {
    char* endp = NULL;
    long pid = strtol(ent->d_name, &endp, 10);
    if (endp == ent->d_name || *endp != '\0' || pid <= 0 || pid == (long)getpid()) continue;

    char pid_dir[PTP_AUTO_PROC_PATH];
    char path[PTP_AUTO_PROC_PATH + 16];
    char comm[32];
    snprintf(pid_dir, sizeof(pid_dir), "%s/%s", g_proc_root, ent->d_name);
    snprintf(path, sizeof(path), "%s/comm", pid_dir);
    if (!read_line_file(path, comm, sizeof(comm))) continue;
    if (strcmp(comm, "ptp4l") != 0 && strcmp(comm, "ts2phc") != 0) continue;

    snprintf(path, sizeof(path), "%s/cmdline", pid_dir);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      char buf[4096];
      ssize_t len = read(fd, buf, sizeof(buf) - 1);
      close(fd);
      if (len > 0) {
        buf[len] = '\0';
        found = cmdline_has_iface(buf, (size_t)len, iface);
      }
    }
    if (!found) found = proc_holds_phc(pid_dir, phc_path);

    if (found) {
      if (pid_out != NULL) *pid_out = pid;
      if (comm_out != NULL) snprintf(comm_out, comm_size, "%s", comm);
    }
  }
  closedir(dir);
  return found;
}

/* ------------------------------------------------------------------ */
/* Grandmaster probe                                                    */
/* ------------------------------------------------------------------ */

static bool parse_announce(const uint8_t* b, size_t len, struct announce_info* out) {
  if (b == NULL || len < PTP_ANNOUNCE_LEN) return false;
  if ((b[0] & 0x0F) != PTP_MSG_ANNOUNCE) return false;
  if ((b[1] & 0x0F) != 2) return false;
  size_t msg_len = ((size_t)b[2] << 8) | b[3];
  if (msg_len < PTP_ANNOUNCE_LEN || msg_len > len) return false;

  out->domain       = b[4];
  out->log_announce = (int8_t)b[33];
  out->priority1    = b[47];
  out->clock_class  = b[48];
  memcpy(out->gm_id, &b[53], sizeof(out->gm_id));
  return true;
}

static int open_l2_probe(int ifindex) {
  int fd = socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, htons(ETH_P_1588));
  if (fd < 0) return -1;

  struct sockaddr_ll sll;
  memset(&sll, 0, sizeof(sll));
  sll.sll_family   = AF_PACKET;
  sll.sll_protocol = htons(ETH_P_1588);
  sll.sll_ifindex  = ifindex;

  struct packet_mreq mr;
  memset(&mr, 0, sizeof(mr));
  mr.mr_ifindex = ifindex;
  mr.mr_type    = PACKET_MR_MULTICAST;
  mr.mr_alen    = sizeof(ptp_primary_mcast_mac);
  memcpy(mr.mr_address, ptp_primary_mcast_mac, sizeof(ptp_primary_mcast_mac));

  if (bind(fd, (struct sockaddr*)&sll, sizeof(sll)) < 0 ||
      setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int open_udp_probe(const char* iface, int ifindex) {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return -1;

  int one = 1;
  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family      = AF_INET;
  sin.sin_port        = htons(PTP_GENERAL_PORT);
  sin.sin_addr.s_addr = htonl(INADDR_ANY);

  struct ip_mreqn mreq;
  memset(&mreq, 0, sizeof(mreq));
  inet_pton(AF_INET, PTP_PRIMARY_MCAST_IP, &mreq.imr_multiaddr);
  mreq.imr_ifindex = ifindex;

  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0 ||
      setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface, (socklen_t)strlen(iface)) < 0 ||
      bind(fd, (struct sockaddr*)&sin, sizeof(sin)) < 0 ||
      setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/* 1: a grandmaster announced, 0: none within timeout_ms, -1: could not listen. */
static int probe_announce(const char* iface, unsigned int timeout_ms, struct announce_info* out) {
  int ifindex = (int)if_nametoindex(iface);
  if (ifindex == 0) return -1;

  struct pollfd pfd[2];
  const int transport[2] = {PTP_TRANSPORT_L2, PTP_TRANSPORT_UDPV4};
  pfd[0].fd = open_l2_probe(ifindex);
  pfd[1].fd = open_udp_probe(iface, ifindex);
  pfd[0].events = pfd[1].events = POLLIN;
  if (pfd[0].fd < 0 && pfd[1].fd < 0) {
    LOG_WARN("PTP: cannot listen for PTP Announce on %s (%s); ptp4l will decide",
             iface, strerror(errno));
    return -1;
  }

  int found = 0;
  const uint64_t deadline = mono_ms() + timeout_ms;
  while (!found) {
    uint64_t now = mono_ms();
    if (now >= deadline) break;
    int ret = poll(pfd, 2, (int)(deadline - now));
    if (ret < 0 && errno != EINTR) break;
    for (int i = 0; i < 2 && !found; i++) {
      if (pfd[i].fd < 0 || !(pfd[i].revents & POLLIN)) continue;
      uint8_t buf[512];
      ssize_t len;
      while ((len = recv(pfd[i].fd, buf, sizeof(buf), 0)) > 0) {
        if (parse_announce(buf, (size_t)len, out)) {
          out->transport = transport[i];
          found = 1;
          break;
        }
      }
    }
  }

  for (int i = 0; i < 2; i++)
    if (pfd[i].fd >= 0) close(pfd[i].fd);
  return found;
}

/* Start a free-running PHC near TAI so its timestamps are plausible; the
 * receivers only need to agree with it. */
static int seed_phc_from_system(int phc_index) {
  char path[32];
  snprintf(path, sizeof(path), "/dev/ptp%d", phc_index);
  int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    LOG_WARN("PTP: cannot open %s to seed it from the system clock (%s)", path, strerror(errno));
    return -1;
  }

  int ret = 0;
  clockid_t clkid = PTP_FD_TO_CLOCKID(fd);
  struct timespec phc, tai;
  if (clock_gettime(clkid, &phc) == 0 && clock_gettime(CLOCK_REALTIME, &tai) == 0) {
    tai.tv_sec += PTP_AUTO_UTC_OFFSET_S;
    int64_t diff_s = (int64_t)tai.tv_sec - (int64_t)phc.tv_sec;
    if (diff_s > 1 || diff_s < -1) {
      if (clock_settime(clkid, &tai) == 0) {
        LOG_INFO("PTP: set %s from the system clock (was %" PRId64 " s off)", path, diff_s);
      } else {
        LOG_WARN("PTP: cannot set %s from the system clock (%s)", path, strerror(errno));
        ret = -1;
      }
    }
  }
  close(fd);
  return ret;
}

/* ------------------------------------------------------------------ */
/* ptp4l process                                                        */
/* ------------------------------------------------------------------ */

static const char* find_ptp4l(void) {
  for (const char* const* p = g_ptp4l_paths; *p != NULL; p++)
    if (access(*p, X_OK) == 0) return *p;
  return NULL;
}

/* "ptp4l[123.456]: msg" -> "msg" */
static const char* ptp4l_strip_prefix(const char* line) {
  if (strncmp(line, "ptp4l[", 6) != 0) return line;
  const char* p = strstr(line, "]: ");
  return (p != NULL) ? p + 3 : line;
}

/* Port state transitions look like "port 1 (eth0): LISTENING to MASTER on
 * ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES". Returns the new state, or P4L_UNKNOWN. */
static enum ptp4l_state ptp4l_parse_state(const char* msg) {
  static const struct {
    const char*      name;
    enum ptp4l_state state;
  } map[] = {
      {"INITIALIZING", P4L_LISTENING}, {"LISTENING", P4L_LISTENING},
      {"PRE_MASTER", P4L_LISTENING},   {"UNCALIBRATED", P4L_UNCALIBRATED},
      {"SLAVE", P4L_SLAVE},            {"MASTER", P4L_MASTER},
      {"GRAND_MASTER", P4L_MASTER},    {"PASSIVE", P4L_PASSIVE},
      {"FAULTY", P4L_FAULTY},          {"DISABLED", P4L_FAULTY},
  };

  for (const char* p = strstr(msg, " to "); p != NULL; p = strstr(p + 1, " to ")) {
    const char* s = p + 4;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
      size_t len = strlen(map[i].name);
      if (strncmp(s, map[i].name, len) == 0 && strncmp(s + len, " on ", 4) == 0)
        return map[i].state;
    }
  }
  return P4L_UNKNOWN;
}

static enum ptp_auto_source role_of(enum ptp4l_state s) {
  if (s == P4L_SLAVE || s == P4L_UNCALIBRATED) return PTP_AUTO_SRC_SWITCH;
  if (s == P4L_MASTER) return PTP_AUTO_SRC_NIC;
  return PTP_AUTO_SRC_SYSTEM;
}

static bool contains_ci(const char* hay, const char* needle) {
  return strcasestr(hay, needle) != NULL;
}

static void ptp4l_handle_line(struct ptp_auto* pa, const char* line) {
  const char* msg = ptp4l_strip_prefix(line);
  if (msg[0] == '\0') return;

  enum ptp4l_state st = ptp4l_parse_state(msg);
  if (st != P4L_UNKNOWN) {
    pthread_mutex_lock(&pa->lock);
    enum ptp4l_state prev = pa->state;
    pa->state = st;
    pthread_cond_broadcast(&pa->cond);
    pthread_mutex_unlock(&pa->lock);

    LOG_INFO("ptp4l: %s", msg);
    enum ptp_auto_source role = role_of(st);
    if (role != PTP_AUTO_SRC_SYSTEM && role != role_of(prev))
      LOG_INFO("PTP: %s", role == PTP_AUTO_SRC_SWITCH
                              ? "following the switch grandmaster"
                              : "this NIC is now the PTP grandmaster");
    return;
  }

  if (strstr(msg, "rms ") != NULL || strstr(msg, "master offset") != NULL)
    LOG_DEBUG("ptp4l: %s", msg);
  else if (contains_ci(msg, "fail") || contains_ci(msg, "error") ||
           contains_ci(msg, "timed out") || contains_ci(msg, "fault"))
    LOG_WARN("ptp4l: %s", msg);
  else
    LOG_INFO("ptp4l: %s", msg);
}

static void* ptp4l_reader_thread(void* arg) {
  struct ptp_auto* pa = (struct ptp_auto*)arg;

  FILE* f = fdopen(pa->out_fd, "r");
  if (f != NULL) {
    char* line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) >= 0) {
      line[strcspn(line, "\r\n")] = '\0';
      ptp4l_handle_line(pa, line);
    }
    free(line);
    fclose(f);
  } else {
    close(pa->out_fd);
  }

  /* Wait without reaping: the zombie keeps the pid reserved, so a later
   * kill() in ptp_auto_stop() cannot hit a recycled pid. */
  siginfo_t si;
  memset(&si, 0, sizeof(si));
  while (waitid(P_PID, (id_t)pa->pid, &si, WEXITED | WNOWAIT) < 0 && errno == EINTR) {
  }

  pthread_mutex_lock(&pa->lock);
  pa->exited = true;
  bool report = pa->running && !pa->stopping;
  pthread_cond_broadcast(&pa->cond);
  pthread_mutex_unlock(&pa->lock);

  if (report)
    LOG_WARN("PTP: ptp4l on %s exited (%s %d); the NIC PHC is free-running and "
             "receivers have lost their PTP reference",
             pa->iface, si.si_code == CLD_EXITED ? "status" : "signal", si.si_status);
  return NULL;
}

static int ptp4l_spawn(struct ptp_auto* pa, const char* bin, int transport,
                       int domain, int log_announce) {
  char prio1[32], prio2[32], dom[32], ann[48];
  snprintf(prio1, sizeof(prio1), "--priority1=%d", PTP_AUTO_PRIORITY);
  snprintf(prio2, sizeof(prio2), "--priority2=%d", PTP_AUTO_PRIORITY);
  snprintf(dom, sizeof(dom), "--domainNumber=%d", domain);
  snprintf(ann, sizeof(ann), "--logAnnounceInterval=%d", log_announce);

  /* network_transport L2 by default: DPDK's ixgbe timesync installs only an
   * ethertype 0x88F7 filter, so MTL receivers cannot timestamp PTP over UDP. */
  const char* argv[] = {
      bin, "-i", pa->iface, "-m", "-H",
      transport == PTP_TRANSPORT_UDPV4 ? "-4" : "-2",
      prio1, prio2, "--clockClass=248", dom, ann,
      "--announceReceiptTimeout=3", "--logSyncInterval=-3",
      "--logMinDelayReqInterval=-3", "--tx_timestamp_timeout=50",
      "--summary_interval=5", NULL};

  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC) < 0) return -1;

  pid_t parent = getpid();
  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }
  if (pid == 0) {
    /* Child: async-signal-safe calls only until execv. */
    setpgid(0, 0);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != parent) _exit(1);
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) dup2(devnull, STDIN_FILENO);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    execv(bin, (char* const*)argv);
    _exit(127);
  }

  close(pipefd[1]);
  pa->pid    = pid;
  pa->out_fd = pipefd[0];
  LOG_INFO("PTP: started %s on %s (pid %d, %s, domain %d, priority1 %d)", bin,
           pa->iface, (int)pid, transport == PTP_TRANSPORT_UDPV4 ? "UDPv4" : "L2",
           domain, PTP_AUTO_PRIORITY);
  return 0;
}

static void ptp4l_stop(struct ptp_auto* pa) {
  if (pa->pid <= 0) return;

  pthread_mutex_lock(&pa->lock);
  pa->stopping = true;
  bool exited = pa->exited;
  pthread_mutex_unlock(&pa->lock);

  if (!exited) {
    kill(pa->pid, SIGTERM);
    if (pa->reader_started) {
      struct timespec dl;
      deadline_after_ms(&dl, PTP_AUTO_STOP_MS);
      pthread_mutex_lock(&pa->lock);
      while (!pa->exited)
        if (pthread_cond_timedwait(&pa->cond, &pa->lock, &dl) == ETIMEDOUT) break;
      exited = pa->exited;
      pthread_mutex_unlock(&pa->lock);
    }
    if (!exited) {
      LOG_WARN("PTP: ptp4l (pid %d) did not exit on SIGTERM; killing it", (int)pa->pid);
      kill(pa->pid, SIGKILL);
    }
  }

  if (pa->reader_started) {
    pthread_join(pa->reader, NULL);
    pa->reader_started = false;
  } else if (pa->out_fd >= 0) {
    close(pa->out_fd);
  }
  pa->out_fd = -1;

  while (waitpid(pa->pid, NULL, 0) < 0 && errno == EINTR) {
  }
  pa->pid = 0;
}

/* Wait until ptp4l reaches one of the states in want_mask, exits, or times out. */
static enum ptp4l_state ptp4l_wait(struct ptp_auto* pa, unsigned int want_mask,
                                   unsigned int timeout_ms, bool* exited) {
  struct timespec dl;
  deadline_after_ms(&dl, timeout_ms);

  pthread_mutex_lock(&pa->lock);
  while (!pa->exited && !(want_mask & P4L_BIT(pa->state)))
    if (pthread_cond_timedwait(&pa->cond, &pa->lock, &dl) == ETIMEDOUT) break;
  enum ptp4l_state st = pa->state;
  *exited = pa->exited;
  pthread_mutex_unlock(&pa->lock);
  return st;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

static void use_system_time(struct ptp_auto* pa) {
  pa->source   = PTP_AUTO_SRC_SYSTEM;
  pa->iface[0] = '\0';
  pa->phc[0]   = '\0';
}

struct ptp_auto* ptp_auto_start(const struct nic_config* nics, int nic_count) {
  struct ptp_auto* pa = calloc(1, sizeof(*pa));
  if (pa == NULL) {
    LOG_ERROR("PTP: allocation failed; using system time");
    return NULL;
  }
  pa->out_fd = -1;
  pa->source = PTP_AUTO_SRC_SYSTEM;
  pthread_mutex_init(&pa->lock, NULL);
  pthread_condattr_t ca;
  pthread_condattr_init(&ca);
  pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
  pthread_cond_init(&pa->cond, &ca);
  pthread_condattr_destroy(&ca);

  /* 1. Locate a PHC behind the configured ports. */
  struct candidate cands[PTP_AUTO_MAX_CANDIDATES];
  int ncand = find_candidates(nics, nic_count, cands, PTP_AUTO_MAX_CANDIDATES);
  const struct candidate* chosen = NULL;
  int phc_index = -1;
  for (int i = 0; i < ncand && chosen == NULL; i++) {
    const struct candidate* c = &cands[i];
    if (c->needs_subnet && !g_iface_on_subnet_fn(c->iface, nics[c->nic].sip_addr)) {
      LOG_DEBUG("PTP: skipping %s (%s of %s): not on the port's subnet",
                c->iface, c->relation, nics[c->nic].port);
      continue;
    }
    if (!iface_is_up(c->iface)) {
      LOG_INFO("PTP: skipping %s (%s of %s): link is down", c->iface, c->relation,
               nics[c->nic].port);
      continue;
    }
    phc_index = g_phc_index_fn(c->iface);
    if (phc_index < 0) {
      LOG_INFO("PTP: skipping %s (%s of %s): no PTP hardware clock", c->iface,
               c->relation, nics[c->nic].port);
      continue;
    }
    chosen = c;
  }

  if (chosen == NULL) {
    LOG_WARN("PTP: no PTP hardware clock is reachable through a kernel interface "
             "for the configured port(s); using system time. Keep the PF on the "
             "kernel driver and give DPDK SR-IOV VFs to enable PTP.");
    return pa;
  }

  snprintf(pa->iface, sizeof(pa->iface), "%s", chosen->iface);
  snprintf(pa->phc, sizeof(pa->phc), "/dev/ptp%d", phc_index);
  LOG_INFO("PTP: using %s (%s of %s), PHC %s", pa->iface, chosen->relation,
           nics[chosen->nic].port, pa->phc);

  /* 2. Someone else already steers this PHC — do not fight over it. */
  long ext_pid = 0;
  char ext_comm[32] = "";
  if (ptp_daemon_running_on(pa->iface, phc_index, &ext_pid, ext_comm, sizeof(ext_comm))) {
    pa->source = PTP_AUTO_SRC_EXTERNAL;
    LOG_INFO("PTP: %s (pid %ld) already manages %s; using it as-is", ext_comm, ext_pid,
             pa->phc);
    return pa;
  }

  const char* bin = find_ptp4l();
  if (bin == NULL) {
    LOG_WARN("PTP: ptp4l not found (install linuxptp); using system time");
    use_system_time(pa);
    return pa;
  }

  /* 3. Is a grandmaster already announcing? */
  struct announce_info ann;
  memset(&ann, 0, sizeof(ann));
  int found = g_probe_fn(pa->iface, g_probe_ms, &ann);
  int transport    = PTP_TRANSPORT_L2;
  int domain       = PTP_AUTO_NIC_DOMAIN;
  int log_announce = PTP_AUTO_NIC_LOG_ANNOUNCE;
  if (found == 1) {
    transport = ann.transport;
    domain    = ann.domain;
    /* Match the grandmaster's announce interval so ptp4l's LISTENING timeout
     * (announceReceiptTimeout intervals) does not expire between its announces. */
    log_announce = ann.log_announce;
    if (log_announce < -3) log_announce = -3;
    if (log_announce > 4) log_announce = 4;
    LOG_INFO("PTP: grandmaster %02x%02x%02x.%02x%02x.%02x%02x%02x is announcing on %s "
             "(%s, domain %u, priority1 %u, clockClass %u)",
             ann.gm_id[0], ann.gm_id[1], ann.gm_id[2], ann.gm_id[3], ann.gm_id[4],
             ann.gm_id[5], ann.gm_id[6], ann.gm_id[7], pa->iface,
             transport == PTP_TRANSPORT_UDPV4 ? "UDPv4" : "L2", ann.domain,
             ann.priority1, ann.clock_class);
  } else {
    if (found == 0)
      LOG_INFO("PTP: no grandmaster announced on %s within %u ms", pa->iface, g_probe_ms);
    g_seed_fn(phc_index);
  }

  /* 4. Run ptp4l and let BMCA settle the role. */
  if (ptp4l_spawn(pa, bin, transport, domain, log_announce) < 0) {
    LOG_WARN("PTP: cannot start ptp4l (%s); using system time", strerror(errno));
    use_system_time(pa);
    return pa;
  }
  if (pthread_create(&pa->reader, NULL, ptp4l_reader_thread, pa) != 0) {
    LOG_WARN("PTP: cannot monitor ptp4l; stopping it and using system time");
    ptp4l_stop(pa);
    use_system_time(pa);
    return pa;
  }
  pa->reader_started = true;

  unsigned int want = (found == 1) ? P4L_BIT(P4L_SLAVE)
                                   : P4L_BIT(P4L_SLAVE) | P4L_BIT(P4L_MASTER);
  unsigned int wait_ms = (found == 0) ? g_wait_master_ms : g_wait_slave_ms;
  bool exited = false;
  enum ptp4l_state st = ptp4l_wait(pa, want, wait_ms, &exited);
  pthread_mutex_lock(&pa->lock);
  exited = pa->exited;
  pa->running = !exited;
  pthread_mutex_unlock(&pa->lock);

  if (exited) {
    LOG_WARN("PTP: ptp4l on %s exited during start-up; using system time", pa->iface);
    ptp4l_stop(pa);
    use_system_time(pa);
    return pa;
  }

  if (st == P4L_SLAVE) {
    pa->source = PTP_AUTO_SRC_SWITCH;
  } else if (st == P4L_MASTER) {
    pa->source = PTP_AUTO_SRC_NIC;
  } else {
    pa->source = (found == 1) ? PTP_AUTO_SRC_SWITCH : PTP_AUTO_SRC_NIC;
    LOG_WARN("PTP: ptp4l on %s has not settled after %u ms (state %s); continuing "
             "with its PHC, which will follow once ptp4l locks",
             pa->iface, wait_ms, ptp4l_state_name(st));
  }

  LOG_INFO("PTP: time source is the %s (%s on %s)", ptp_auto_source_name(pa->source),
           pa->phc, pa->iface);
  return pa;
}

void ptp_auto_stop(struct ptp_auto* pa) {
  if (pa == NULL) return;
  ptp4l_stop(pa);
  pthread_cond_destroy(&pa->cond);
  pthread_mutex_destroy(&pa->lock);
  free(pa);
}

enum ptp_auto_source ptp_auto_get_source(const struct ptp_auto* pa) {
  return (pa != NULL) ? pa->source : PTP_AUTO_SRC_SYSTEM;
}

const char* ptp_auto_phc_device(const struct ptp_auto* pa) {
  return (pa != NULL) ? pa->phc : "";
}

const char* ptp_auto_interface(const struct ptp_auto* pa) {
  return (pa != NULL) ? pa->iface : "";
}
