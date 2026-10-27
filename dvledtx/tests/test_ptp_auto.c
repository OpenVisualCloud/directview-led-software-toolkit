/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

/*
 * Unit tests for ptp_auto.c — automatic PTP time-source selection.
 *
 * ptp_auto.c is included directly to reach its static helpers and test seams:
 * sysfs/procfs roots point at a temporary tree, PHC lookup, subnet check,
 * Announce probe and PHC seeding are replaced by fakes, and ptp4l is a small
 * shell script that prints the state transitions a real ptp4l would. No root,
 * NIC or PTP network is needed.
 */

#define _GNU_SOURCE

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/util/ptp_auto.c"

/* ── Fixture helpers ────────────────────────────────────────────────────── */

static char g_root[64];
static char g_args_file[PATH_MAX];
static char g_script[PATH_MAX];
static const char* g_script_paths[2];

static void mkdirs(const char* path) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char* p = tmp + 1; *p != '\0'; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
  mkdir(tmp, 0755);
}

static void write_file(const char* path, const char* content) {
  FILE* f = fopen(path, "w");
  assert_non_null(f);
  fputs(content, f);
  fclose(f);
}

/* A PCI function; with iface != NULL it is kernel-bound and owns that netdev. */
static void add_pci(const char* bdf, const char* iface, const char* operstate) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/sys/bus/pci/devices/%s", g_root, bdf);
  mkdirs(path);
  if (iface == NULL) return;
  snprintf(path, sizeof(path), "%s/sys/bus/pci/devices/%s/net/%s", g_root, bdf, iface);
  mkdirs(path);
  snprintf(path, sizeof(path), "%s/sys/class/net/%s", g_root, iface);
  mkdirs(path);
  snprintf(path, sizeof(path), "%s/sys/class/net/%s/operstate", g_root, iface);
  write_file(path, operstate);
}

static void add_vf(const char* vf_bdf, const char* pf_bdf) {
  char path[PATH_MAX], target[64];
  add_pci(vf_bdf, NULL, NULL);
  snprintf(path, sizeof(path), "%s/sys/bus/pci/devices/%s/physfn", g_root, vf_bdf);
  snprintf(target, sizeof(target), "../%s", pf_bdf);
  assert_int_equal(symlink(target, path), 0);
}

static void write_script(const char* body) {
  FILE* f = fopen(g_script, "w");
  assert_non_null(f);
  fprintf(f, "#!/bin/sh\nprintf '%%s\\n' \"$@\" > \"$FAKE_PTP4L_ARGS\"\n%s\n", body);
  fclose(f);
  chmod(g_script, 0755);
}

static bool args_contain(const char* arg) {
  FILE* f = fopen(g_args_file, "r");
  if (f == NULL) return false;
  char line[256];
  bool found = false;
  while (!found && fgets(line, sizeof(line), f) != NULL) {
    line[strcspn(line, "\n")] = '\0';
    found = strcmp(line, arg) == 0;
  }
  fclose(f);
  return found;
}

/* ── Fakes ──────────────────────────────────────────────────────────────── */

static int fake_phc_result = 5;
static int fake_phc_index(const char* iface) {
  (void)iface;
  return fake_phc_result;
}

static bool fake_subnet_result = false;
static bool fake_on_subnet(const char* iface, const uint8_t* ip) {
  (void)iface;
  (void)ip;
  return fake_subnet_result;
}

static int fake_probe_result = 0;
static struct announce_info fake_announce;
static int fake_probe_calls = 0;
static int fake_probe(const char* iface, unsigned int timeout_ms, struct announce_info* out) {
  (void)iface;
  (void)timeout_ms;
  fake_probe_calls++;
  if (fake_probe_result == 1) *out = fake_announce;
  return fake_probe_result;
}

static int fake_seed_calls = 0;
static int fake_seed(int idx) {
  (void)idx;
  fake_seed_calls++;
  return 0;
}

