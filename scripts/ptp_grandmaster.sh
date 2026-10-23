#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# Run ptp4l as the PTP grandmaster for a dvledtx deployment.
#
# Use when the media switch has no usable time reference (its grandmaster has
# no GPS, so its absolute time is wrong) and the transmitter must own the
# reference clock instead. ptp4l disciplines and serves this host's NIC PHC to
# the receivers; dvledtx reads the same PHC through the "ptp" config block with
# "mode": "grandmaster", so TX pacing and RTP timestamps come from the clock the
# receivers lock to.
#
# Usage: sudo bash scripts/ptp_grandmaster.sh <interface> [priority1]
#   e.g. sudo bash scripts/ptp_grandmaster.sh enp4s0 64
#
# <interface> must still be owned by the kernel driver — a port bound to
# vfio-pci has no netdev and no /dev/ptpN. With a DPDK transmitter, keep the PF
# in the kernel and give dvledtx SR-IOV VFs.

set -e

IFACE="${1:?usage: $0 <interface> [priority1]}"
PRIORITY1="${2:-64}"   # lower wins BMCA; switch grandmasters usually sit at 128

if [[ ! -d "/sys/class/net/${IFACE}" ]]; then
  echo "error: interface ${IFACE} not found (bound to a userspace driver?)" >&2
  exit 1
fi

if ! command -v ptp4l >/dev/null 2>&1; then
  echo "error: ptp4l not found — install linuxptp (apt install linuxptp)" >&2
  exit 1
fi

PHC_INDEX="$(ethtool -T "${IFACE}" 2>/dev/null | awk '/PTP Hardware Clock:/ {print $4}')"
if [[ -z "${PHC_INDEX}" || "${PHC_INDEX}" == "none" || "${PHC_INDEX}" == "-1" ]]; then
  echo "error: ${IFACE} has no PTP hardware clock" >&2
  exit 1
fi

# Fixed path, not mktemp: the final exec replaces this shell so an EXIT trap
# would never fire, and repeated runs would leak a file each time.
CONF="/tmp/ptp4l-gm-${IFACE}.conf"

# linuxptp 4.0 renamed masterOnly to serverOnly and warns on the old spelling.
PTP4L_MAJOR="$(ptp4l -v 2>&1 | head -1 | sed -E 's/[^0-9]*([0-9]+).*/\1/')"
if [[ -n "${PTP4L_MAJOR}" && "${PTP4L_MAJOR}" -ge 4 ]]; then
  SERVER_ONLY_KEY="serverOnly"
else
  SERVER_ONLY_KEY="masterOnly"
fi

# priority1 below the switch's keeps BMCA on this host; serverOnly stops ptp4l
# from ever slaving to the switch's GPS-less grandmaster.
#
# network_transport L2 is required, not cosmetic: DPDK's ixgbe timesync only
# installs an ethertype 0x88F7 filter (ixgbe_timesync_enable), so a receiver
# using MTL's built-in PTP client can only hardware-timestamp L2 PTP. Over the
# UDPv4 default the receiver falls back to software timestamps, or fails with
# ptp_timesync_read_rx_time err -22.
cat >"${CONF}" <<EOF
[global]
priority1               ${PRIORITY1}
priority2               128
clockClass              248
domainNumber            0
network_transport       L2
time_stamping           hardware
logAnnounceInterval     -2
logSyncInterval         -3
logMinDelayReqInterval  -3
tx_timestamp_timeout    50
summary_interval        4

[${IFACE}]
${SERVER_ONLY_KEY}              1
EOF

echo "PTP grandmaster on ${IFACE} (/dev/ptp${PHC_INDEX}), priority1=${PRIORITY1}"
echo "dvledtx config:  \"ptp\": { \"enable\": true, \"mode\": \"grandmaster\", \"phc_interface\": \"${IFACE}\" }"
echo

# Seed the free-running PHC from the system clock so its timestamps are at least
# plausible; receivers only need to agree with this clock, not with UTC.
phc_ctl "${IFACE}" set >/dev/null 2>&1 || \
  echo "warn: could not seed ${IFACE} PHC from the system clock; continuing free-running"

exec ptp4l -f "${CONF}" -i "${IFACE}" -m
