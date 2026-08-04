#!/usr/bin/env bash
#
# dvledtx format/resolution benchmark sweep for the Intel E610 NIC.
#
# Runs the transmitter for a fixed duration per (resolution, fps, pixel format)
# combination, parses the MTL statistics out of the per-run log, and writes a
# CSV plus an XLSX report.
#
# Must be run as root (DPDK/vfio). Designed to be launched inside GNU screen so
# it survives an SSH disconnect:
#
#   sudo ./scripts/benchmark.sh --screen --single
#   sudo ./scripts/benchmark.sh --screen
#
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$REPO_ROOT/build/dvledtx"

SRC_30FPS="/home/intel/workspace/sample/gbrp12le/ball_1080p_gbrp12le_30fps_15min.mp4"
SRC_60FPS="/home/intel/workspace/sample/gbrp12le/ball_2k_gbrp12le_60fps_5min.mp4"
SRC_30_W=1920; SRC_30_H=1080
SRC_60_W=2560; SRC_60_H=1440

# Root of the per-format sample library. When a source matching the case's
# resolution + fps + pixel format exists there, it is used in preference to the
# fallbacks above, so no rescaling or colorspace conversion is required.
SAMPLE_ROOT="/home/intel/workspace/sample"
NATIVE=1

NIC_BDF="0000:03:00.1"
SIP="192.168.50.29"
DIP="239.168.85.20"
UDP_PORT=20000
PAYLOAD_TYPE=96

DURATION=120           # seconds of transmission per case
WARMUP_SAMPLES=2       # MTL stat samples discarded at the start of each run
SHUTDOWN_GRACE=15      # seconds to wait for a clean SIGINT shutdown
SETTLE=8               # seconds between cases (hugepage / NIC release)

SESSION_NAME="dvledbench"
SINGLE=0
USE_SCREEN=0
RESUME=0
SINGLE_CASE="1920 1080 30 yuv422p10le"

FORMATS=(yuv420 yuv422p10le yuv444p10le gbrp10le yuv422p12le yuv444p12le gbrp12le)
# "<width> <height> <fps>"
GEOMETRIES=(
  "1920 1080 30"
  "1920 1080 60"
  "2560 1440 30"
  "2560 1440 60"
  "3840 2160 30"
  "3840 2160 60"
)

usage() {
  cat <<EOF
Usage: sudo $0 [options]

  --screen            Re-exec inside a detached GNU screen session ($SESSION_NAME)
  --single [FMT]      Run a single validation case (default: $SINGLE_CASE)
  --case "W H FPS FMT" Run one explicit case, e.g. --case "3840 2160 30 yuv444p12le"
  --src FILE          Override the source video for --single/--case
  --src-size WxH      Source dimensions when using --src (default: probed)  --src30 FILE        Source video for all 30 fps cases (dimensions probed)
  --src60 FILE        Source video for all 60 fps cases (dimensions probed)  --no-native         Do not auto-select a native source from $SAMPLE_ROOT
  --formats "A B ..." Restrict the sweep to these pixel formats
  --geometries "WxH@FPS ..."  Restrict the sweep to these geometries
  --duration SEC      Transmission time per case (default: $DURATION)
  --resume            Skip cases already present in the results CSV
  --out DIR           Output directory (default: <repo>/benchmark_results/<timestamp>)
  -h, --help          Show this help

Examples:
  sudo $0 --screen --single           # validate the harness on one format
  sudo $0 --screen                    # full sweep (${#GEOMETRIES[@]} x ${#FORMATS[@]} cases)
  sudo $0 --case "3840 2160 30 yuv444p12le" --src /path/native_4k.mp4
  sudo screen -r $SESSION_NAME        # re-attach (session is owned by root)
EOF
}

OUT_DIR=""
SRC_OVERRIDE=""
SRC_OVERRIDE_W=""
SRC_OVERRIDE_H=""
RAW_ARGS=("$@")
while [[ $# -gt 0 ]]; do
  case "$1" in
    --screen)   USE_SCREEN=1; shift ;;
    --single)
      SINGLE=1; shift
      if [[ $# -gt 0 && "$1" != --* ]]; then SINGLE_CASE="1920 1080 30 $1"; shift; fi ;;
    --case)     SINGLE=1; SINGLE_CASE="$2"; shift 2 ;;
    --src)      SRC_OVERRIDE="$2"; shift 2 ;;
    --src-size) SRC_OVERRIDE_W="${2%x*}"; SRC_OVERRIDE_H="${2#*x}"; shift 2 ;;
    --src30)    SRC_30FPS="$2"; SRC_30_W=""; SRC_30_H=""; shift 2 ;;
    --src60)    SRC_60FPS="$2"; SRC_60_W=""; SRC_60_H=""; shift 2 ;;
    --no-native) NATIVE=0; shift ;;
    --formats)  read -r -a FORMATS <<< "$2"; shift 2 ;;
    --geometries)
      GEOMETRIES=()
      for g in $2; do GEOMETRIES+=("${g%x*} $(x=${g#*x}; echo "${x%@*} ${g#*@}")"); done
      shift 2 ;;
    --duration) DURATION="$2"; shift 2 ;;
    --resume)   RESUME=1; shift ;;
    --out)      OUT_DIR="$2"; shift 2 ;;
    -h|--help)  usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

