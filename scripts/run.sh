#!/bin/bash

# Run script for TxApp
# Usage: ./run.sh [options]
# Defaults can be overridden via environment variables or arguments

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"  # repo root (one level up from script/)

PORT="${PORT:-0000:02:00.0}"
SIP="${SIP:-192.168.50.29}"
DIP="${DIP:-239.168.85.20}"
FMT="${FMT:-yuv422p10le}"
FPS="${FPS:-25}"
UDP_PORT="${UDP_PORT:-20000}"
PAYLOAD_TYPE="${PAYLOAD_TYPE:-96}"
TX_URL="${TX_URL:-/home/intel/workspace/karthik/goldencove/sample/bbb_sunflower_1080p_30fps_normal.mp4}"
HUGEPAGES="${HUGEPAGES:-4096}"
CROP_IDX="${CROP_IDX:-0}"    # ignored when SESSIONS>1 (all strips sent)
# SESSIONS: how many vertical strips to transmit simultaneously.
# Hardware limit: MTL needs 1 system queue + 1 queue per session.
#   I225-V  (2.5GbE)  : max 4 TX queues → max 3 sessions
#   X710    (10GbE)   : max 64 TX queues → up to ~63 sessions (bandwidth permitting)
#   E810    (25/100GbE): similar high queue counts
# Bandwidth guide (yuv422p10le, 1080p, 25fps):
#   3 sessions (640px strips)  : ~550 Mbps  → fits 1GbE
#   5 sessions (384px strips)  : ~1.66 Gbps → needs 10GbE
#   6 sessions (320px strips)  : ~2.0 Gbps  → needs 10GbE
SESSIONS="${SESSIONS:-3}"
TIME="${TIME:-}"             # empty = run forever; set e.g. TIME=60 ./run.sh

echo "=== TxApp Run Script ==="
echo "MTL root   : ${MTL_ROOT}"
echo "Port       : ${PORT}"
echo "Source IP  : ${SIP}"
echo "Dest IP    : ${DIP}"
echo "Format     : ${FMT}"
echo "FPS        : ${FPS}"
echo "UDP port   : ${UDP_PORT}"
echo "Payload type: ${PAYLOAD_TYPE}"
echo "TX URL     : ${TX_URL}"
echo "Hugepages  : ${HUGEPAGES}"
echo "Sessions   : ${SESSIONS} (3=full video wall, 1=single crop strip)"
echo "========================"

# Step 0: Kill any stale TxApp / DPDK process holding the VFIO device
echo "[0/3] Killing any stale TxApp processes..."
pkill -9 -x TxApp 2>/dev/null && sleep 0.5 && echo "  stale process killed" || echo "  none found"

# Step 1: Bind NIC to vfio-pci (I225-V does not support SR-IOV, skip create_vf)
echo "[1/3] Binding ${PORT} to vfio-pci..."
sudo modprobe vfio-pci
# Must bring the interface down before dpdk-devbind will accept it
IFNAME=$(dpdk-devbind.py -s | grep "${PORT}" | grep -oP 'if=\K\S+')
if [ -n "${IFNAME}" ]; then
  echo "  bringing down interface ${IFNAME}..."
  sudo ip link set "${IFNAME}" down
fi
sudo dpdk-devbind.py --bind=vfio-pci "${PORT}" && echo "  bound to vfio-pci OK" || { echo "  ERROR: bind failed"; exit 1; }

# Step 2: Set hugepages
echo "[2/3] Setting hugepages to ${HUGEPAGES}..."
sudo sysctl -w vm.nr_hugepages="${HUGEPAGES}"

# Step 3: Run TxApp
echo "[3/3] Starting TxApp (${SESSIONS} sessions)..."
"${APP_ROOT}/build/TxApp" \
    --port "${PORT}" \
    --sip  "${SIP}"  \
    --dip  "${DIP}"  \
    --udp_port "${UDP_PORT}" \
    --payload_type "${PAYLOAD_TYPE}" \
    --fps  "${FPS}"  \
    --fmt  "${FMT}"  \
    --tx_url "${TX_URL}" \
    --st20p_sessions "${SESSIONS}" \
    ${TIME:+--time "${TIME}"}