static int setup(void** state) {
  (void)state;
  snprintf(g_root, sizeof(g_root), "/tmp/dvledtx-ptp-auto-XXXXXX");
  assert_non_null(mkdtemp(g_root));

  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/proc", g_root);
  mkdirs(path);
  snprintf(path, sizeof(path), "%s/sys/bus/pci/devices", g_root);
  mkdirs(path);
  snprintf(g_args_file, sizeof(g_args_file), "%s/args", g_root);
  snprintf(g_script, sizeof(g_script), "%s/ptp4l", g_root);
  setenv("FAKE_PTP4L_ARGS", g_args_file, 1);

  static char sysfs[PATH_MAX], procfs[PATH_MAX];
  snprintf(sysfs, sizeof(sysfs), "%s/sys", g_root);
  snprintf(procfs, sizeof(procfs), "%s/proc", g_root);
  g_sysfs_root = sysfs;
  g_proc_root  = procfs;

  g_script_paths[0] = g_script;
  g_script_paths[1] = NULL;
  g_ptp4l_paths     = g_script_paths;

  g_phc_index_fn       = fake_phc_index;
  g_iface_on_subnet_fn = fake_on_subnet;
  g_probe_fn           = fake_probe;
  g_seed_fn            = fake_seed;
  g_wait_slave_ms      = 3000;
  g_wait_master_ms     = 3000;

  fake_phc_result    = 5;
  fake_subnet_result = false;
  fake_probe_result  = 0;
  fake_probe_calls   = 0;
  fake_seed_calls    = 0;
  memset(&fake_announce, 0, sizeof(fake_announce));
  return 0;
}

static int teardown(void** state) {
  (void)state;
  char cmd[PATH_MAX + 16];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_root);
  assert_int_equal(system(cmd), 0);
  return 0;
}

static struct nic_config one_nic(const char* bdf) {
  struct nic_config n;
  memset(&n, 0, sizeof(n));
  snprintf(n.port, sizeof(n.port), "%s", bdf);
  n.sip_addr[0] = 192; n.sip_addr[1] = 168; n.sip_addr[2] = 50; n.sip_addr[3] = 29;
  return n;
}

/* ── Pure helpers ───────────────────────────────────────────────────────── */

static void build_announce(uint8_t* b, uint8_t domain, int8_t log_ann, uint8_t prio1) {
  memset(b, 0, PTP_ANNOUNCE_LEN);
  b[0]  = 0x0B;  /* transportSpecific 0, messageType Announce */
  b[1]  = 0x02;  /* versionPTP 2 */
  b[3]  = PTP_ANNOUNCE_LEN;
  b[4]  = domain;
  b[33] = (uint8_t)log_ann;
  b[47] = prio1;
  b[48] = 248;
  for (int i = 0; i < 8; i++) b[53 + i] = (uint8_t)(0x10 + i);
}

static void test_parse_announce_valid(void** state) {
  (void)state;
  uint8_t b[PTP_ANNOUNCE_LEN];
  struct announce_info a;
  build_announce(b, 127, -2, 128);
  assert_true(parse_announce(b, sizeof(b), &a));
  assert_int_equal(a.domain, 127);
  assert_int_equal(a.log_announce, -2);
  assert_int_equal(a.priority1, 128);
  assert_int_equal(a.clock_class, 248);
  assert_int_equal(a.gm_id[0], 0x10);
  assert_int_equal(a.gm_id[7], 0x17);
}

static void test_parse_announce_rejects_other_messages(void** state) {
  (void)state;
  uint8_t b[PTP_ANNOUNCE_LEN];
  struct announce_info a;

  build_announce(b, 0, 0, 128);
  b[0] = 0x00; /* Sync */
  assert_false(parse_announce(b, sizeof(b), &a));

  build_announce(b, 0, 0, 128);
  b[1] = 0x01; /* PTPv1 */
  assert_false(parse_announce(b, sizeof(b), &a));

  build_announce(b, 0, 0, 128);
  assert_false(parse_announce(b, PTP_ANNOUNCE_LEN - 1, &a));

  build_announce(b, 0, 0, 128);
  b[3] = PTP_ANNOUNCE_LEN + 10; /* claims more than was received */
  assert_false(parse_announce(b, sizeof(b), &a));

  assert_false(parse_announce(NULL, 64, &a));
}

