/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * ptp_clock — local NIC PTP Hardware Clock (PHC) time source for MTL.
 *
 * Used for the "grandmaster" PTP mode, where this TX host owns the reference
 * clock instead of slaving to an external grandmaster. ptp4l runs as PTP
 * master on the NIC's kernel interface and disciplines/serves that NIC's PHC
 * to the receivers; dvledtx feeds the very same PHC to MTL through
 * mtl_init_params.ptp_get_time_fn so the TX pacing, RTP timestamps and the
 * grandmaster the receivers lock to are all the same clock.
 *
 * MTL calls the time function from the dataplane tasklet, far too often for a
 * per-call /dev/ptpN read (~1-2 us each, a driver register access). The module
 * therefore samples the PHC on a low-rate background thread and serves time
 * from a frequency-corrected linear model over CLOCK_MONOTONIC_RAW, so the
 * fast path is a single vDSO clock_gettime() plus arithmetic.
 */

struct ptp_clock;

/* True when `ref` is a syntactically valid PHC reference — "/dev/ptpN" or a
 * kernel interface name. Does not check that the device exists; used to reject
 * config values before they reach open(). */
bool ptp_clock_valid_device_ref(const char* ref);

/* Open the PHC and start the sampling thread.
 *   device        — "/dev/ptpN", a kernel interface name ("enp4s0"), or "" to
 *                   auto-select the only PHC present on the host.
 *   interval_ms   — PHC resample period; 0 selects the default (1000 ms).
 * Returns NULL on error (device not found, no permission, PHC not readable). */
struct ptp_clock* ptp_clock_open(const char* device, unsigned int interval_ms);

/* Stop the sampling thread and release the handle. Safe with NULL. */
void ptp_clock_close(struct ptp_clock* clk);

/* Current PHC time in nanoseconds (TAI, as served by the local grandmaster).
 * Signature matches mtl_init_params.ptp_get_time_fn; priv is the handle. */
uint64_t ptp_clock_get_time_ns(void* priv);

/* Resolved device path, for logging. Returns "" for a NULL handle. */
const char* ptp_clock_device(const struct ptp_clock* clk);

/* Sampling health snapshot. Any out pointer may be NULL.
 *   offset_ns  — last measured error between the served model and the PHC
 *   freq_ppb   — current model frequency correction vs CLOCK_MONOTONIC_RAW
 *   samples    — number of PHC samples accepted so far
 * Returns 0 on success, -1 for a NULL handle. */
int ptp_clock_stats(const struct ptp_clock* clk, int64_t* offset_ns,
                    int64_t* freq_ppb, uint64_t* samples);
