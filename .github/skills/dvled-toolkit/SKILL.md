---
name: dvled-toolkit
description: 'Build, configure, run, test, fuzz and debug the Direct View LED toolkit (dvledtx ST 2110-20 transmitter, dvledrx X11 receiver). Use when: editing dvledtx/dvledrx C sources; writing or validating TX/RX JSON configs; meson/ninja build or enable_mtl_tx option; running cmocka unit tests or coverage; fuzzing config_reader; hugepages, DPDK/VFIO NIC binding with nicctl.sh; PTP vs no-PTP pacing; MTL init failures; av_write_frame ret=-5, video_trs_burst_fail, st20_tx_queue_fatal_error, unrecoverable_error; E610/ixgbe link reset; RX flicker, no frames, multicast/IGMP; MTL source lookup and MTL change policy; screen capture / x11grab; VA-API hwaccel; pixel formats (gbrp12le, yuv422p10le, ...).'
argument-hint: 'Task, e.g. "add a config field", "run tests", "why does MTL init fail"'
---

# Direct View LED Software Toolkit

Simplified SMPTE ST 2110-20 uncompressed video pipeline for tiled LED walls, built on the Media Transport Library (MTL) + DPDK + FFmpeg.

| App | Role | Entry point |
|-----|------|-------------|
| `dvledtx` | Decode file / X11 screen with FFmpeg, crop into N tiles, send each as an ST 2110-20 RTP session | [dvledtx/src/main.c](../../../dvledtx/src/main.c) |
| `dvledrx` | Receive ST 2110-20 sessions via MTL, convert, render to X11 windows/outputs (Xorg only, no Wayland) | [dvledrx/src/main.c](../../../dvledrx/src/main.c) |

Deployment assumptions (air-gapped TX host, dedicated L2 media/PTP switch) are in [docs/assumptions.md](../../../docs/assumptions.md) and [Security.md](../../../Security.md). Do not add network management, remote control, or SSH-related features.

## Source Map

**dvledtx** (headers mirror `src/` under `dvledtx/include/`)
- `src/main.c` — getopt parsing, signal handling, main TX loop
- `src/util/config_reader.c` — **custom** minimal JSON parser (no external JSON lib); all config validation
- `src/util/logger.c` — file/console logging
- `src/core/session_manager.c` — ST 2110-20 TX session lifecycle
- `src/ffmpeg/ffmpeg_decoder.c` — open file / x11grab, decode, VA-API with CPU fallback
- `src/ffmpeg/ffmpeg_frame_handler.c` — sws_scale, crop, pixel-format conversion
- `src/ffmpeg/ffmpeg_tx.c` — FFmpeg `mtl_st20p` muxer path (default)
- `src/mtl/mtl_tx.c` — direct MTL `st20p_tx_get_frame`/`st20p_tx_put_frame` path (`ENABLE_MTL_TX` only)

**dvledrx** (flat layout, headers in `dvledrx/include/`)
- `src/config_reader.c` — JSON parsing via **libjson-c**
- `src/session_manager.c` — MTL `st20p_rx_*` sessions
- `src/frame_converter.c` — YUV/RGB conversion, crop, scale
- `src/x11_handler.c` — X11/XRandR outputs, windows, rendering
- `src/logger.c` — logging, honours `RXAPP_LOG_LEVEL`

TX and RX config readers are independent implementations. A schema change usually needs edits in the reader, the context struct (`dvledtx/include/app_context.h` or `dvledrx/include/config_reader.h`), unit tests, fuzz corpus, sample configs and README.

## MTL: Always Use as Context, Do Not Modify

Both apps are thin layers over the Media Transport Library (MTL). Most runtime behaviour (pacing, PTP, queues, multicast, NIC drivers) lives in MTL, so **always read the MTL source before explaining or fixing a runtime issue**. The checkout is the sibling directory `../Media-Transport-Library`.

1. **Match the version first.** The checkout must be the version that is actually installed. Compare `git -C ../Media-Transport-Library describe --tags --always --dirty` with `pkg-config --modversion mtl`. If they differ, say so before quoting MTL code.
2. **Trace log lines to their source.** Look up any `MTL: ...` message there, for example `grep -rn "hang duration" ../Media-Transport-Library/lib/src`.
3. **Where to look:**

| Topic | MTL path |
|-------|----------|
| Public API used by dvledtx/dvledrx | `include/mtl_api.h`, `include/st_pipeline_api.h`, `include/st20_api.h` |
| FFmpeg `mtl_st20p` muxer and AVOptions (default TX path) | `ecosystem/ffmpeg_plugin/mtl_st20p_tx.c`, `ecosystem/ffmpeg_plugin/mtl_common.c` |
| TX/RX video sessions, pacing, queue recovery | `lib/src/st2110/st_tx_video_session.c`, `lib/src/st2110/st_rx_video_session.c`, `lib/src/st2110/pipeline/` |
| NIC driver table, timesync, pacing selection | `lib/src/dev/mt_dev.c` |
| PTP client | `lib/src/mt_ptp.c` |
| Multicast join / IGMP | `lib/src/mt_mcast.c` |
| NIC binding, VFs | `script/nicctl.sh` |
| Build / run setup | `doc/build.md`, `doc/run.md` |