static void test_ptp4l_state_parsing(void** state) {
  (void)state;
  assert_int_equal(ptp4l_parse_state("port 1 (eth0): LISTENING to MASTER on "
                                     "ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES"), P4L_MASTER);
  assert_int_equal(ptp4l_parse_state("port 1 (eth0): LISTENING to GRAND_MASTER on "
                                     "ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES"), P4L_MASTER);
  assert_int_equal(ptp4l_parse_state("port 1 (eth0): LISTENING to UNCALIBRATED on "
                                     "RS_SLAVE"), P4L_UNCALIBRATED);
  assert_int_equal(ptp4l_parse_state("port 1 (eth0): UNCALIBRATED to SLAVE on "
                                     "MASTER_CLOCK_SELECTED"), P4L_SLAVE);
  assert_int_equal(ptp4l_parse_state("port 1 (eth0): SLAVE to FAULTY on FAULT_DETECTED"),
                   P4L_FAULTY);
  assert_int_equal(ptp4l_parse_state("port 1 (eth0): INITIALIZING to LISTENING on "
                                     "INIT_COMPLETE"), P4L_LISTENING);

  assert_int_equal(ptp4l_parse_state("failed to create a clock"), P4L_UNKNOWN);
  assert_int_equal(ptp4l_parse_state("selected local clock 001122.fffe.334455 as best master"),
                   P4L_UNKNOWN);
  assert_int_equal(ptp4l_parse_state("rms 12 max 30 freq -2000 +/- 5"), P4L_UNKNOWN);
}

static void test_ptp4l_strip_prefix(void** state) {
  (void)state;
  assert_string_equal(ptp4l_strip_prefix("ptp4l[123.456]: hello"), "hello");
  assert_string_equal(ptp4l_strip_prefix("no prefix"), "no prefix");
  assert_string_equal(ptp4l_strip_prefix("ptp4l[broken"), "ptp4l[broken");
}

static void test_role_of_states(void** state) {
  (void)state;
  assert_int_equal(role_of(P4L_SLAVE), PTP_AUTO_SRC_SWITCH);
  assert_int_equal(role_of(P4L_UNCALIBRATED), PTP_AUTO_SRC_SWITCH);
  assert_int_equal(role_of(P4L_MASTER), PTP_AUTO_SRC_NIC);
  assert_int_equal(role_of(P4L_LISTENING), PTP_AUTO_SRC_SYSTEM);
  assert_int_equal(role_of(P4L_FAULTY), PTP_AUTO_SRC_SYSTEM);
}

static void test_is_pci_bdf(void** state) {
  (void)state;
  assert_true(is_pci_bdf("0000:4b:00.0"));
  assert_true(is_pci_bdf("0000:AF:1f.7"));
  assert_false(is_pci_bdf(NULL));
  assert_false(is_pci_bdf(""));
  assert_false(is_pci_bdf("0000:4b:00"));
  assert_false(is_pci_bdf("0000:4b:00.0/"));
  assert_false(is_pci_bdf("../../../etc"));
  assert_false(is_pci_bdf("0000:4g:00.0"));
}

static void test_cmdline_has_iface(void** state) {
  (void)state;
  static const char a[] = "ptp4l\0-f\0/etc/x.conf\0-i\0enp3s0f0\0-m";
  static const char b[] = "ptp4l\0-ienp3s0f0";
  static const char c[] = "ptp4l\0-i\0enp3s0f1";
  static const char d[] = "ptp4l\0-i";
  assert_true(cmdline_has_iface(a, sizeof(a), "enp3s0f0"));
  assert_true(cmdline_has_iface(b, sizeof(b), "enp3s0f0"));
  assert_false(cmdline_has_iface(c, sizeof(c), "enp3s0f0"));
  assert_false(cmdline_has_iface(d, sizeof(d), "enp3s0f0"));
}

static void test_source_names_and_null_handle(void** state) {
  (void)state;
  assert_string_equal(ptp_auto_source_name(PTP_AUTO_SRC_SYSTEM), "system time");
  assert_string_equal(ptp_auto_source_name(PTP_AUTO_SRC_SWITCH), "switch grandmaster");
  assert_string_equal(ptp_auto_source_name(PTP_AUTO_SRC_NIC), "NIC grandmaster");
  assert_int_equal(ptp_auto_get_source(NULL), PTP_AUTO_SRC_SYSTEM);
  assert_string_equal(ptp_auto_phc_device(NULL), "");
  assert_string_equal(ptp_auto_interface(NULL), "");
  ptp_auto_stop(NULL);
}

/* ── Interface discovery ────────────────────────────────────────────────── */

/* A VF's PF comes first; another port of the same NIC follows, flagged for
 * the subnet check; a different NIC is never considered. */