# ---------------------------------------------------------------- screen re-exec
if [[ $USE_SCREEN -eq 1 && -z "${STY:-}" ]]; then
  if ! command -v screen >/dev/null; then
    echo "ERROR: GNU screen is not installed (apt install screen)" >&2
    exit 1
  fi
  inner=()
  for a in "${RAW_ARGS[@]}"; do [[ "$a" == "--screen" ]] || inner+=("$a"); done
  screen -S "$SESSION_NAME" -X quit >/dev/null 2>&1
  screen -dmS "$SESSION_NAME" bash "${BASH_SOURCE[0]}" "${inner[@]}"
  echo "Started detached screen session '$SESSION_NAME' (owned by root)."
  echo "  Attach:  sudo screen -r $SESSION_NAME   (detach with Ctrl-A d)"
  echo "  List:    sudo screen -ls"
  echo "  Progress: tail -f <out-dir>/benchmark.log"
  exit 0
fi

# ---------------------------------------------------------------- pre-flight
if [[ $EUID -ne 0 ]]; then
  echo "ERROR: must run as root (DPDK/vfio access). Use: sudo $0 ..." >&2
  exit 1
fi

[[ -x "$BIN" ]] || { echo "ERROR: $BIN not found. Build first: ninja -C build" >&2; exit 1; }
for f in "$SRC_30FPS" "$SRC_60FPS"; do
  [[ -r "$f" ]] || { echo "ERROR: source video not readable: $f" >&2; exit 1; }
done

# Probe dimensions for any fps fallback source supplied via --src30 / --src60.
probe_dims() {
  ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
          -of csv=p=0:s=x "$1" 2>/dev/null
}
if [[ -z "$SRC_30_W" ]]; then
  d=$(probe_dims "$SRC_30FPS"); SRC_30_W="${d%x*}"; SRC_30_H="${d#*x}"
  [[ -n "$SRC_30_W" && -n "$SRC_30_H" ]] || { echo "ERROR: cannot probe --src30 dimensions" >&2; exit 1; }
fi
if [[ -z "$SRC_60_W" ]]; then
  d=$(probe_dims "$SRC_60FPS"); SRC_60_W="${d%x*}"; SRC_60_H="${d#*x}"
  [[ -n "$SRC_60_W" && -n "$SRC_60_H" ]] || { echo "ERROR: cannot probe --src60 dimensions" >&2; exit 1; }
fi

if [[ -n "$SRC_OVERRIDE" ]]; then
  [[ -r "$SRC_OVERRIDE" ]] || { echo "ERROR: --src not readable: $SRC_OVERRIDE" >&2; exit 1; }
  if [[ -z "$SRC_OVERRIDE_W" || -z "$SRC_OVERRIDE_H" ]]; then
    dims=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
             -of csv=p=0:s=x "$SRC_OVERRIDE" 2>/dev/null)
    SRC_OVERRIDE_W="${dims%x*}"; SRC_OVERRIDE_H="${dims#*x}"
  fi
  if [[ -z "$SRC_OVERRIDE_W" || -z "$SRC_OVERRIDE_H" ]]; then
    echo "ERROR: could not determine source size; pass --src-size WxH" >&2
    exit 1
  fi
fi

free_hp=$(awk '/HugePages_Free/{print $2}' /proc/meminfo)
if [[ "${free_hp:-0}" -lt 512 ]]; then
  echo "ERROR: only ${free_hp:-0} free hugepages. Run: sysctl -w vm.nr_hugepages=2048" >&2
  exit 1
fi
if ! ls /sys/bus/pci/drivers/vfio-pci/"$NIC_BDF" >/dev/null 2>&1; then
  echo "ERROR: $NIC_BDF is not bound to vfio-pci." >&2
  exit 1
fi

if [[ -z "$OUT_DIR" ]]; then
  OUT_DIR="$REPO_ROOT/benchmark_results/$(date +%Y%m%d_%H%M%S)"
fi
LOG_DIR="$OUT_DIR/logs"
CFG_DIR="$OUT_DIR/configs"
CSV="$OUT_DIR/results.csv"
XLSX="$OUT_DIR/e610_benchmark.xlsx"
mkdir -p "$LOG_DIR" "$CFG_DIR"

