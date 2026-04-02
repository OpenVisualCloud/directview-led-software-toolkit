#!/usr/bin/env bash
# =============================================================================
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation
# =============================================================================
# Media Transport Library (MTL) – Automated Build Script for Ubuntu/Debian
# Based on: https://github.com/OpenVisualCloud/Media-Transport-Library/blob/v26.01/doc/build.md
#
# Usage:
#   chmod +x build_mtl_ubuntu.sh
#   ./build_mtl_ubuntu.sh [--with-sdl2] [--no-gtest] [--portable <arch>]
#
# Options:
#   --with-sdl2          Install SDL2 packages (needed for RxTxApp display)
#   --no-gtest           Skip gtest build from source (use apt version only)
#   --force-src-deps     Always build json-c / libpcap / gtest from source,
#                        even when the apt-installed versions are detected.
#   --portable <arch>    Build DPDK with a fixed CPU arch (e.g. haswell, skylake-avx512)
#                        instead of native. Useful for portable/redistributable binaries.
#   --mtl-dir <path>     Override the directory where MTL source is cloned (default: $PWD/Media-Transport-Library)
#   --build-dir <path>   Override base build directory (default: $PWD)
# =============================================================================

# ── POSIX-compatible bash guard (must be first; uses no bash-only syntax) ─────
# This check is intentionally placed before any bash-only constructs (arrays,
# [[ ]], etc.) so that running the script under sh/dash shows a useful error
# instead of a cryptic syntax failure.
if [ -z "${BASH_VERSION:-}" ]; then
    echo "[ERROR]  This script must be run with bash, not sh/dash." >&2
    echo "[ERROR]  Re-run as:  bash $(basename "$0")" >&2
    exit 1
fi

set -o pipefail
# NOTE: We intentionally do NOT use set -e so the script never aborts on failure.
#       Every command is checked individually and errors are logged, but execution continues.

# ── Colour helpers ─────────────────────────────────────────────────────────────
RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'

_info()    { echo -e "${CYAN}[INFO]${RESET}  $*"; }
_ok()      { echo -e "${GREEN}[OK]${RESET}    $*"; }
_warn()    { echo -e "${YELLOW}[WARN]${RESET}   $*"; }
_error()   { echo -e "${RED}[ERROR]${RESET}  $*" >&2; }
_section() { echo -e "\n${BOLD}${CYAN}══════════════════════════════════════════════${RESET}"; \
             echo -e "${BOLD}${CYAN}  $*${RESET}"; \
             echo -e "${BOLD}${CYAN}══════════════════════════════════════════════${RESET}"; }

# ── Global failure tracker ──────────────────────────────────────────────────────
FAILED_STEPS=()

record_failure() {
    local step="$1"
    FAILED_STEPS+=("$step")
    _error "Step FAILED: $step"
}

# ── Run a command, capture result, never abort ──────────────────────────────────
# Usage: run_cmd <description> <cmd> [<arg>...]
# Pass the command and each argument as separate words (array style) to avoid
# eval-based injection; the function executes them directly via "$@".
run_cmd() {
    local desc="$1"; shift
    _info "Running: $desc"
    "$@"
    local rc=$?
    if [ $rc -ne 0 ]; then
        record_failure "$desc (exit code $rc)"
        return $rc
    else
        _ok "$desc"
        return 0
    fi
}

# ── Verify a binary/command exists after install ───────────────────────────────
verify_cmd() {
    local cmd="$1" step_name="$2"
    if command -v "$cmd" &>/dev/null; then
        _ok "Verified command available: $cmd"
    else
        record_failure "Post-install check: '$cmd' not found (step: $step_name)"
        _warn "'$cmd' was not found in PATH after install – dependent steps may fail."
    fi
}

# ── Verify a shared library is known to ldconfig ───────────────────────────────
verify_lib() {
    local lib="$1" step_name="$2"
    if ldconfig -p 2>/dev/null | grep "$lib" > /dev/null; then
        _ok "Verified library in ldcache: $lib"
    else
        _warn "Library '$lib' not found in ldconfig cache after step: $step_name"
        record_failure "Post-install check: library '$lib' missing (step: $step_name)"
    fi
}

# ── Verify a pkg-config module exists ─────────────────────────────────────────
verify_pkg() {
    local module="$1" step_name="$2"
    if pkg-config --exists "$module" 2>/dev/null; then
        _ok "Verified pkg-config module: $module"
    else
        _warn "pkg-config module '$module' not found after step: $step_name"
        record_failure "Post-install check: pkg-config '$module' missing (step: $step_name)"
    fi
}

# ── Pinned versions (edit here to change what is built) ───────────────────────
MTL_TAG="v26.01"
FFMPEG_VERSION="7.0"

