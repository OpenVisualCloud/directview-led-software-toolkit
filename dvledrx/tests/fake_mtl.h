/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * In-process stand-in for the MTL calls dvledrx makes, so the session manager
 * and main can run without a NIC, DPDK, hugepages or root.  Sessions serve
 * synthetic frames from a small pool and the fake records what it was asked.
 */

#pragma once

#include <stdbool.h>

#include "mtl_api.h"
#include "st_pipeline_api.h"

#define FAKE_MAX_RX 16

struct fake_mtl_state {
  /* knobs (also settable via FAKE_MTL_* environment variables) */
  bool fail_init;          /* FAKE_MTL_FAIL_INIT=1 */
  int fail_create_at;      /* FAKE_MTL_FAIL_CREATE=<n>: n-th create fails, -1 none */
  int frames_per_session;  /* FAKE_MTL_FRAMES=<n>: frames per session, -1 unlimited */
  int empty_every;         /* FAKE_MTL_EMPTY_EVERY=<n>: every n-th frame has no data */
  int raise_after;         /* FAKE_MTL_RAISE_AFTER=<n>: raise(SIGTERM) after n frames */

  /* observations */
  int init_calls, uninit_calls, create_calls, free_calls, wake_calls;
  int frames_got, frames_put, bad_puts, leaked;
  struct mtl_init_params last_init;
  struct st20p_rx_ops ops[FAKE_MAX_RX];
  char names[FAKE_MAX_RX][32]; /* ops[i].name points here */
};

extern struct fake_mtl_state fake_mtl;

/* Clear observations and reload the knobs from the environment. */
void fake_mtl_reset(void);