if [[ ! -f "$CSV" ]]; then
  echo "width,height,fps_target,fmt,samples,fps_avg,fps_min,fps_max,mbps_avg,mbps_min,mbps_max,epoch_drop,starved_frames,status" > "$CSV"
fi

RUN_LOG="$OUT_DIR/benchmark.log"
say() { echo "[$(date '+%F %T')] $*" | tee -a "$RUN_LOG"; }

cleanup_stale() {
  pkill -INT -x dvledtx 2>/dev/null
  sleep 2
  pkill -9 -x dvledtx 2>/dev/null
  sleep 1
}

trap 'say "interrupted, cleaning up"; cleanup_stale; exit 130' INT TERM

# ---------------------------------------------------------------- native source
# Maps a case to a sample file already at the target resolution and pixel
# format, e.g. 3840x2160 30fps yuv444p12le ->
#   $SAMPLE_ROOT/yuv444p12le/ball_4k_yuv444p12le_30fps_5min.mp4
find_native_src() {
  local w=$1 h=$2 fps=$3 fmt=$4 rt fd
  case "${w}x${h}" in
    1920x1080) rt=1080p ;;
    2560x1440) rt=2k ;;
    3840x2160) rt=4k ;;
    *) return 1 ;;
  esac
  # config uses "yuv420"; the sample library uses "yuv420p"
  fd=$fmt; [[ "$fmt" == "yuv420" ]] && fd=yuv420p
  local m
  m=$(ls "$SAMPLE_ROOT/$fd/ball_${rt}_${fd}_${fps}fps_"*.mp4 2>/dev/null | head -1)
  [[ -n "$m" && -r "$m" ]] || return 1
  echo "$m"
}

# ---------------------------------------------------------------- config writer
write_config() {
  local cfg=$1 src=$2 sw=$3 sh=$4 tw=$5 th=$6 fps=$7 fmt=$8 log=$9
  cat > "$cfg" <<EOF
{
  "log_file": "$log",
  "interfaces": [
    {
      "nic_index": 0,
      "name": "$NIC_BDF",
      "sip":  "$SIP",
      "dip":  "$DIP"
    }
  ],
  "video": {
    "width":  $sw,
    "height": $sh,
    "tx_url": "$src"
  },
  "tx_video": {
    "scale_width":  $tw,
    "scale_height": $th,
    "fps":    $fps,
    "fmt":    "$fmt"
  },
  "ptp": {
    "enable":  false,
    "pi":      false,
    "unicast": false
  },
  "tx_sessions": [
    {
      "nic_index":    0,
      "udp_port":     $UDP_PORT,
      "payload_type": $PAYLOAD_TYPE,
      "crop": { "x": 0, "y": 0, "w": $tw, "h": $th }
    }
  ]
}
EOF
}

# ---------------------------------------------------------------- log parser
# Emits: samples fps_avg fps_min fps_max mbps_avg mbps_min mbps_max epoch_drop starved
parse_log() {
  local log=$1 warmup=$2
  awk -v warmup="$warmup" '
    /TX_VIDEO_SESSION\(0,0[:)]/ {
      if (match($0, /fps [0-9.]+/))        { fps[++nf] = substr($0, RSTART+4, RLENGTH-4) }
      if (match($0, /throughput [0-9.]+/)) { bw[++nb]  = substr($0, RSTART+11, RLENGTH-11) }
      if (match($0, /epoch drop [0-9]+/))  { drop += substr($0, RSTART+11, RLENGTH-11) }
      if (match($0, /no ready frame from user [0-9]+/)) { starv += substr($0, RSTART+25, RLENGTH-25) }
    }
    END {
      n = 0; s = 0; mn = 1e9; mx = 0
      for (i = warmup + 1; i <= nf; i++) {
        v = fps[i] + 0
        if (v <= 0) continue
        n++; s += v
        if (v < mn) mn = v
        if (v > mx) mx = v
      }
      m = 0; sb = 0; bmn = 1e9; bmx = 0
      for (i = warmup + 1; i <= nb; i++) {
        v = bw[i] + 0
        if (v <= 0) continue
        m++; sb += v
        if (v < bmn) bmn = v
        if (v > bmx) bmx = v
      }
      if (n == 0) { print "0 0 0 0 0 0 0 " drop+0 " " starv+0; exit }
      printf "%d %.3f %.3f %.3f %.2f %.2f %.2f %d %d\n", n, s/n, mn, mx, \
             (m ? sb/m : 0), (m ? bmn : 0), (m ? bmx : 0), drop+0, starv+0
    }
  ' "$log"
}