static void test_candidates_vf_pf_and_sibling(void** state) {
  (void)state;
  add_pci("0000:03:00.0", "enp3s0f0", "up\n");
  add_pci("0000:03:00.1", "enp3s0f1", "up\n");
  add_pci("0000:04:00.0", "enp4s0", "up\n");
  add_vf("0000:03:02.0", "0000:03:00.0");

  struct nic_config nic = one_nic("0000:03:02.0");
  struct candidate c[PTP_AUTO_MAX_CANDIDATES];
  int n = find_candidates(&nic, 1, c, PTP_AUTO_MAX_CANDIDATES);
  assert_int_equal(n, 2);
  assert_string_equal(c[0].iface, "enp3s0f0");
  assert_string_equal(c[0].relation, "SR-IOV PF");
  assert_false(c[0].needs_subnet);
  assert_string_equal(c[1].iface, "enp3s0f1");
  assert_true(c[1].needs_subnet);
}

/* A DPDK-bound PF has no netdev; only its siblings are candidates. */
static void test_candidates_dpdk_pf_uses_sibling(void** state) {
  (void)state;
  add_pci("0000:03:00.0", NULL, NULL);
  add_pci("0000:03:00.1", "enp3s0f1", "up\n");

  struct nic_config nic = one_nic("0000:03:00.0");
  struct candidate c[PTP_AUTO_MAX_CANDIDATES];
  int n = find_candidates(&nic, 1, c, PTP_AUTO_MAX_CANDIDATES);
  assert_int_equal(n, 1);
  assert_string_equal(c[0].iface, "enp3s0f1");
  assert_true(c[0].needs_subnet);
}

/* Two VFs of the same PF yield the PF once. */
static void test_candidates_deduplicated(void** state) {
  (void)state;
  add_pci("0000:03:00.0", "enp3s0f0", "up\n");
  add_vf("0000:03:02.0", "0000:03:00.0");
  add_vf("0000:03:02.1", "0000:03:00.0");

  struct nic_config nics[2] = {one_nic("0000:03:02.0"), one_nic("0000:03:02.1")};
  struct candidate c[PTP_AUTO_MAX_CANDIDATES];
  int n = find_candidates(nics, 2, c, PTP_AUTO_MAX_CANDIDATES);
  assert_int_equal(n, 1);
  assert_string_equal(c[0].iface, "enp3s0f0");
}

static void test_candidates_ignore_invalid_port(void** state) {
  (void)state;
  struct nic_config nic = one_nic("../../etc");
  struct candidate c[PTP_AUTO_MAX_CANDIDATES];
  assert_int_equal(find_candidates(&nic, 1, c, PTP_AUTO_MAX_CANDIDATES), 0);
}

static void add_proc(const char* pid, const char* comm, const char* cmdline, size_t cmdline_len,
                     const char* fd_target) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/proc/%s/fd", g_root, pid);
  mkdirs(path);
  snprintf(path, sizeof(path), "%s/proc/%s/comm", g_root, pid);
  write_file(path, comm);
  snprintf(path, sizeof(path), "%s/proc/%s/cmdline", g_root, pid);
  FILE* f = fopen(path, "w");
  assert_non_null(f);
  fwrite(cmdline, 1, cmdline_len, f);
  fclose(f);
  if (fd_target != NULL) {
    snprintf(path, sizeof(path), "%s/proc/%s/fd/3", g_root, pid);
    assert_int_equal(symlink(fd_target, path), 0);
  }
}

static void test_external_ptp4l_detected(void** state) {
  (void)state;
  add_proc("4321", "ptp4l\n", "ptp4l\0-i\0enp3s0f0\0", 19, NULL);

  long pid = 0;
  char comm[32];
  assert_true(ptp_daemon_running_on("enp3s0f0", 5, &pid, comm, sizeof(comm)));
  assert_int_equal(pid, 4321);
  assert_string_equal(comm, "ptp4l");
  assert_false(ptp_daemon_running_on("enp3s0f1", 5, NULL, NULL, 0));
}

/* A ptp4l whose interface comes from a config file, or that runs on another
 * port sharing the PHC, is found through the PHC it holds open. */