4. **Do not change MTL files.** Fix or work around the issue in this order, and stop at the first that works:
   1. config or environment (hugepages, binding, link reset, switch settings);
   2. dvledtx/dvledrx code, using only the public MTL API and plugin AVOptions;
   3. a different MTL commit or build flag, as advice to the user;
   4. only as a last resort, an MTL source change: present the diff and the reason, and **wait for explicit user approval** before editing.
   - Never edit, rebuild or install MTL (or DPDK) silently. Before finishing, confirm `git -C ../Media-Transport-Library status --short` shows only approved changes.
   - The `mt_dev.c` ixgbe timesync patch in known issue #4 is the only approved MTL change so far. It is applied by the user for E610 PTP.
   - Prefer reporting MTL bugs upstream (OpenVisualCloud/Media-Transport-Library) over carrying local patches.

## Build

Requires FFmpeg libs, MTL (`pkg-config --modversion mtl` → 26.09.x) and DPDK installed system-wide; dvledrx also needs X11, Xext, Xrandr, json-c, zlib.

```bash
bash scripts/build.sh                       # default: FFmpeg mtl_st20p TX path
bash scripts/build.sh -Denable_mtl_tx=true  # direct MTL pipeline TX path
```

- `build.sh` deletes and recreates `build/`, then checks `build/dvledtx/dvledtx` and `build/dvledrx/dvledrx` exist.
- Only meson option: `enable_mtl_tx` (bool, default `false`) → adds `-DENABLE_MTL_TX`. Guard MTL-direct code with `#ifdef ENABLE_MTL_TX` and keep both paths compiling.
- Binaries are stripped (`-s`) and built with `_FORTIFY_SOURCE=2`; use a separate meson builddir without `-s` for gdb.

## Test

```bash
bash scripts/test.sh                # builds build-tests/ with -Db_coverage=true, runs meson test, gcovr report
bash scripts/test.sh --no-coverage
meson test -C build-tests <name> --print-errorlogs   # single test after a test.sh run
```

- Framework: **cmocka**; definitions in [dvledtx/tests/meson.build](../../../dvledtx/tests/meson.build) and [dvledrx/tests/meson.build](../../../dvledrx/tests/meson.build).
- No NIC/hugepages needed: TX tests mock MTL/FFmpeg via `-Wl,--wrap=` and `*_mock.c` / `*_stub.c`; RX tests link [dvledrx/tests/fake_mtl.c](../../../dvledrx/tests/fake_mtl.c).
- `test_main` `#include`s `main.c` directly to test CLI parsing.
- X11 tests self-skip without a display. Screen-capture success tests need `DVLED_TEST_DISPLAY="<display>+x,y"` (auto-detected by `test.sh` via `xdpyinfo` on `$DISPLAY`, `:99`, `:0`).
- Coverage excludes `main.c`. Add/extend a cmocka test for every behaviour change.

## Fuzz

Config parsers are the main attack surface. Harnesses exist per app:
- `dvledtx/fuzz/fuzz_config_reader.c` (AFL), `fuzz_config_reader_libfuzzer.c` (libFuzzer)
- `dvledrx/fuzz/` same pair plus `fuzz_config_reader_common.h`
- Seeds in each `fuzz/corpus/`. When adding a config field, add a corpus seed that exercises it.

## Run on Hardware

1. VFIO + IOMMU enabled (`intel_iommu=on iommu=pt`), user in `vfio` group — see README "IOMMU / VFIO Kernel Parameters" and "VFIO Group Setup".
2. Hugepages: `sudo sysctl -w vm.nr_hugepages=2048` (4096 for multi-session/4K); verify `grep HugePages_ /proc/meminfo`.
3. Bind NIC to PMD using MTL's script: `sudo <MTL>/script/nicctl.sh bind_pmd <BDF>`; check `readlink /sys/bus/pci/devices/<BDF>/driver` → `vfio-pci`. On E610/ixgbe, do `bind_kernel`, `sleep 3`, then `bind_pmd` before **every** run, or the second run hangs (known issue #1). Hugepages and binding are lost on reboot.
4. Run (root required for DPDK):
   ```bash
   sudo ./build/dvledtx/dvledtx --config dvledtx/config/tx_fullhd_single_session.json [-t 60]
   sudo ./build/dvledrx/dvledrx --config dvledrx/config/rx_fullhd_1session.json [-t 60]
   ```