# ---------------------------------------------------------------- single case
run_case() {
  local tw=$1 th=$2 fps=$3 fmt=$4
  local tag="${tw}x${th}_${fps}fps_${fmt}"

  if [[ $RESUME -eq 1 ]] && grep -q "^$tw,$th,$fps,$fmt," "$CSV"; then
    say "SKIP  $tag (already in results)"
    return 0
  fi

  local src sw sh srckind
  if [[ -n "$SRC_OVERRIDE" ]]; then
    src="$SRC_OVERRIDE"; sw=$SRC_OVERRIDE_W; sh=$SRC_OVERRIDE_H; srckind="override"
  elif [[ $NATIVE -eq 1 ]] && src=$(find_native_src "$tw" "$th" "$fps" "$fmt"); then
    sw=$tw; sh=$th; srckind="native"
  elif [[ "$fps" -ge 60 ]]; then
    src="$SRC_60FPS"; sw=$SRC_60_W; sh=$SRC_60_H; srckind="scaled"
  else
    src="$SRC_30FPS"; sw=$SRC_30_W; sh=$SRC_30_H; srckind="scaled"
  fi

  local cfg="$CFG_DIR/$tag.json"
  local log="$LOG_DIR/$tag.log"
  local out="$LOG_DIR/$tag.console.log"
  : > "$log"
  write_config "$cfg" "$src" "$sw" "$sh" "$tw" "$th" "$fps" "$fmt" "$log"

  say "RUN   $tag  (${DURATION}s, $srckind src=$(basename "$src"))"
  cleanup_stale

  ( cd "$REPO_ROOT" && "$BIN" --config "$cfg" ) > "$out" 2>&1 &
  local pid=$!

  local waited=0
  while [[ $waited -lt $DURATION ]]; do
    if ! kill -0 "$pid" 2>/dev/null; then
      say "WARN  $tag exited early after ${waited}s"
      break
    fi
    sleep 2; waited=$((waited + 2))
  done

  local status="ok"
  if kill -0 "$pid" 2>/dev/null; then
    kill -INT "$pid" 2>/dev/null
    local g=0
    while kill -0 "$pid" 2>/dev/null && [[ $g -lt $SHUTDOWN_GRACE ]]; do
      sleep 1; g=$((g + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
      say "      MTL shutdown hung, forcing kill -9"
      kill -9 "$pid" 2>/dev/null
    fi
  else
    status="early_exit"
  fi
  wait "$pid" 2>/dev/null
  cleanup_stale

  local r
  r=$(parse_log "$log" "$WARMUP_SAMPLES")
  read -r samples fps_avg fps_min fps_max mbps mbps_min mbps_max drop starv <<< "$r"

  if [[ "${samples:-0}" -eq 0 ]]; then
    status="no_stats"
    say "FAIL  $tag  no MTL statistics captured (see $out)"
  else
    say "DONE  $tag  fps=$fps_avg (min $fps_min / max $fps_max)  bw=${mbps} Mb/s (peak ${mbps_max})  epoch_drop=$drop  samples=$samples"
  fi

  echo "$tw,$th,$fps,$fmt,${samples:-0},${fps_avg:-0},${fps_min:-0},${fps_max:-0},${mbps:-0},${mbps_min:-0},${mbps_max:-0},${drop:-0},${starv:-0},$status" >> "$CSV"
  sleep "$SETTLE"
}

# ---------------------------------------------------------------- sweep
say "output dir: $OUT_DIR"
say "duration/case: ${DURATION}s   free hugepages: $free_hp"

start_ts=$(date +%s)
if [[ $SINGLE -eq 1 ]]; then
  read -r w h f fmt <<< "$SINGLE_CASE"
  say "single validation case: ${w}x${h} ${f}fps $fmt"
  run_case "$w" "$h" "$f" "$fmt"
else
  total=$(( ${#GEOMETRIES[@]} * ${#FORMATS[@]} ))
  say "full sweep: $total cases (~$(( total * (DURATION + SETTLE + 20) / 60 )) min)"
  i=0
  for g in "${GEOMETRIES[@]}"; do
    read -r w h f <<< "$g"
    for fmt in "${FORMATS[@]}"; do
      i=$((i + 1))
      say "--- case $i/$total ---"
      run_case "$w" "$h" "$f" "$fmt"
    done
  done
fi
say "sweep finished in $(( ($(date +%s) - start_ts) / 60 )) min"

# ---------------------------------------------------------------- report
if python3 "$REPO_ROOT/scripts/bench_to_xlsx.py" "$CSV" "$XLSX" >> "$RUN_LOG" 2>&1; then
  say "XLSX written: $XLSX"
else
  say "WARN  XLSX generation failed (see $RUN_LOG); CSV is still at $CSV"
fi

if [[ -n "${SUDO_USER:-}" ]]; then
  chown -R "$SUDO_USER":"$(id -gn "$SUDO_USER")" "$OUT_DIR" 2>/dev/null
fi

say "results: $CSV"