static void test_external_daemon_detected_by_phc(void** state) {
  (void)state;
  add_proc("5000", "ptp4l\n", "ptp4l\0-f\0/etc/ptp4l.conf\0", 26, "/dev/ptp5");
  add_proc("5001", "ts2phc\n", "ts2phc\0-c\0eth9\0", 16, "/dev/ptp7");

  long pid = 0;
  char comm[32];
  assert_true(ptp_daemon_running_on("enp3s0f0", 5, &pid, comm, sizeof(comm)));
  assert_int_equal(pid, 5000);
  assert_true(ptp_daemon_running_on("enp3s0f0", 7, &pid, comm, sizeof(comm)));
  assert_string_equal(comm, "ts2phc");
  assert_false(ptp_daemon_running_on("enp3s0f0", 6, NULL, NULL, 0));
}

/* phc2sys reading the PHC into CLOCK_REALTIME does not steer the PHC. */
static void test_phc2sys_is_not_an_owner(void** state) {
  (void)state;
  add_proc("6000", "phc2sys\n", "phc2sys\0-s\0enp3s0f0\0", 20, "/dev/ptp5");
  assert_false(ptp_daemon_running_on("enp3s0f0", 5, NULL, NULL, 0));
}

/* ── End-to-end start/stop with a fake ptp4l ────────────────────────────── */

static struct ptp_auto* start_vf_setup(void) {
  add_pci("0000:03:00.0", "enp3s0f0", "up\n");
  add_vf("0000:03:02.0", "0000:03:00.0");
  struct nic_config nic = one_nic("0000:03:02.0");
  return ptp_auto_start(&nic, 1);
}

/* No grandmaster on the LAN: the NIC becomes grandmaster on L2, domain 0,
 * after its PHC was seeded from the system clock. */
static void test_start_nic_grandmaster(void** state) {
  (void)state;
  write_script("echo 'ptp4l[1.0]: port 1 (enp3s0f0): INITIALIZING to LISTENING on INIT_COMPLETE'\n"
               "echo 'ptp4l[1.8]: port 1 (enp3s0f0): LISTENING to MASTER on "
               "ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES'\n"
               "exec sleep 30");
  fake_probe_result = 0;

  struct ptp_auto* pa = start_vf_setup();
  assert_non_null(pa);
  assert_int_equal(ptp_auto_get_source(pa), PTP_AUTO_SRC_NIC);
  assert_string_equal(ptp_auto_phc_device(pa), "/dev/ptp5");
  assert_string_equal(ptp_auto_interface(pa), "enp3s0f0");
  assert_int_equal(fake_seed_calls, 1);
  assert_true(args_contain("enp3s0f0"));
  assert_true(args_contain("-2"));
  assert_true(args_contain("--domainNumber=0"));
  assert_true(args_contain("--priority1=250"));
  assert_true(args_contain("--logAnnounceInterval=-2"));

  pid_t pid = pa->pid;
  ptp_auto_stop(pa);
  assert_int_equal(kill(pid, 0), -1); /* reaped */
}

/* A grandmaster announces over UDPv4 in domain 127: ptp4l follows it on the
 * same transport/domain, the PHC is not seeded, and the source is SWITCH. */
static void test_start_switch_grandmaster(void** state) {
  (void)state;
  write_script("echo 'ptp4l[1.0]: port 1 (enp3s0f0): LISTENING to UNCALIBRATED on RS_SLAVE'\n"
               "sleep 0.2\n"
               "echo 'ptp4l[2.0]: port 1 (enp3s0f0): UNCALIBRATED to SLAVE on "
               "MASTER_CLOCK_SELECTED'\n"
               "exec sleep 30");
  fake_probe_result         = 1;
  fake_announce.transport   = PTP_TRANSPORT_UDPV4;
  fake_announce.domain      = 127;
  fake_announce.log_announce = 0;
  fake_announce.priority1   = 128;

  struct ptp_auto* pa = start_vf_setup();
  assert_non_null(pa);
  assert_int_equal(ptp_auto_get_source(pa), PTP_AUTO_SRC_SWITCH);
  assert_int_equal(fake_seed_calls, 0);
  assert_true(args_contain("-4"));
  assert_true(args_contain("--domainNumber=127"));
  assert_true(args_contain("--logAnnounceInterval=0"));
  ptp_auto_stop(pa);
}

/* With a grandmaster detected, a transient MASTER (before its first announce
 * lands) must not end the wait; the later SLAVE decides. */