5. Force stop: `sudo pkill -9 -f dvledtx`.

CLI (both apps): `-C/--config <file>` (required), `-t/--test-time <secs>` (1..86400), `-v/--version`, `--help`.

## Config Essentials

Samples: [dvledtx/config/](../../../dvledtx/config/), [dvledrx/config/](../../../dvledrx/config/). Full field docs: README "JSON Configuration".

| Area | TX keys | RX keys |
|------|---------|---------|
| NICs | `interfaces[]`: `name` (PCI BDF), `sip`, `dip`, `nic_index` | `interfaces[]`: `name`, `SIP`, `DIP` (upper-case) |
| Source / format | `video`: `width`, `height`, `input_mode` (`file`/`screen_capture`), `tx_url`, `screen_input`; `tx_video`: `scale_width`, `scale_height`, `fps`, `fmt`; `hwaccel` | `video`: `width`, `height`, `fps`, `fmt` |
| Sessions | `tx_sessions[]`: `nic_index`, `udp_port`, `payload_type` (96), `crop{x,y,w,h}` | `rx_sessions[]`: `udp_port`, `payload_type`, `crop`, `scale`, `window`, `output`; optional `displays[]`, `display_mode` |
| Timing | `ptp{enable,pi,unicast}` | `ptp{...}`, `tasklet_sleep` |
| Logging | `log_file` (else `LOG_FILE` env, else console) | `log_file`, `log_level` (or `RXAPP_LOG_LEVEL`) |

Rules to preserve:
- TX `fmt`, `fps`, `udp_port`, `payload_type` and per-session `crop.w/h` must match the RX side, otherwise RX shows no frames.
- Formats: `yuv422p10le`, `yuv420`, `yuv444p10le`, `gbrp10le`, `yuv422p12le`, `yuv444p12le`, `gbrp12le`. FPS: 25, 30, 50, 60.
- Crop rectangles must lie within the (scaled) source frame.
- `ptp.enable=false` → free-running pacing (use `*_noptp.json`); `true` needs a PTP grandmaster on the media network.
- Symlinked input files are rejected by design — do not relax this.

## Debugging Checklist

| Symptom | Check |
|---------|-------|
| MTL init failed | BDF matches `lspci`; NIC on `vfio-pci`; `ldconfig -p \| grep mtl`; `dmesg \| grep -i iommu` |
| Hugepage / mempool errors | `HugePages_Free` > 0; raise `vm.nr_hugepages` |
| Permission denied on `/dev/vfio` | `vfio` group membership, run with `sudo` |
| Non-zero exit at start (e.g. 255) | Read `log_file` (e.g. `dvledtx.log`): config validation, MTL/EAL init, or input open failure |
| Screen capture fails | Must be Xorg; on headless use Xvfb (`DISPLAY=:99`), see README |
| Hardware decode falls back to CPU | FFmpeg built with VA-API, `vainfo` works, `/dev/dri` access |
| RX black / no frames | TX↔RX format, port, multicast `dip`, PTP setting mismatch; IGMP snooping on switch |
| RX flickers / tears | Two TX on the same `dip:udp_port` (check other hosts) |
| `av_write_frame failed (ret=-5)`, `fps 0` | Symptom only; find the first `video_trs_burst_fail` / `unrecoverable_error` above it |

**Before diagnosing any hardware run, read [known issues](./references/known-issues.md).** It covers the E610 TX hang and link reset, PTP on ixgbe, VF vs PF, reboot recovery, 12-bit plugin rebuilds, hidden CI link errors, dvledrx under sudo, RX flicker from duplicate `dip:udp_port`, stale frames, IGMP snooping, E610 pacing limits, and TX/RX format mismatch.

Agent notes:
- Runs need `sudo` with a password. Hand the exact command to the user instead of retrying or piping a password.
- Do not modify or rebuild MTL/DPDK unless the user asks (see **MTL: Always Use as Context, Do Not Modify**). Confirm which install is active first (`pkg-config --modversion mtl libdpdk`).

## Coding Conventions

- C (compiler-default std; TX `-O2`, RX `-O3`, both `-DALLOW_EXPERIMENTAL_API`), 2-space indent, braces on same line, `snake_case` functions/structs, `UPPER_CASE` macros.
- Every source/script starts with:
  ```c
  /* SPDX-License-Identifier: BSD-3-Clause
   * Copyright 2026 Intel Corporation
   */
  ```
- Return `0` on success, negative on failure; log via the app's logger, not raw `printf`.
- Bounds-check all config-derived values; free resources on every error path.
- CI ([.github/workflows/](../../../.github/workflows/)) runs build (both TX paths), smoke tests (`-v`, `--help`), unit tests, ShellCheck, cppcheck, zizmor, Trivy, checksec — keep changes clean for all of them.
