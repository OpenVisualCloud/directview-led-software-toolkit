<!-- SPDX-License-Identifier: BSD-3-Clause
      Copyright 2022 Intel Corporation
-->
# MTL Build Script – Step-by-Step Guide

**Script:** `build_mtl_ubuntu.sh`  
**Reference:** [MTL Build Guide v26.01](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/v26.01/doc/build.md)  
**Target OS:** Ubuntu / Debian (x86_64)

---

## Quick Start

```bash
# 1. Download or copy the script to your working directory
#    (the same directory where MTL, DPDK and FFmpeg will be cloned)

# 2. Make the script executable
chmod +x build_mtl_ubuntu.sh

# 3. Run (basic build, no display support)
./build_mtl_ubuntu.sh

# 4. Run with SDL2 display support (needed for RxTxApp)
./build_mtl_ubuntu.sh --with-sdl2
```

---

## Before You Run — Requirements & Pre-run Checklist

Work through every item below **before** executing the script.
The script's pre-flight checks (Step 0) will catch most of these automatically,
but resolving them upfront avoids avoidable failures.

### 1. Operating System
- Must be **Ubuntu 20.04 LTS or later** (22.04 and 24.04 are recommended).
- Architecture must be **x86_64**. ARM and other architectures are not supported by MTL/DPDK.

### 2. User Account & sudo
- Run as a **normal (non-root) user** that has `sudo` access.
- Verify sudo works before running:
  ```bash
  sudo -v
  ```
- If you see `sudo: command not found` or a permission error, ask your system administrator to grant sudo access.

### 3. Network Access
- The machine must have **outbound HTTPS access to github.com**.
- All source repositories are cloned from GitHub during the build.
- If you are behind a corporate proxy, set the following before running:
  ```bash
  export https_proxy=http://<proxy-host>:<port>
  export http_proxy=http://<proxy-host>:<port>
  export no_proxy=localhost,127.0.0.1
  # Also configure git to use the proxy:
  git config --global http.proxy http://<proxy-host>:<port>
  ```

### 4. Disk Space
- Ensure at least **20 GB of free disk space** on the filesystem where you run the script.
- Check available space:
  ```bash
  df -h .
  ```

### 5. Working Directory
- The script creates a **fresh, timestamped workspace** directory for every run:
  ```
  $BASE_DIR/mtl_build_workspace_YYYYMMDD_HHMMSS/
  ├── Media-Transport-Library/
  ├── dpdk/
  ├── src_deps/
  ├── ffmpeg_build/
  └── _pip_build_venv/
  ```
- Nothing is ever reused from previous runs — every clone and build is fresh.
- The workspace is **automatically removed** after all builds complete;
  only system-installed artifacts (under `/usr/local/`) persist.
- To use a different base directory, pass `--build-dir`:
  ```bash
  ./build_mtl_ubuntu.sh --build-dir /opt/mtl_build
  ```
- Ensure the chosen directory is **writable** by your user:
  ```bash
  ls -ld /opt/mtl_build   # should show your user as owner
  ```

### 6. Versions to Build (optional customisation)
The following versions are **pinned at the top of the script** and require no external input:

| Variable | Default | Where to change |
|---|---|---|
| `MTL_TAG` | `v26.01` | Line ~95 of `build_mtl_ubuntu.sh` |
| `FFMPEG_VERSION` | `7.0` | Line ~96 of `build_mtl_ubuntu.sh` |

> **Do not** set these as environment variables — edit the variables directly in the script.

### 7. Python pip — Isolated venv
The script installs Python build tools (`pyelftools`, `ninja`) in a **temporary Python
virtual environment** inside the workspace (`${WORKSPACE_DIR}/_pip_build_venv`) rather
than system-wide. This avoids any PEP 668 / externally-managed-environment issues and
leaves the system Python untouched. The venv is removed automatically when the entire
workspace is cleaned up at the end of the script.

### 8. SDL2 Display Support (optional)
Only required if you intend to use the **RxTxApp display output** feature.
Pass `--with-sdl2` when running the script:
```bash
./build_mtl_ubuntu.sh --with-sdl2
```

### 9. Re-running the Script
Every run creates a **fresh workspace** with new clones — nothing is ever reused from
previous runs, so there is no risk of stale or modified source trees affecting the build.
The workspace is automatically deleted at the end. Simply re-run the script:
```bash
./build_mtl_ubuntu.sh
```
System-installed artifacts from previous runs (DPDK, MTL, FFmpeg under `/usr/local/`)
are overwritten by the new `sudo make install` / `sudo ninja install` steps.

---

## Pinned Versions

| Component | Version |
|---|---|
| Media Transport Library | `v26.01` |
| DPDK | `v25.11` |
| FFmpeg | `7.0` (release branch) |

> To change versions, edit the `MTL_TAG` and `FFMPEG_VERSION` variables at the top of the script.

---

## Usage

```bash
chmod +x build_mtl_ubuntu.sh

# Basic build
./build_mtl_ubuntu.sh

# With SDL2 display support (for RxTxApp)
./build_mtl_ubuntu.sh --with-sdl2

# Skip gtest source build (use apt version only)
./build_mtl_ubuntu.sh --no-gtest

# Always build json-c / libpcap / gtest from source
./build_mtl_ubuntu.sh --force-src-deps

# Build DPDK for a portable target (AVX2, deployable to other machines)
./build_mtl_ubuntu.sh --portable haswell

# Build DPDK for AVX-512 portable target
./build_mtl_ubuntu.sh --portable skylake-avx512

# Override clone/build directories
./build_mtl_ubuntu.sh --build-dir /opt/mtl_build
./build_mtl_ubuntu.sh --mtl-dir /opt/Media-Transport-Library
```


## References

- [MTL Build Guide v26.01](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/v26.01/doc/build.md)
- [MTL FFmpeg Plugin README](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/v26.01/ecosystem/ffmpeg_plugin/README.md)
- [MTL Run Guide v26.01](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/v26.01/doc/run.md)
- [DPDK v25.11 Release](https://github.com/DPDK/dpdk/tree/v25.11)
- [FFmpeg release/7.0](https://github.com/FFmpeg/FFmpeg/tree/release/7.0)