static void test_start_switch_ignores_transient_master(void** state) {
  (void)state;
  write_script("echo 'ptp4l[1.0]: port 1 (enp3s0f0): LISTENING to MASTER on "
               "ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES'\n"
               "sleep 0.3\n"
               "echo 'ptp4l[2.0]: port 1 (enp3s0f0): MASTER to UNCALIBRATED on RS_SLAVE'\n"
               "echo 'ptp4l[2.5]: port 1 (enp3s0f0): UNCALIBRATED to SLAVE on "
               "MASTER_CLOCK_SELECTED'\n"
               "exec sleep 30");
  fake_probe_result       = 1;
  fake_announce.transport = PTP_TRANSPORT_L2;

  struct ptp_auto* pa = start_vf_setup();
  assert_int_equal(ptp_auto_get_source(pa), PTP_AUTO_SRC_SWITCH);
  ptp_auto_stop(pa);
}

/* ptp4l that never leaves LISTENING: keep its PHC, warn, and pick by probe. */
static void test_start_unsettled_keeps_phc(void** state) {
  (void)state;
  write_script("echo 'ptp4l[1.0]: port 1 (enp3s0f0): INITIALIZING to LISTENING on INIT_COMPLETE'\n"
               "exec sleep 30");
  g_wait_master_ms  = 300;
  fake_probe_result = 0;

  struct ptp_auto* pa = start_vf_setup();
  assert_int_equal(ptp_auto_get_source(pa), PTP_AUTO_SRC_NIC);
  assert_string_equal(ptp_auto_phc_device(pa), "/dev/ptp5");
  ptp_auto_stop(pa);
}

/* ptp4l dies at start-up (e.g. no hardware timestamping): system time. */
static void test_start_ptp4l_exits_falls_back_to_system(void** state) {
  (void)state;
  write_script("echo 'ptp4l[1.0]: interface does not support requested timestamping mode'\n"
               "exit 1");

  struct ptp_auto* pa = start_vf_setup();
  assert_non_null(pa);
  assert_int_equal(ptp_auto_get_source(pa), PTP_AUTO_SRC_SYSTEM);
  assert_string_equal(ptp_auto_phc_device(pa), "");
  assert_int_equal(pa->pid, 0);
  ptp_auto_stop(pa);
}

static void test_start_no_kernel_interface_uses_system(void** state) {
  (void)state;
  add_pci("0000:03:00.0", NULL, NULL);
  struct nic_config nic = one_nic("0000:03:00.0");
  struct ptp_auto* pa = ptp_auto_start(&nic, 1);
  assert_non_null(pa);
  assert_int_equal(ptp_auto_get_source(pa), PTP_AUTO_SRC_SYSTEM);
  assert_int_equal(fake_probe_calls, 0);
  ptp_auto_stop(pa);
}

static void test_start_no_phc_uses_system(void** state) {
  (void)state;
  fake_phc_result = -1;
  struct ptp_auto* pa = start_vf_setup();
  assert_int_equal(ptp_auto_get_source(pa), PTP_AUTO_SRC_SYSTEM);
  assert_int_equal(fake_probe_calls, 0);
  ptp_auto_stop(pa);
}

static void test_start_link_down_uses_system(void** state) {
  (void)state;
  add_pci("0000:03:00.0", "enp3s0f0", "down\n");
  add_vf("0000:03:02.0", "0000:03:00.0");
  struct nic_config nic = one_nic("0000:03:02.0");
  struct ptp_auto* pa = ptp_auto_start(&nic, 1);
  assert_int_equal(ptp_auto_get_source(pa), PTP_AUTO_SRC_SYSTEM);
  ptp_auto_stop(pa);
}

static void test_start_missing_ptp4l_uses_system(void** state) {
  (void)state;
  /* g_script is never written, so no executable exists at that path */
  struct ptp_auto* pa = start_vf_setup();
  assert_int_equal(ptp_auto_get_source(pa), PTP_AUTO_SRC_SYSTEM);
  assert_int_equal(fake_probe_calls, 0);
  ptp_auto_stop(pa);
}

/* An operator-run ptp4l already owns the interface: use its PHC, start nothing. */
static void test_start_reuses_external_ptp4l(void** state) {
  (void)state;
  test_external_ptp4l_detected(state);
  write_script("exit 1");

  struct ptp_auto* pa = start_vf_setup();
  assert_int_equal(ptp_auto_get_source(pa), PTP_AUTO_SRC_EXTERNAL);
  assert_string_equal(ptp_auto_phc_device(pa), "/dev/ptp5");
  assert_int_equal(pa->pid, 0);
  assert_int_equal(fake_probe_calls, 0);
  ptp_auto_stop(pa);
}

