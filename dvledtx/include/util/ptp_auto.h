/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#pragma once
#include <stdbool.h>

#include "app_context.h"

/*
 * ptp_auto — automatic selection of the TX time source when "ptp.enable" is set.
 *
 * In order of preference:
 *   1. SWITCH — a PTP grandmaster (normally the media switch) is announcing on
 *      the media LAN. dvledtx runs ptp4l as a slave on the NIC's kernel
 *      interface, which locks the NIC PHC to the switch.
 *   2. NIC    — no grandmaster is announcing. ptp4l runs on the same interface
 *      and becomes the grandmaster itself, serving the NIC PHC to the receivers.
 *   3. SYSTEM — no NIC PHC is reachable through a kernel interface (or linuxptp
 *      is missing). MTL keeps its default system clock and TSC pacing.
 *
 * In cases 1 and 2 the same PHC is handed to MTL (see ptp_clock.h), so TX
 * pacing and RTP timestamps always follow the clock the receivers lock to.
 * ptp4l runs with a deliberately poor BMCA priority, so it fails over between
 * 1 and 2 on its own if the switch grandmaster appears or disappears later.
 *
 * The PHC is found from the DPDK ports in interfaces[]: the port's own kernel
 * interface, its SR-IOV physical function, or another port of the same NIC
 * whose IPv4 subnet contains the port's source IP.
 */

enum ptp_auto_source {
  PTP_AUTO_SRC_SYSTEM = 0, /* system clock, TSC pacing */
  PTP_AUTO_SRC_SWITCH,     /* NIC PHC slaved by ptp4l to an external grandmaster */
  PTP_AUTO_SRC_NIC,        /* NIC PHC served by ptp4l as the grandmaster */
  PTP_AUTO_SRC_EXTERNAL,   /* NIC PHC managed by a ptp4l that was already running */
};

struct ptp_auto;

/* Detect and start the best available time source for the given ports.
 * Never fails outright: returns a handle whose source is PTP_AUTO_SRC_SYSTEM
 * when no PHC can be used, or NULL only on allocation failure (treat as SYSTEM). */
struct ptp_auto* ptp_auto_start(const struct nic_config* nics, int nic_count);

/* Stop the ptp4l instance (if dvledtx started one) and free the handle.
 * Safe with NULL. */
void ptp_auto_stop(struct ptp_auto* pa);

/* Source chosen at start-up. Safe with NULL (returns PTP_AUTO_SRC_SYSTEM). */
enum ptp_auto_source ptp_auto_get_source(const struct ptp_auto* pa);

/* "/dev/ptpN" for SWITCH, NIC and EXTERNAL; "" for SYSTEM or NULL. */
const char* ptp_auto_phc_device(const struct ptp_auto* pa);

/* Kernel interface the PHC belongs to; "" for SYSTEM or NULL. */
const char* ptp_auto_interface(const struct ptp_auto* pa);

/* Human-readable source name, for logging. */
const char* ptp_auto_source_name(enum ptp_auto_source src);