# ── Argument parsing ───────────────────────────────────────────────────────────
WITH_SDL2=false
SKIP_GTEST=false
FORCE_SRC_DEPS=false
PORTABLE_ARCH=""
BASE_DIR="${PWD}"
MTL_DIR=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --with-sdl2)       WITH_SDL2=true ;;
        --no-gtest)        SKIP_GTEST=true ;;
        --force-src-deps)  FORCE_SRC_DEPS=true ;;
        --portable)
            shift
            if [[ $# -eq 0 || "$1" == --* ]]; then
                echo "[ERROR]  Missing or invalid value for --portable (expected architecture name)" >&2
                exit 1
            fi
            PORTABLE_ARCH="$1"
            ;;
        --mtl-dir)
            shift
            if [[ $# -eq 0 || "$1" == --* ]]; then
                echo "[ERROR]  Missing or invalid value for --mtl-dir (expected directory path)" >&2
                exit 1
            fi
            MTL_DIR="$1"
            ;;
        --build-dir)
            shift
            if [[ $# -eq 0 || "$1" == --* ]]; then
                echo "[ERROR]  Missing or invalid value for --build-dir (expected directory path)" >&2
                exit 1
            fi
            BASE_DIR="$1"
            ;;
        -h|--help)
            sed -n '/^# Usage:/,/^# =====/p' "$0" | grep -E '^#' | sed 's/^# \?//'
            exit 0
            ;;
        *) _warn "Unknown argument: $1 (ignored)" ;;
    esac
    shift
done

# ── Create a fresh, isolated workspace for all build work ─────────────────────
# Every run gets its own timestamped directory so nothing is ever reused.
# The workspace is removed at the end of the script; only system-installed
# artifacts (via sudo make install / sudo ninja install) persist.
WORKSPACE_DIR="${BASE_DIR}/mtl_build_workspace_$(date +%Y%m%d_%H%M%S)"
mkdir -p "${WORKSPACE_DIR}"

[[ -z "$MTL_DIR" ]] && MTL_DIR="${WORKSPACE_DIR}/Media-Transport-Library"
DPDK_DIR="${WORKSPACE_DIR}/dpdk"

# ── Preamble ───────────────────────────────────────────────────────────────────
_section "MTL Automated Build – Ubuntu/Debian"
_info "Date/Time    : $(date)"
_info "MTL tag      : ${MTL_TAG}"
_info "FFmpeg version: ${FFMPEG_VERSION}"
_info "Base dir     : ${BASE_DIR}"
_info "Workspace    : ${WORKSPACE_DIR}"
_info "MTL source   : ${MTL_DIR}"
_info "DPDK source  : ${DPDK_DIR}"
_info "SDL2 support : ${WITH_SDL2}"
_info "Force src deps: ${FORCE_SRC_DEPS}"
_info "Portable arch: ${PORTABLE_ARCH:-native (default)}"
echo ""

# ══════════════════════════════════════════════════════════════════════════════
# STEP 0 – Pre-flight checks
# All checks record failures but never abort – the script stays non-blocking.
# ══════════════════════════════════════════════════════════════════════════════
_section "Step 0 – Pre-flight checks"

# ── 0.1  Shell is bash (not sh / dash) ───────────────────────────────────────
# The top-of-file POSIX guard already aborts if not running under bash, so
# reaching here means we are confirmed in bash.
_ok "Shell is bash ${BASH_VERSION}"

# ── 0.2  OS check ─────────────────────────────────────────────────────────────
if ! grep -qiE 'ubuntu|debian' /etc/os-release 2>/dev/null; then
    _warn "This script is designed for Ubuntu/Debian. Continuing anyway, but results may vary."
    record_failure "Pre-flight: OS is not Ubuntu/Debian"
else
    DISTRO_ID=$(. /etc/os-release && echo "${ID:-}")
    DISTRO_VERSION_ID=$(. /etc/os-release && echo "${VERSION_ID:-0}")
    DISTRO_NAME=$(. /etc/os-release && echo "${PRETTY_NAME:-}")
    _ok "Detected OS: ${DISTRO_NAME}"

    # For Ubuntu, check minimum version 20.04; Debian uses a different versioning scheme
    if [ "${DISTRO_ID}" = "ubuntu" ]; then
        UBUNTU_VER=$(echo "${DISTRO_VERSION_ID}" | cut -d. -f1)
        if [ "${UBUNTU_VER}" -lt 20 ] 2>/dev/null; then
            _warn "Ubuntu ${DISTRO_VERSION_ID} is older than 20.04 – some packages may not be available."
            record_failure "Pre-flight: Ubuntu version ${DISTRO_VERSION_ID} may be too old (< 20.04)"
        fi
    fi
fi

# ── 0.3  CPU architecture ─────────────────────────────────────────────────────
ARCH=$(uname -m)
if [ "${ARCH}" != "x86_64" ]; then
    _error "MTL and DPDK require x86_64. Detected architecture: ${ARCH}"
    record_failure "Pre-flight: unsupported CPU architecture '${ARCH}' (need x86_64)"
else
    _ok "CPU architecture: ${ARCH}"
fi

# ── 0.4  Not running directly as root ─────────────────────────────────────────
# The script uses 'sudo' for privilege escalation. Running as root directly
# can cause HOME/PATH issues and is generally not recommended.
if [ "$(id -u)" -eq 0 ]; then
    _warn "Script is running as root. Prefer running as a normal user with sudo access."
    _warn "Some operations (pip install paths) may behave unexpectedly as root."
else
    _ok "Running as user: $(id -un) (UID $(id -u))"
fi

# ── 0.5  sudo privilege check ─────────────────────────────────────────────────
if [ "$(id -u)" -ne 0 ]; then
    _info "Checking sudo access (you may be prompted for your password)..."
    if sudo -v 2>/dev/null; then
        _ok "sudo access confirmed."
    else
        _error "sudo access denied. All install steps require sudo."
        record_failure "Pre-flight: sudo access not available for $(id -un)"
    fi
fi

# ── 0.6  Disk space (require at least 20 GB free on BASE_DIR filesystem) ──────
# Create BASE_DIR if it doesn't exist yet (a --build-dir value may not exist yet;
# later steps use mkdir -p for sub-dirs, but the disk/writability checks need the
# directory to be present first).
if [ ! -d "${BASE_DIR}" ]; then
    _info "BASE_DIR '${BASE_DIR}' does not exist – creating it now."
    mkdir -p "${BASE_DIR}" 2>/dev/null \
        || { _error "Cannot create BASE_DIR '${BASE_DIR}'."; record_failure "Pre-flight: cannot create BASE_DIR '${BASE_DIR}'"; }
fi

DISK_FREE_KB=$(df -k "${BASE_DIR}" 2>/dev/null | awk 'NR==2 {print $4}')
DISK_FREE_GB=$(( ${DISK_FREE_KB:-0} / 1024 / 1024 ))
if [ "${DISK_FREE_GB}" -lt 20 ] 2>/dev/null; then
    _warn "Low disk space: ${DISK_FREE_GB} GB free on $(df -k "${BASE_DIR}" | awk 'NR==2{print $6}')."
    _warn "DPDK + FFmpeg + MTL builds typically need ~20 GB. Build may fail mid-way."
    record_failure "Pre-flight: low disk space (${DISK_FREE_GB} GB free, need ~20 GB)"
else
    _ok "Disk space: ${DISK_FREE_GB} GB free – sufficient."
fi

# ── 0.7  BASE_DIR is writable ─────────────────────────────────────────────────
if [ ! -w "${BASE_DIR}" ]; then
    _error "BASE_DIR '${BASE_DIR}' is not writable by $(id -un)."
    record_failure "Pre-flight: BASE_DIR '${BASE_DIR}' not writable"
else
    _ok "BASE_DIR is writable: ${BASE_DIR}"
fi

# ── 0.8  Network connectivity (GitHub reachability) ──────────────────────────
_info "Checking network connectivity to github.com..."
GITHUB_CHECK_TOOL=""
if command -v curl >/dev/null 2>&1; then
    GITHUB_CHECK_TOOL="curl"
elif command -v wget >/dev/null 2>&1; then
    GITHUB_CHECK_TOOL="wget"
else
    _warn "Neither curl nor wget is installed; skipping GitHub reachability check."
    record_failure "Pre-flight: curl/wget missing – cannot verify github.com reachability"
fi

if [ "${GITHUB_CHECK_TOOL}" = "curl" ]; then
    if curl -fsS --max-time 10 https://github.com > /dev/null 2>&1; then
        _ok "github.com is reachable."
    else
        _error "Cannot reach github.com (checked via curl). All git clone steps will fail."
        _error "Check your network, proxy settings, or firewall rules."
        record_failure "Pre-flight: github.com unreachable – git clone steps will fail"
    fi
elif [ "${GITHUB_CHECK_TOOL}" = "wget" ]; then
    if wget -q --spider --timeout=10 https://github.com 2>/dev/null; then
        _ok "github.com is reachable (via wget)."
    else
        _error "Cannot reach github.com (checked via wget). All git clone steps will fail."
        _error "Check your network, proxy settings, or firewall rules."
        record_failure "Pre-flight: github.com unreachable – git clone steps will fail"
    fi
fi

# ── 0.9  apt lock check ───────────────────────────────────────────────────────
_info "Checking for apt/dpkg locks..."
APT_LOCK_HELD=false
for lockfile in /var/lib/dpkg/lock-frontend /var/lib/apt/lists/lock /var/cache/apt/archives/lock; do
    if sudo fuser "${lockfile}" 2>/dev/null | grep '[0-9]' > /dev/null; then
        _error "apt/dpkg lock held on ${lockfile} by another process."
        record_failure "Pre-flight: apt lock held (${lockfile}) – concurrent apt process running"
        APT_LOCK_HELD=true
    fi
done
if ! $APT_LOCK_HELD; then
    _ok "No apt/dpkg locks detected."
fi

# ── 0.10  Set DEBIAN_FRONTEND to avoid apt interactive prompts ────────────────
export DEBIAN_FRONTEND=noninteractive
_ok "DEBIAN_FRONTEND=noninteractive (prevents apt from hanging on prompts)"

# Print pre-flight summary before proceeding
PRE_FLIGHT_FAILS=$(printf '%s\n' "${FAILED_STEPS[@]}" 2>/dev/null | grep -c '^Pre-flight:' 2>/dev/null || true)
PRE_FLIGHT_FAILS=${PRE_FLIGHT_FAILS:-0}
if [ "${PRE_FLIGHT_FAILS}" -gt 0 ]; then
    _warn "${PRE_FLIGHT_FAILS} pre-flight check(s) failed (listed above). Proceeding with caution."
else
    _ok "All pre-flight checks passed."
fi

# ══════════════════════════════════════════════════════════════════════════════
# STEP 1 – Install OS build dependencies
# ══════════════════════════════════════════════════════════════════════════════
_section "Step 1 – Install OS build dependencies (apt-get)"

run_cmd "apt-get update" \
    sudo apt-get update

APT_PACKAGES=(
    git gcc g++ build-essential make meson python3 python3-venv
    pkg-config libnuma-dev libjson-c-dev libpcap-dev
    libgtest-dev libssl-dev systemtap-sdt-dev llvm clang
    flex byacc
)

run_cmd "apt-get install core packages" \
    sudo apt-get install -y "${APT_PACKAGES[@]}"

# Verify key build tools
for cmd in git gcc g++ make meson python3 pkg-config clang llvm-config flex; do
    verify_cmd "$cmd" "apt-get install core packages"
done

# Refresh ldconfig cache so newly installed libs are discoverable
sudo ldconfig 2>/dev/null || true

# Verify core libraries are present in ldconfig cache
for lib in libnuma libssl; do
    if ldconfig -p 2>/dev/null | grep "${lib}\.so" > /dev/null; then
        _ok "Verified library in ldcache: ${lib}"
    else
        _warn "Library '${lib}' not found in ldconfig cache after apt install."
        record_failure "Post-install check: library '${lib}' missing (step: apt-get install core packages)"
    fi
done

# systemtap-sdt-dev is a header-only package – verify via the header file, not ldconfig
if [ -f /usr/include/x86_64-linux-gnu/sys/sdt.h ] || [ -f /usr/include/sys/sdt.h ]; then
    _ok "Verified systemtap-sdt header: sdt.h"
else
    _warn "systemtap-sdt header (sdt.h) not found after apt install."
    record_failure "Post-install check: sdt.h missing (step: apt-get install core packages)"
fi

# Verify pkg-config can resolve core development packages
# Note: libnuma's pkg-config module is named 'numa', not 'libnuma'
for mod in numa json-c libpcap openssl; do
    if pkg-config --exists "${mod}" 2>/dev/null; then
        _ok "pkg-config module available: ${mod}"
    else
        _warn "pkg-config module '${mod}' not found – may be resolved after source build in Step 1c."
    fi
done

# ── Optional SDL2 ─────────────────────────────────────────────────────────────
if $WITH_SDL2; then
    _info "SDL2 support requested – installing SDL2 packages"
    run_cmd "apt-get install SDL2 packages" \
        sudo apt-get install -y libsdl2-dev libsdl2-ttf-dev
    verify_lib "libSDL2" "apt-get install SDL2"
fi

# ── Kernel headers (needed for DPDK / PMD drivers) ────────────────────────────
run_cmd "apt-get install kernel headers" \
    sudo apt-get install -y "linux-headers-$(uname -r)"

if [ -d "/lib/modules/$(uname -r)/build" ]; then
    _ok "Kernel headers present: /lib/modules/$(uname -r)/build"
else
    record_failure "Kernel headers directory not found for $(uname -r)"
    _warn "Some DPDK kernel modules may not build correctly."
fi

# ── Python build tools via isolated venv ─────────────────────────────────────
_section "Step 1b – Install Python build tools via isolated venv (pyelftools, ninja)"

# Use a temporary venv so we don't touch system Python packages.
# This avoids PEP 668 / externally-managed-environment issues entirely
# and keeps the system clean – the venv is removed at the end of the script.
PY_VENV_DIR="${WORKSPACE_DIR}/_pip_build_venv"
_info "Creating temporary Python venv at: ${PY_VENV_DIR}"
run_cmd "create Python venv" \
    python3 -m venv "${PY_VENV_DIR}"

if [ ! -f "${PY_VENV_DIR}/bin/python3" ]; then
    record_failure "Python venv creation failed – pyelftools and ninja will not be available"
    _error "Cannot create Python venv. Ensure python3-venv is installed."
    _warn "Attempting to install python3-venv and retry..."
    run_cmd "apt-get install python3-venv" \
        sudo apt-get install -y python3-venv
    run_cmd "create Python venv (retry)" \
        python3 -m venv "${PY_VENV_DIR}"
fi

if [ -f "${PY_VENV_DIR}/bin/python3" ]; then
    # Install packages inside the venv – no sudo needed, no system-wide impact,
    # and PEP 668 does not apply inside a venv.
    run_cmd "pip install pyelftools (venv)" \
        "${PY_VENV_DIR}/bin/pip" install --timeout 30 pyelftools

    run_cmd "pip install ninja (venv)" \
        "${PY_VENV_DIR}/bin/pip" install --timeout 30 ninja

    # Prepend the venv's bin to PATH so DPDK/FFmpeg build systems use the
    # right python3 (with pyelftools) and the ninja from the venv.
    export PATH="${PY_VENV_DIR}/bin:${PATH}"
    _ok "Added venv to PATH: ${PY_VENV_DIR}/bin"

    # Verify pyelftools is importable
    if python3 -c "import elftools" 2>/dev/null; then
        _ok "Verified: python3 can import elftools (pyelftools via venv)"
    else
        record_failure "pyelftools not importable after venv install"
        _warn "pyelftools import failed – DPDK build may fail during install phase."
    fi

    # Verify ninja is available
    verify_cmd "ninja" "pip install ninja (venv)"
else
    record_failure "Python venv unavailable – pyelftools and ninja not installed"
    _error "Skipping pip package installation. DPDK/FFmpeg build may fail."
fi

# ══════════════════════════════════════════════════════════════════════════════
# STEP 1c – Build dependencies from source (Section 1.2 of build guide)
#
# On Ubuntu the apt packages installed in Step 1 (libjson-c-dev, libpcap-dev,
# libgtest-dev) are usually sufficient.  Each sub-step below checks whether the
# library is already resolvable via pkg-config / ldconfig and SKIPS the source
# build when it is.  Pass --force-src-deps to always build from source.
# ══════════════════════════════════════════════════════════════════════════════
_section "Step 1c – Build dependencies from source (json-c / libpcap / gtest)"

# Ensure cmake is available (needed for json-c and gtest source builds)
if ! command -v cmake &>/dev/null; then
    _info "cmake not found – installing via apt"
    run_cmd "apt-get install cmake" sudo apt-get install -y cmake
    verify_cmd "cmake" "apt-get install cmake"
fi

# ── Helper: build a dep from source only when not already present ─────────────
# Usage: _need_src_build <pkg-config-name-or-empty> <ldconfig-lib-name-or-empty>
# Returns 0 (true) when the source build is needed, 1 when already satisfied.
_need_src_build() {
    local pc_name="$1" lib_name="$2"
    if $FORCE_SRC_DEPS; then
        return 0
    fi
    if [ -n "$pc_name" ] && pkg-config --exists "$pc_name" 2>/dev/null; then
        _ok "Dependency '$pc_name' already satisfied via pkg-config – skipping source build."
        return 1
    fi
    if [ -n "$lib_name" ] && ldconfig -p 2>/dev/null | grep "$lib_name" > /dev/null; then
        _ok "Dependency '$lib_name' already satisfied via ldconfig – skipping source build."
        return 1
    fi
    return 0
}

# Pick up --force-src-deps flag set during argument parsing above.
# Can also be overridden at call time: FORCE_SRC_DEPS=true ./build_mtl_ubuntu.sh

SRC_BUILD_DIR="${WORKSPACE_DIR}/src_deps"
mkdir -p "${SRC_BUILD_DIR}"

# ── 1.2.1  json-c ─────────────────────────────────────────────────────────────
_info "--- 1.2.1  json-c ---"
if _need_src_build "json-c" "libjson-c"; then
    _info "Building json-c from source (tag json-c-0.16)"
    cd "${SRC_BUILD_DIR}"

    rm -rf json-c
    run_cmd "git clone json-c" \
        git clone https://github.com/json-c/json-c.git -b json-c-0.16

    if [ -d "json-c" ]; then
        cd json-c
        mkdir -p build
        cd build
        run_cmd "cmake json-c"         cmake ../
        run_cmd "make json-c"          make -j"$(nproc)"
        run_cmd "sudo make install json-c" sudo make install
        cd "${SRC_BUILD_DIR}"

        run_cmd "ldconfig after json-c install" sudo ldconfig
        verify_lib "libjson-c" "json-c source build"
    else
        record_failure "json-c source directory missing after clone"
    fi

    cd "${BASE_DIR}"
fi

# ── 1.2.2  libpcap ────────────────────────────────────────────────────────────
_info "--- 1.2.2  libpcap ---"
if _need_src_build "libpcap" "libpcap"; then
    _info "Building libpcap from source (tag libpcap-1.9)"
    cd "${SRC_BUILD_DIR}"

    rm -rf libpcap
    run_cmd "git clone libpcap" \
        git clone https://github.com/the-tcpdump-group/libpcap.git -b libpcap-1.9

    if [ -d "libpcap" ]; then
        cd libpcap
        run_cmd "configure libpcap"     ./configure
        run_cmd "make libpcap"          make -j"$(nproc)"
        run_cmd "sudo make install libpcap" sudo make install
        cd "${SRC_BUILD_DIR}"

        run_cmd "ldconfig after libpcap install" sudo ldconfig
        verify_lib "libpcap" "libpcap source build"
    else
        record_failure "libpcap source directory missing after clone"
    fi

    cd "${BASE_DIR}"
fi

# ── 1.2.3  gtest ──────────────────────────────────────────────────────────────
_info "--- 1.2.3  gtest ---"
if $SKIP_GTEST; then
    _info "--no-gtest specified – skipping gtest source build."
elif _need_src_build "gtest" "libgtest"; then
    _info "Building gtest from source (tag v1.13.x)"
    cd "${SRC_BUILD_DIR}"

    rm -rf googletest
    run_cmd "git clone googletest" \
        git clone https://github.com/google/googletest.git -b v1.13.x

    if [ -d "googletest" ]; then
        cd googletest
        mkdir -p build
        cd build
        run_cmd "cmake gtest"              cmake ../
        run_cmd "make gtest"               make -j"$(nproc)"
        run_cmd "sudo make install gtest"  sudo make install
        cd "${SRC_BUILD_DIR}"

        run_cmd "ldconfig after gtest install" sudo ldconfig
        verify_lib "libgtest" "gtest source build"
    else
        record_failure "googletest source directory missing after clone"
    fi

    cd "${BASE_DIR}"
fi

# ══════════════════════════════════════════════════════════════════════════════
# STEP 2 – Clone Media Transport Library
# ══════════════════════════════════════════════════════════════════════════════
_section "Step 2 – Clone Media Transport Library (tag ${MTL_TAG})"

rm -rf "${MTL_DIR}"
run_cmd "git clone MTL tag ${MTL_TAG}" \
    git clone --branch "${MTL_TAG}" --depth 1 \
        https://github.com/OpenVisualCloud/Media-Transport-Library.git \
        "${MTL_DIR}"

export mtl_source_code="${MTL_DIR}"

if [ -d "${mtl_source_code}" ]; then
    _ok "mtl_source_code = ${mtl_source_code}"

    # Verify the checked-out tag matches MTL_TAG
    MTL_ACTUAL_TAG=$(git -C "${mtl_source_code}" describe --tags --exact-match 2>/dev/null || echo "unknown")
    if [ "${MTL_ACTUAL_TAG}" = "${MTL_TAG}" ]; then
        _ok "MTL tag verified: ${MTL_ACTUAL_TAG}"
    else
        _warn "MTL tag mismatch: expected '${MTL_TAG}', got '${MTL_ACTUAL_TAG}'."
        record_failure "MTL tag mismatch: expected ${MTL_TAG}, got ${MTL_ACTUAL_TAG}"
    fi
else
    record_failure "MTL source directory not found: ${mtl_source_code}"
    _error "MTL source unavailable – DPDK patch and MTL build steps will likely fail."
fi

# ══════════════════════════════════════════════════════════════════════════════
# STEP 3 – DPDK: Get source
# ══════════════════════════════════════════════════════════════════════════════
_section "Step 3 – Clone DPDK v25.11"

rm -rf "${DPDK_DIR}"
run_cmd "git clone DPDK" \
    git clone https://github.com/DPDK/dpdk.git "${DPDK_DIR}"

if cd "${DPDK_DIR}"; then
    DPDK_TAG="v25.11"
    run_cmd "git checkout DPDK ${DPDK_TAG}" \
        git checkout "${DPDK_TAG}"

    # Create a local branch to hold the patches cleanly (fresh clone, always new).
    run_cmd "git switch -c ${DPDK_TAG}" \
        git switch -c "${DPDK_TAG}"

    DPDK_VER=$(git describe --tags 2>/dev/null || echo "unknown")
    _info "DPDK version after checkout: ${DPDK_VER}"

    # Verify the expected tag is reachable from HEAD (fresh clone: HEAD should match tag exactly).
    DPDK_COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
    DPDK_TAG_COMMIT=$(git rev-parse --short "refs/tags/${DPDK_TAG}^{commit}" 2>/dev/null || echo "")
    if [ "${DPDK_COMMIT}" = "${DPDK_TAG_COMMIT}" ]; then
        _ok "DPDK HEAD (${DPDK_COMMIT}) matches tag ${DPDK_TAG} exactly."
    elif git merge-base --is-ancestor "refs/tags/${DPDK_TAG}" HEAD 2>/dev/null; then
        _ok "DPDK tag ${DPDK_TAG} (${DPDK_TAG_COMMIT}) is ancestor of HEAD (${DPDK_COMMIT}) – patches applied on top."
    else
        _warn "DPDK HEAD (${DPDK_COMMIT}) does NOT descend from tag ${DPDK_TAG} commit (${DPDK_TAG_COMMIT})."
        record_failure "DPDK checkout revision mismatch for ${DPDK_TAG}"
    fi

    cd "${BASE_DIR}"
else
    record_failure "cd into DPDK directory: ${DPDK_DIR} not accessible"
    _error "Cannot enter ${DPDK_DIR} – Step 3 git checkout skipped."
fi

# ══════════════════════════════════════════════════════════════════════════════
# STEP 4 – Apply MTL patches to DPDK
# ══════════════════════════════════════════════════════════════════════════════
_section "Step 4 – Apply MTL DPDK patches"

PATCH_DIR="${mtl_source_code}/patches/dpdk/25.11"

if [ -d "${PATCH_DIR}" ]; then
    PATCH_COUNT=$(find "${PATCH_DIR}" -maxdepth 1 -name '*.patch' | wc -l)
    _info "Found ${PATCH_COUNT} patch(es) in ${PATCH_DIR}"

    if [ "${PATCH_COUNT}" -gt 0 ]; then
        if cd "${DPDK_DIR}"; then
            # Pre-expand the glob into an array so run_cmd receives individual args.
            DPDK_PATCHES=("${PATCH_DIR}"/*.patch)

            # Idempotence check: if the patches apply cleanly, proceed with git am.
            # If not, check whether they are already applied (reverse check).
            # Only skip silently when patches are confirmed already applied;
            # otherwise record a failure so the operator knows manual action is needed.
            if git apply --check "${DPDK_PATCHES[@]}" >/dev/null 2>&1; then
                _info "DPDK patches appear applicable. Proceeding with git am."
                run_cmd "git am DPDK patches" \
                    git am --3way "${DPDK_PATCHES[@]}"

                PATCH_RC=$?
                if [ ${PATCH_RC} -ne 0 ]; then
                    _warn "git am reported errors. Attempting git am --abort to clean state."
                    git am --abort 2>/dev/null || true
                    _error "Patches did not apply cleanly. Manual intervention may be needed."
                else
                    _ok "All ${PATCH_COUNT} DPDK patch(es) applied successfully."
                    _info "Last ${PATCH_COUNT} commit(s) in DPDK after patching:"
                    git log --oneline -n "${PATCH_COUNT}" 2>/dev/null || true
                fi
            elif git apply --reverse --check "${DPDK_PATCHES[@]}" >/dev/null 2>&1; then
                _ok "DPDK patches already applied – skipping git am."
            else
                record_failure "DPDK patches: cannot apply and not detected as already applied; manual intervention may be needed"
                _error "Patches are neither applicable nor detected as already applied. Check DPDK tree in ${DPDK_DIR}."
            fi
            cd "${BASE_DIR}"
        else
            record_failure "cd into DPDK for patching: ${DPDK_DIR} not accessible"
            _error "Cannot enter ${DPDK_DIR} – patch step skipped."
        fi
    else
        _warn "Patch directory exists but contains no .patch files – skipping."
    fi
else
    _warn "Patch directory not found: ${PATCH_DIR}"
    record_failure "MTL patch directory missing: ${PATCH_DIR}"
fi

# ══════════════════════════════════════════════════════════════════════════════
# STEP 5 – Build and install DPDK
# ══════════════════════════════════════════════════════════════════════════════
_section "Step 5 – Build and install DPDK"

if cd "${DPDK_DIR}"; then
    # Remove stale build dir so meson setup is clean on re-runs
    if [ -d build ]; then
        _info "Removing stale DPDK build directory for clean rebuild."
        run_cmd "remove stale DPDK build dir" rm -rf build
    fi

    # Compose meson setup flags
    MESON_FLAGS=""
    if [ -n "${PORTABLE_ARCH}" ]; then
        MESON_FLAGS="-Dcpu_instruction_set=${PORTABLE_ARCH}"
        _info "Building DPDK with portable arch: ${PORTABLE_ARCH}"
    else
        _info "Building DPDK with native CPU optimisations."
    fi

    run_cmd "meson setup DPDK build" \
        meson setup build ${MESON_FLAGS:+"${MESON_FLAGS}"}

    # Verify libnuma was found (important for MTL performance)
    if [ -f build/build.ninja ]; then
        _ok "build/build.ninja created successfully."
        if grep -q "numa found: YES" meson-logs/meson-log.txt 2>/dev/null || \
           grep -rq "Library numa found: YES" build/ 2>/dev/null; then
            _ok "libnuma detected by DPDK meson."
        else
            _warn "Could not confirm 'Library numa found: YES' in meson logs. Check manually."
        fi
    else
        record_failure "meson setup did not produce build/build.ninja"
    fi

    run_cmd "ninja build DPDK" \
        ninja -C build

    run_cmd "ninja install DPDK" \
        sudo ninja install -C build

    cd "${BASE_DIR}"
else
    record_failure "cd into DPDK for build: ${DPDK_DIR} not accessible"
    _error "Cannot enter ${DPDK_DIR} – DPDK build skipped."
fi

# ── Fix PKG_CONFIG_PATH if libdpdk.pc was installed to a non-standard path ────
_section "Step 5b – Configure PKG_CONFIG_PATH for libdpdk"

DPDK_PC=$(find /usr /usr/local -name libdpdk.pc 2>/dev/null | head -n1)
if [ -n "${DPDK_PC}" ]; then
    DPDK_PC_DIR=$(dirname "${DPDK_PC}")
    _ok "Found libdpdk.pc at: ${DPDK_PC}"
    export PKG_CONFIG_PATH="${DPDK_PC_DIR}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
    _info "PKG_CONFIG_PATH updated to include: ${DPDK_PC_DIR}"

    # Persist for the current user only when MTL_UPDATE_BASHRC=1 is set,
    # to avoid unintended side effects in CI or shared environments.
    PROFILE_LINE="export PKG_CONFIG_PATH=\"${DPDK_PC_DIR}\${PKG_CONFIG_PATH:+:\${PKG_CONFIG_PATH}}\""
    if [ -n "${MTL_UPDATE_BASHRC:-}" ] && [ "${MTL_UPDATE_BASHRC}" != "0" ]; then
        if ! grep -qF "${DPDK_PC_DIR}" "${HOME}/.bashrc" 2>/dev/null; then
            echo "# MTL/DPDK PKG_CONFIG_PATH" >> "${HOME}/.bashrc"
            echo "${PROFILE_LINE}"           >> "${HOME}/.bashrc"
            _ok "PKG_CONFIG_PATH appended to ~/.bashrc for future sessions (MTL_UPDATE_BASHRC enabled)."
        fi
    else
        _info "Skipping persistent PKG_CONFIG_PATH update to ~/.bashrc. Set MTL_UPDATE_BASHRC=1 to enable."
    fi
else
    _warn "libdpdk.pc not found – PKG_CONFIG_PATH not updated."
    record_failure "libdpdk.pc not found after DPDK install"
fi

# Verify DPDK pkg-config
verify_pkg "libdpdk" "DPDK install"

# Update ldconfig cache so DPDK shared libs are found
run_cmd "ldconfig update after DPDK install" sudo ldconfig

# Note: DPDK does not install a single 'libdpdk.so'; it installs hundreds of
# individual librte_* shared libraries.  The pkg-config check above (libdpdk)
# is the correct way to verify the installation; an ldcache grep for 'libdpdk'
# would always fail.  Instead, verify at least some librte_* libs are present.
RTE_LIB_COUNT=$(ldconfig -p 2>/dev/null | grep -c 'librte_' || true)
if [ "${RTE_LIB_COUNT:-0}" -gt 0 ]; then
    _ok "Verified DPDK libraries in ldcache: ${RTE_LIB_COUNT} librte_* entries found."
else
    _warn "No librte_* libraries found in ldconfig cache after DPDK install."
    record_failure "Post-install check: DPDK librte_* libraries missing from ldcache"
fi

# ══════════════════════════════════════════════════════════════════════════════
# STEP 6 – Build Media Transport Library
# ══════════════════════════════════════════════════════════════════════════════
_section "Step 6 – Build Media Transport Library and sample apps"

if [ ! -d "${mtl_source_code}" ]; then
    _error "MTL source directory not found at ${mtl_source_code}. Skipping MTL build."
    record_failure "MTL build skipped – source directory missing"
else
    if cd "${mtl_source_code}"; then
        if [ ! -f "./build.sh" ]; then
            _error "build.sh not found in ${mtl_source_code}."
            record_failure "MTL build.sh not found"
        else
            chmod +x ./build.sh

            run_cmd "MTL build.sh" \
                env "PKG_CONFIG_PATH=${PKG_CONFIG_PATH}" ./build.sh

            MTL_BUILD_RC=$?
            if [ ${MTL_BUILD_RC} -ne 0 ]; then
                _warn "MTL build.sh exited with errors. See output above."
            else
                # Look for the build output directory
                if [ -d "${mtl_source_code}/build" ]; then
                    _ok "MTL build directory created: ${mtl_source_code}/build"
                    LIB_COUNT=$(find "${mtl_source_code}/build" -name '*.so' -o -name '*.a' 2>/dev/null | wc -l)
                    _info "Shared/static libraries in build output: ${LIB_COUNT}"
                fi

                # Run ldconfig so the newly installed MTL libs are discoverable
                run_cmd "ldconfig after MTL install" sudo ldconfig

                # Verify MTL library and pkg-config module
                if ldconfig -p 2>/dev/null | grep "libmtl" > /dev/null; then
                    _ok "Verified library in ldcache: libmtl"
                else
                    _warn "libmtl not found in ldconfig cache – MTL may not have installed shared libs."
                    record_failure "Post-install check: libmtl missing after MTL build"
                fi

                if pkg-config --exists mtl 2>/dev/null; then
                    MTL_PC_VER=$(pkg-config --modversion mtl 2>/dev/null || echo "unknown")
                    _ok "Verified pkg-config module: mtl (version ${MTL_PC_VER})"
                else
                    _warn "pkg-config module 'mtl' not found – PKG_CONFIG_PATH may need updating."
                    record_failure "Post-install check: pkg-config 'mtl' missing after MTL build"
                fi
            fi
        fi

        cd "${BASE_DIR}"
    else
        record_failure "cd into MTL source: ${mtl_source_code} not accessible"
        _error "Cannot enter ${mtl_source_code} – MTL build skipped."
    fi
fi

# ══════════════════════════════════════════════════════════════════════════════
# STEP 7 – FFmpeg + MTL plugin
# Ref: ecosystem/ffmpeg_plugin/README.md
# ══════════════════════════════════════════════════════════════════════════════
_section "Step 7 – FFmpeg + MTL plugin"

FFMPEG_BUILD_DIR="${WORKSPACE_DIR}/ffmpeg_build"
mkdir -p "${FFMPEG_BUILD_DIR}"

# ── Step 7.0  apt prerequisites for FFmpeg build ──────────────────────────────
run_cmd "apt-get install FFmpeg build tools (wget patch unzip nasm)" \
    sudo apt-get install -y wget patch unzip nasm

for cmd in wget patch unzip nasm; do
    verify_cmd "${cmd}" "apt-get install FFmpeg build tools"
done

# ── Step 7.1  Confirm FFmpeg version ─────────────────────────────────────────
# FFMPEG_VERSION is pinned at the top of the script (currently: ${FFMPEG_VERSION}).
# We cross-check versions.env from the MTL repo to warn if they diverge, but
# we always build the pinned version – never override it from versions.env.
VERSIONS_ENV="${mtl_source_code}/versions.env"
if [ -f "${VERSIONS_ENV}" ]; then
    MTL_FFMPEG_VER=$(grep -E '^FFMPEG_VERSION=' "${VERSIONS_ENV}" | cut -d= -f2 | tr -d '"' | head -1)
    _info "versions.env reports FFMPEG_VERSION=${MTL_FFMPEG_VER:-<not set>}"
    if [ -n "${MTL_FFMPEG_VER}" ] && [ "${MTL_FFMPEG_VER}" != "${FFMPEG_VERSION}" ]; then
        _warn "versions.env recommends FFmpeg ${MTL_FFMPEG_VER} but this script is pinned to ${FFMPEG_VERSION}."
        _warn "To use a different version, edit the MTL_TAG/FFMPEG_VERSION variables at the top of this script."
    else
        _ok "FFmpeg version ${FFMPEG_VERSION} matches versions.env recommendation."
    fi
else
    _warn "versions.env not found at ${VERSIONS_ENV} – cannot cross-check FFmpeg version."
fi
_info "Building FFmpeg version: ${FFMPEG_VERSION}"

FFMPEG_PLUGIN_DIR="${mtl_source_code}/ecosystem/ffmpeg_plugin"

# ── Step 7.2  openh264 ────────────────────────────────────────────────────────
_info "--- 7.2  openh264 ---"
if ldconfig -p 2>/dev/null | grep "libopenh264" > /dev/null; then
    _ok "libopenh264 already installed – skipping source build."
else
    _info "Building openh264 from source (tag openh264v2.4.0)"
    cd "${FFMPEG_BUILD_DIR}"

    rm -rf openh264
    run_cmd "git clone openh264" \
        git clone https://github.com/cisco/openh264.git

    if [ -d "openh264" ]; then
        cd openh264
        run_cmd "git checkout openh264v2.4.0" \
            git checkout openh264v2.4.0
        run_cmd "make openh264" \
            make -j"$(nproc)"
        run_cmd "sudo make install openh264" \
            sudo make install
        run_cmd "ldconfig after openh264 install" \
            sudo ldconfig
        verify_lib "libopenh264" "openh264 source build"
        cd "${FFMPEG_BUILD_DIR}"
    else
        record_failure "openh264 source directory missing after clone"
    fi

    cd "${BASE_DIR}"
fi

# ── Step 7.3  Clone FFmpeg and checkout the release branch ───────────────────
_info "--- 7.3  FFmpeg clone and checkout ---"

if ! cd "${FFMPEG_BUILD_DIR}"; then
    record_failure "Cannot enter FFmpeg build directory: ${FFMPEG_BUILD_DIR}"
    _error "Skipping FFmpeg clone and build because FFmpeg build directory is not accessible."
else

rm -rf FFmpeg
run_cmd "git clone FFmpeg" \
    git clone https://github.com/FFmpeg/FFmpeg.git

if [ ! -d "FFmpeg" ]; then
    record_failure "FFmpeg source directory missing after clone"
    _error "Cannot proceed with FFmpeg build."
else
    cd FFmpeg

    run_cmd "git checkout FFmpeg release/${FFMPEG_VERSION}" \
        git checkout "release/${FFMPEG_VERSION}"

    # ── Step 7.4  Apply MTL patch ─────────────────────────────────────────────
    _info "--- 7.4  Apply MTL FFmpeg patch (${FFMPEG_VERSION}/*.patch) ---"
    FFMPEG_PATCH_DIR="${FFMPEG_PLUGIN_DIR}/${FFMPEG_VERSION}"

    if [ -d "${FFMPEG_PATCH_DIR}" ]; then
        FPATCH_COUNT=$(find "${FFMPEG_PATCH_DIR}" -maxdepth 1 -name '*.patch' | wc -l)
        _info "Found ${FPATCH_COUNT} FFmpeg patch(es) in ${FFMPEG_PATCH_DIR}"

        if [ "${FPATCH_COUNT}" -gt 0 ]; then
            # Pre-expand the glob into an array so run_cmd receives individual args.
            FFMPEG_PATCHES=("${FFMPEG_PATCH_DIR}"/*.patch)

            # Idempotence check: if the patches apply cleanly, proceed with git am.
            # If not, check whether they are already applied (reverse check).
            if git apply --check "${FFMPEG_PATCHES[@]}" >/dev/null 2>&1; then
                _info "FFmpeg patches appear applicable. Proceeding with git am."
                run_cmd "git am FFmpeg MTL patches" \
                    git am --3way "${FFMPEG_PATCHES[@]}"
                if [ $? -ne 0 ]; then
                    _warn "git am failed – attempting git am --abort to restore clean state."
                    git am --abort 2>/dev/null || true
                    record_failure "FFmpeg MTL patches did not apply cleanly for version ${FFMPEG_VERSION}"
                else
                    _ok "All ${FPATCH_COUNT} FFmpeg MTL patch(es) applied successfully."
                    _info "Last ${FPATCH_COUNT} commit(s) in FFmpeg after patching:"
                    git log --oneline -n "${FPATCH_COUNT}" 2>/dev/null || true
                fi
            elif git apply --reverse --check "${FFMPEG_PATCHES[@]}" >/dev/null 2>&1; then
                _ok "FFmpeg MTL patches already applied – skipping git am."
            else
                record_failure "FFmpeg patches: cannot apply and not detected as already applied; manual intervention may be needed"
                _error "FFmpeg patches are neither applicable nor detected as already applied. Check FFmpeg tree."
            fi
        else
            _warn "FFmpeg patch directory exists but has no .patch files."
            record_failure "No .patch files in ${FFMPEG_PATCH_DIR}"
        fi
    else
        _warn "FFmpeg patch directory not found: ${FFMPEG_PATCH_DIR}"
        record_failure "FFmpeg patch directory missing: ${FFMPEG_PATCH_DIR}"
    fi

    # ── Step 7.5  Copy MTL in/out device source files ─────────────────────────
    _info "--- 7.5  Copy MTL device source files into libavdevice ---"

    MTL_C_FILES=( "${FFMPEG_PLUGIN_DIR}"/mtl_*.c )
    MTL_H_FILES=( "${FFMPEG_PLUGIN_DIR}"/mtl_*.h )

    if [ -e "${MTL_C_FILES[0]}" ]; then
        run_cmd "copy MTL .c files to libavdevice" \
            cp -rf "${MTL_C_FILES[@]}" libavdevice/
        C_COUNT=$(find libavdevice -maxdepth 1 -name 'mtl_*.c' | wc -l)
        _info "MTL .c files copied: ${C_COUNT}"
    else
        _warn "No mtl_*.c files found in ${FFMPEG_PLUGIN_DIR}"
        record_failure "MTL .c plugin files missing – plugin will not build"
    fi

    if [ -e "${MTL_H_FILES[0]}" ]; then
        run_cmd "copy MTL .h files to libavdevice" \
            cp -rf "${MTL_H_FILES[@]}" libavdevice/
        H_COUNT=$(find libavdevice -maxdepth 1 -name 'mtl_*.h' | wc -l)
        _info "MTL .h files copied: ${H_COUNT}"
    else
        _warn "No mtl_*.h files found in ${FFMPEG_PLUGIN_DIR} – may be expected for this version."
    fi

    # ── Step 7.6  Configure FFmpeg with MTL enabled ───────────────────────────
    _info "--- 7.6  Configure FFmpeg ---"
    # PKG_CONFIG_PATH must be passed as an environment variable prefix so that
    # ./configure and the pkg-config invocations it spawns can see it.
    run_cmd "FFmpeg ./configure --enable-mtl" \
        env "PKG_CONFIG_PATH=${PKG_CONFIG_PATH}" ./configure \
            --enable-shared \
            --disable-static \
            --enable-nonfree \
            --enable-pic \
            --enable-gpl \
            --enable-libopenh264 \
            --enable-encoder=libopenh264 \
            --enable-mtl

    # Verify configure produced a Makefile (prerequisite for make)
    if [ -f Makefile ]; then
        _ok "FFmpeg Makefile generated by ./configure."
        # Confirm MTL was enabled in the final config
        if grep -q 'CONFIG_MTL=yes\|enable-mtl' config.h ffbuild/config.mak 2>/dev/null; then
            _ok "MTL confirmed enabled in FFmpeg config."
        else
            _warn "Could not confirm MTL=yes in FFmpeg config files – check ./configure output above."
            record_failure "MTL flag not confirmed in FFmpeg ./configure output"
        fi
    else
        record_failure "FFmpeg Makefile not found after ./configure – configure likely failed"
        _error "Skipping FFmpeg make steps as configure did not produce a Makefile."
    fi

    # ── Step 7.7  Build FFmpeg ─────────────────────────────────────────────────
    _info "--- 7.7  Build FFmpeg ---"
    run_cmd "make FFmpeg" \
        make -j"$(nproc)"

    # ── Step 7.8  Install FFmpeg ───────────────────────────────────────────────
    _info "--- 7.8  Install FFmpeg ---"
    run_cmd "sudo make install FFmpeg" \
        sudo make install

    run_cmd "ldconfig after FFmpeg install" \
        sudo ldconfig

    # ── Post-install verification ─────────────────────────────────────────────
    FFMPEG_BIN=$(command -v ffmpeg 2>/dev/null || echo "")
    if [ -n "${FFMPEG_BIN}" ]; then
        _ok "ffmpeg binary found: ${FFMPEG_BIN}"
        FFMPEG_VER_OUT=$(ffmpeg -version 2>&1 | head -1)
        _info "${FFMPEG_VER_OUT}"

        # Verify installed version matches the branch we built
        if echo "${FFMPEG_VER_OUT}" | grep "${FFMPEG_VERSION}" > /dev/null; then
            _ok "FFmpeg version string contains expected release '${FFMPEG_VERSION}'."
        else
            _warn "FFmpeg version string does not contain '${FFMPEG_VERSION}'. Got: ${FFMPEG_VER_OUT}"
            record_failure "FFmpeg version mismatch: expected release ${FFMPEG_VERSION}"
        fi

        # Verify the MTL device is compiled in
        if ffmpeg -devices 2>/dev/null | grep -E 'mtl_st|mtl_' > /dev/null; then
            _ok "MTL devices visible in ffmpeg -devices output."
            _info "MTL devices detected:"
            ffmpeg -devices 2>/dev/null | grep -E 'mtl_' || true
        else
            _warn "MTL devices NOT visible in 'ffmpeg -devices'. Plugin may not have been compiled in."
            record_failure "FFmpeg MTL plugin not detected in ffmpeg -devices"
        fi

        # Verify libopenh264 encoder is available
        if ffmpeg -encoders 2>/dev/null | grep 'libopenh264' > /dev/null; then
            _ok "libopenh264 encoder available in ffmpeg."
        else
            _warn "libopenh264 encoder not found in ffmpeg -encoders."
            record_failure "libopenh264 encoder missing from FFmpeg build"
        fi
    else
        record_failure "ffmpeg binary not found after install"
        _warn "FFmpeg binary not found in PATH after install."
    fi

    cd "${BASE_DIR}"
fi

fi # end: if ! cd "${FFMPEG_BUILD_DIR}"

# ── Cleanup: remove the entire build workspace ──────────────────────────────
_section "Cleanup – removing build workspace"
cd "${BASE_DIR}"
if [ -n "${WORKSPACE_DIR:-}" ] && [ -d "${WORKSPACE_DIR}" ]; then
    _info "Removing build workspace: ${WORKSPACE_DIR}"
    rm -rf "${WORKSPACE_DIR}"
    _ok "Build workspace removed. All installed artifacts remain system-wide."
else
    _warn "Workspace directory not found – nothing to clean."
fi

# ══════════════════════════════════════════════════════════════════════════════
# STEP 8 – Final summary
# ══════════════════════════════════════════════════════════════════════════════
_section "Build Summary"

_info "PKG_CONFIG_PATH = ${PKG_CONFIG_PATH}"
_info "mtl_source_code = ${mtl_source_code}"
echo ""

# ── Installed component versions ──────────────────────────────────────────────
echo -e "${BOLD}Installed versions:${RESET}"

MTL_VER=$(pkg-config --modversion mtl 2>/dev/null || echo "not found")
echo -e "  MTL    : ${CYAN}${MTL_VER}${RESET}"

DPDK_VER=$(pkg-config --modversion libdpdk 2>/dev/null || echo "not found")
echo -e "  DPDK   : ${CYAN}${DPDK_VER}${RESET}"

FFMPEG_VER=$(ffmpeg -version 2>/dev/null | head -1 | awk '{print $3}' || echo "not found")
echo -e "  FFmpeg : ${CYAN}${FFMPEG_VER}${RESET}"

echo ""

if [ ${#FAILED_STEPS[@]} -eq 0 ]; then
    echo -e "${GREEN}${BOLD}All steps completed successfully.${RESET}"
else
    echo -e "${YELLOW}${BOLD}Build completed with ${#FAILED_STEPS[@]} failure(s):${RESET}"
    for step in "${FAILED_STEPS[@]}"; do
        echo -e "  ${RED}✗${RESET}  ${step}"
    done
    echo ""
    echo -e "${YELLOW}Review the output above for details. The script continued past each failure."
    echo -e "Resolve the listed issues and re-run. Each run uses a fresh workspace so"
    echo -e "nothing is reused from previous attempts.${RESET}"
fi

echo ""
_info "Next steps: see https://github.com/OpenVisualCloud/Media-Transport-Library/blob/v26.01/doc/run.md"