/* A sibling port is only used when it shares the DPDK port's subnet. */
static void test_start_sibling_requires_subnet(void** state) {
  (void)state;
  add_pci("0000:03:00.0", NULL, NULL);
  add_pci("0000:03:00.1", "enp3s0f1", "up\n");
  struct nic_config nic = one_nic("0000:03:00.0");

  fake_subnet_result = false;
  struct ptp_auto* pa = ptp_auto_start(&nic, 1);
  assert_int_equal(ptp_auto_get_source(pa), PTP_AUTO_SRC_SYSTEM);
  ptp_auto_stop(pa);

  write_script("echo 'ptp4l[1.0]: port 1 (enp3s0f1): LISTENING to MASTER on "
               "ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES'\n"
               "exec sleep 30");
  fake_subnet_result = true;
  pa = ptp_auto_start(&nic, 1);
  assert_int_equal(ptp_auto_get_source(pa), PTP_AUTO_SRC_NIC);
  assert_string_equal(ptp_auto_interface(pa), "enp3s0f1");
  ptp_auto_stop(pa);
}

/* A ptp4l that ignores SIGTERM is killed after the grace period. */
static void test_stop_escalates_to_sigkill(void** state) {
  (void)state;
  write_script("trap '' TERM\n"
               "echo 'ptp4l[1.0]: port 1 (enp3s0f0): LISTENING to MASTER on "
               "ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES'\n"
               "while :; do sleep 1; done");

  struct ptp_auto* pa = start_vf_setup();
  assert_int_equal(ptp_auto_get_source(pa), PTP_AUTO_SRC_NIC);
  pid_t pid = pa->pid;
  ptp_auto_stop(pa);
  assert_int_equal(kill(pid, 0), -1);
}

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_parse_announce_valid),
      cmocka_unit_test(test_parse_announce_rejects_other_messages),
      cmocka_unit_test(test_ptp4l_state_parsing),
      cmocka_unit_test(test_ptp4l_strip_prefix),
      cmocka_unit_test(test_role_of_states),
      cmocka_unit_test(test_is_pci_bdf),
      cmocka_unit_test(test_cmdline_has_iface),
      cmocka_unit_test(test_source_names_and_null_handle),
      cmocka_unit_test_setup_teardown(test_candidates_vf_pf_and_sibling, setup, teardown),
      cmocka_unit_test_setup_teardown(test_candidates_dpdk_pf_uses_sibling, setup, teardown),
      cmocka_unit_test_setup_teardown(test_candidates_deduplicated, setup, teardown),
      cmocka_unit_test_setup_teardown(test_candidates_ignore_invalid_port, setup, teardown),
      cmocka_unit_test_setup_teardown(test_external_ptp4l_detected, setup, teardown),
      cmocka_unit_test_setup_teardown(test_external_daemon_detected_by_phc, setup, teardown),
      cmocka_unit_test_setup_teardown(test_phc2sys_is_not_an_owner, setup, teardown),
      cmocka_unit_test_setup_teardown(test_start_nic_grandmaster, setup, teardown),
      cmocka_unit_test_setup_teardown(test_start_switch_grandmaster, setup, teardown),
      cmocka_unit_test_setup_teardown(test_start_switch_ignores_transient_master, setup, teardown),
      cmocka_unit_test_setup_teardown(test_start_unsettled_keeps_phc, setup, teardown),
      cmocka_unit_test_setup_teardown(test_start_ptp4l_exits_falls_back_to_system, setup, teardown),
      cmocka_unit_test_setup_teardown(test_start_no_kernel_interface_uses_system, setup, teardown),
      cmocka_unit_test_setup_teardown(test_start_no_phc_uses_system, setup, teardown),
      cmocka_unit_test_setup_teardown(test_start_link_down_uses_system, setup, teardown),
      cmocka_unit_test_setup_teardown(test_start_missing_ptp4l_uses_system, setup, teardown),
      cmocka_unit_test_setup_teardown(test_start_reuses_external_ptp4l, setup, teardown),
      cmocka_unit_test_setup_teardown(test_start_sibling_requires_subnet, setup, teardown),
      cmocka_unit_test_setup_teardown(test_stop_escalates_to_sigkill, setup, teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
