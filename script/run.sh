#!/bin/bash

# Run script for TxApp
# Usage: ./run.sh [options]
# Defaults can be overridden via environment variables or arguments

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"  # repo root (one level up from script/)

PORT="${PORT:-0000:02:00.0}"
# CONFIG: path to JSON config file (overrides all other video/session defaults)
CONFIG="${CONFIG:-${APP_ROOT}/config/tx_3sessions.json}"

echo "=== TxApp Run Script ==="
echo "MTL root   : ${MTL_ROOT}"
echo "Port       : ${PORT}"
echo "Config     : ${CONFIG}"
echo "========================"

# Step 0: Kill any stale TxApp / DPDK process holding the VFIO device.
# Use -f (full cmdline match) to also catch the 'sudo' wrapper that keeps
# running when Ctrl+Z suspends only the shell script, not its children.
echo "[0/3] Killing any stale TxApp processes..."
if pkill -9 -f "build/TxApp" 2>/dev/null || pkill -9 -x TxApp 2>/dev/null; then
  echo "  stale process killed — waiting for VFIO device release..."
  sleep 2
else
  echo "  none found"
fi

# Step 1: Bind NIC to vfio-pci.
# Always unbind first (even if already in vfio-pci) so any device state
# left by a killed process is fully reset before we open it again.
echo "[1/3] Binding ${PORT} to vfio-pci..."
sudo modprobe vfio-pci
IFNAME=$(dpdk-devbind.py -s | grep "${PORT}" | grep -oP 'if=\K\S+')
if [ -n "${IFNAME}" ]; then
  echo "  bringing down interface ${IFNAME}..."
  sudo ip link set "${IFNAME}" down
fi
# Force unbind from current driver (vfio-pci or kernel) to reset VFIO state
CURRENT_DRV=$(basename "$(readlink /sys/bus/pci/devices/${PORT}/driver 2>/dev/null)" 2>/dev/null)
if [ -n "${CURRENT_DRV}" ]; then
  echo "  unbinding from ${CURRENT_DRV}..."
  echo "${PORT}" | sudo tee /sys/bus/pci/drivers/${CURRENT_DRV}/unbind > /dev/null
  sleep 0.3
fi
sudo dpdk-devbind.py --bind=vfio-pci "${PORT}" && echo "  bound to vfio-pci OK" || { echo "  ERROR: bind failed"; exit 1; }

# Step 2: Set hugepages
HUGEPAGES="${HUGEPAGES:-4096}"
echo "[2/3] Setting hugepages to ${HUGEPAGES}..."
sudo sysctl -w vm.nr_hugepages="${HUGEPAGES}"

# Step 3: Run TxApp
TIME="${TIME:-}"   # empty = run forever; set e.g. TIME=60 ./run.sh
echo "[3/3] Starting TxApp with config: ${CONFIG}..."
sudo "${APP_ROOT}/build/TxApp" \
    --port   "${PORT}" \
    --config "${CONFIG}" \
    ${TIME:+--time "${TIME}"}
