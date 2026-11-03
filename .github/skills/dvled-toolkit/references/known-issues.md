# Known Issues and Solutions

Issues hit on real hardware (Intel E610 `8086:57b0` / ixgbe PF, MTL 26.09, DPDK 26.07, Ubuntu 24.04). Always find the **first** error in the log; later lines are usually consequences.

`<MTL>` = Media-Transport-Library checkout, `<BDF>` = NIC PCI address (e.g. `0000:04:00.0`).

## Healthy TX Run (reference)

```
[INFO ] dvledtx initializing...
MTL: ..., mt_eth_link_dump(0), link_speed 10g link_status 1     <- ~4-5 s after start
MTL: ..., tv_init_pacing[00], ... fps 30.000000
[INFO ] ST20P TX(0): ffmpeg_tx opened (1920x1080 gbrp12le @ 30fps) -> 239.168.85.20:20000 via <BDF>
MTL: ..., TX_VIDEO_SESSION(0,0:st20p_ffmpge): fps 29.99 frames 300 pkts 2221816:0   <- every 10 s
[INFO ] dvledtx shutdown complete
```

1080p30 `gbrp12le` uses about 2.3 Gb/s per session. Any `fps 0.000000` stats line means the session is dead.

## Healthy RX Run (reference)

- 10 s stats show `fps ~30.0`, `frames 300` and `get/succ/put 300/300/300`, with `incomplete frames 0`.
- `DEV(0) rx` Mb/s is about the per-stream rate (for example about 980 Mb/s for 960×1080 `gbrp10le` @ 30).
- The first window after start is often partial (for example 10 fps); judge from the second window onwards.

---

## 1. TX stops about 1 s after start, usually on the second run

**Signature**
```
MTL: video_trs_burst_fail(0), hang duration 1000 ms
MTL: Error: st20_tx_queue_fatal_error(0,1), old mempool still in use
MTL: Error: TX_VIDEO_SESSION(0,0): unrecoverable_error 1
[ERROR] ST20P TX(0): av_write_frame failed (ret=-5)      <- once per second until exit
MTL: TX_VIDEO_SESSION(...): fps 0.000000 frames 0
```
Also at shutdown: `still has mbuf in mempool`, `frame N are still in converted`.

**Tell-tale:** `mt_eth_link_dump ... link_status 1` appears in the **same second** as port start. Good runs show a 4–5 s link renegotiation.

**Cause:** On E610, DPDK `ixgbe_dev_start()` resets the MAC but does not restart autonegotiation if the PHY is still linked from the previous run. The NIC then never drains the TX ring. This is not caused by dvledtx or the PTP code: it reproduces on the pre-PTP commit and with `ptp.enable=false`.

**Fix (verified: 60 s, 30 fps, 0 drops):** reset the link before every run.
```bash
sudo sh -c "<MTL>/script/nicctl.sh bind_kernel <BDF> && sleep 3 && <MTL>/script/nicctl.sh bind_pmd <BDF>"
```
If `bind_kernel` has nothing to fall back to (device left unbound), reboot and then redo hugepages and binding (see #2).

## 2. Exit 255 right after a reboot

**Signature:** `EAL: Cannot get hugepage information`, `HugePages_Total: 0`; `readlink /sys/bus/pci/devices/<BDF>/driver` shows `ixgbe` instead of `vfio-pci`.

**Cause:** Hugepages and the PMD binding do not persist across reboots.

**Fix:**
```bash
sudo sh -c 'sysctl -w vm.nr_hugepages=4096 && <MTL>/script/nicctl.sh bind_pmd <BDF>'
```

## 3. Exit 255: `/dev/vfio/N: Permission denied`, `mtl_init` failed

**Cause:** dvledtx ran without root (for example, sudo credentials had expired and the prompt was cancelled). The log file may also be unwritable because an earlier root run owns `dvledtx.log`.

**Fix:** Run with `sudo` in an interactive terminal. An agent cannot type the sudo password, so give the user the exact command to run instead of retrying.

## 4. PTP on E610 PF: never locks, then TX freezes

**Signature**
```
MTL: Error: ptp_timesync_read_rx_time(0), err -22
MTL: ... t3 tx timestamp timeout
MTL: video_trs_ptp_tasklet ... invalid tsc cur <same value repeated>
[ERROR] av_write_frame failed (ret=-5), fps 0
```

**Causes found (layered, all three must be fixed):**
1. MTL enables timesync **before** `rte_eth_dev_start()`. Starting an ixgbe port resets the MAC and wipes the timesync registers (`err -22`).
2. By default DPDK is built without `RTE_LIBRTE_IEEE1588`, so ixgbe never sets the TX-timestamp bit (`t3 tx timestamp timeout`).
3. The E610 PHC stays frozen at its start value, so the PTP pacer waits forever. This is a **regression in MTL v26.09** (`9762c314`). It does not happen with MTL at commit [`7bee242`](https://github.com/OpenVisualCloud/Media-Transport-Library/commit/7bee24231c864d12da6db2c2ffbfd3986717af5a) (2026-08-17), which is 142 commits earlier. Not bisected yet. Candidates in `lib/src/mt_ptp.c` and `lib/src/dev/mt_dev.c` include `d36d32ff`, `af8d6bee` and `545a266a` (`git log 7bee242..v26.09 -- lib/src/mt_ptp.c lib/src/dev/mt_dev.c`). Upstream PR #1893 (still open) targets the same area.

**Fix (verified working PTP):** use MTL `7bee242` + the `mt_dev.c` patch + DPDK with IEEE1588. Moving to `7bee242` alone is **not** enough.

1. Check out MTL at `7bee242` and apply the ixgbe timesync patch in `lib/src/dev/mt_dev.c` (`mt_dev_create()`, around line 1958):
   ```diff
        /* IGC timesync is deferred until RX queues are configured. */
   +    /* ixgbe dev_start resets the MAC, clearing TIMINCA/TSYNC*CTL; enable after start. */
        if ((mt_user_ptp_service(impl) || mt_user_hw_timestamp(impl)) &&
   -        (port_type == MT_PORT_PF) && !dev_igc_timesync_required(inf)) {
   +        (port_type == MT_PORT_PF) && !dev_igc_timesync_required(inf) &&
   +        (inf->drv_info.drv_type != MT_DRV_IXGBE)) {
          ret = dev_start_timesync(inf);
   ```
   The existing "start time sync after `rte_eth_dev_start`" block then enables it for ixgbe.
2. Build DPDK (with MTL's DPDK patches, per `<MTL>/doc/build.md`) with the IEEE1588 flag, and install it:
   ```bash
   meson setup build -Dc_args=-DRTE_LIBRTE_IEEE1588 && ninja -C build && sudo ninja -C build install && sudo ldconfig
   ```
3. Build and install MTL, rebuild FFmpeg with the `mtl_st20p` plugin from the same MTL commit (see #7), then rebuild the toolkit:
   ```bash
   cd <MTL> && ./build.sh && sudo ldconfig     # build.sh installs via sudo ninja install
   cd <toolkit> && bash scripts/build.sh
   pkg-config --modversion mtl libdpdk     # confirm the installed libs are the ones just built
   ```
- The FFmpeg plugin at `7bee242` already has the options dvledtx needs: 12-bit formats, `p2_port`..`p7_port`, `ptp_enable`, `pacing_way`. No dvledtx change is required.
- This differs from the README pin (`9762c314` / v26.09). Keep the README pin for non-PTP deployments and use the patched `7bee242` only where E610 PTP is required, until upstream fixes the regression.
- Build as a normal user and use `sudo` only for `install`, to avoid root-owned build dirs (#11).

**Workaround without rebuilding MTL:** use `"ptp": { "enable": false }` (TSC pacing). Only `enable` is required; `pi` and `unicast` default to `false`.

## 5. PTP on a VF receives nothing

**Signature:** `unknown nic driver net_ixgbe_vf`, `ixgbevf_add_mac_addr(): Unable to add MAC address 01:00:5E:00:01:81 - diag=-34`, PTP stats `rx: 0`.

**Cause:** MTL only enables hardware timesync on a PF, and the ixgbevf driver rejects the PTP multicast MAC filters.

**Fix:** Use the PF (bound to `vfio-pci`) for any app that runs MTL's built-in PTP client.

## 6. RX gets 0 frames; MTL reports MAC `00:00:00:c9:a0:01`

**Cause:** The PF came up with an invalid MAC, so unicast and multicast filtering drop the stream.

**Fix (verified):** On the RX host, run `nicctl.sh bind_kernel <PF>`, then `nicctl.sh create_vf <PF>`, and use one of the VF BDFs (now `vfio-pci`, with a real MAC) in the RX `interfaces[].name`. This conflicts with #5 if RX PTP is needed.

## 7. 12-bit formats rejected (`unsupported pixel format`)

**Cause:** The FFmpeg `mtl_st20p` plugin was compiled from an older MTL that only knows 10-bit formats. Rebuilding MTL alone does not update it.

**Fix:** Rebuild FFmpeg with the plugin sources from the pinned MTL v26.09 commit (see README "Software Requirements").

## 8. Unit tests fail in CI with no diagnostic

**Signature:** The CI log shows `Setting up combined test build...` and then exits 1.

**Cause:** [scripts/test.sh](../../../../scripts/test.sh) sends `meson setup` and `ninja` output to `/dev/null`, so link errors are hidden. Typical trigger: a new source file is used by a module (for example `mtl_tx.c`) but is not added to **every** test target that compiles that module (`test_mtl_tx`, `test_mtl_tx_stub`, `test_session_manager`).

**Fix:** Reproduce with visible output (needs `libcmocka-dev`):
```bash
meson setup build-tests && ninja -C build-tests
```
Then add the missing source to every affected target in `dvledtx/tests/meson.build`.

## 9. dvledrx log unreadable by non-root tools

**Cause:** dvledrx creates `log_file` with mode `0640` (owner root when run with sudo). Scripts that read it as a normal user (for example over SSH) see nothing.

**Fix:** For automation, omit `log_file` so dvledrx logs to the console, and capture stdout.

## 10. dvledrx under sudo shows no window

**Cause:** Root cannot authenticate to the user's X session without `XAUTHORITY`.

**Fix:**
```bash
sudo env DISPLAY=:0 XAUTHORITY=/run/user/$(id -u)/gdm/Xauthority ./build/dvledrx/dvledrx --config <rx.json>
```
For passwordless automation, use a sudoers drop-in with `Defaults!<path>/dvledrx env_keep+="DISPLAY XAUTHORITY"`. Sudoers lines go in `/etc/sudoers.d/` via `visudo`, not in a shell.

## 11. Custom MTL/DPDK build not picked up, or rebuild fails

- The apps link the **installed** libs. Check them with `pkg-config --modversion mtl libdpdk` and `ldconfig -p | grep -E 'mtl|rte_'`. A build in `<MTL>/build-*/` is only used via `sudo ninja install` or `LD_LIBRARY_PATH`.
- `Operation not permitted` during a DPDK/MTL rebuild means a previous `sudo ninja install` left root-owned files in the build dir. Run `sudo chown -R $USER <build-dir>` and build as the user; use sudo only for `install`.
- With several MTL/DPDK checkouts on one machine, confirm which one is installed before attributing a failure to local patches.

## 12. Receiver CLI changed (old scripts fail immediately)

The old RxApp flags `--time` and `--no_display` no longer exist. dvledrx accepts only `-C/--config`, `-t/--test-time`, `-v`, and `--help`, and exits with its usage text on unknown options.

## 13. RX picture flickers, tears or jumps between images

**Cause:** Two transmitters are sending to the **same `dip` + `udp_port`**. This is often a TX left running on another host, or two configs reusing the defaults `239.168.85.20:20000`. The receiver gets packets from both streams mixed into one session. dvledrx opens sessions with `ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME`, so the mixed or partial frames are still drawn, and the picture alternates or tears.

**Signature:** RX throughput about 2x the expected value (for example 2.6 Gb/s where one stream is 1.3 Gb/s); `incomplete frames N` and `unrecovered pkts` in RX stats; fps erratic or above the configured rate.

**Fix:**
- Stop stray transmitters on **every** host on the media switch: `pgrep -a dvledtx`, then `sudo pkill -f dvledtx`.
- Give each TX session a unique `dip:udp_port` across the whole network. dvledtx only rejects duplicate `udp_port` on the same NIC **within one config**. It cannot detect another host using the same group.
- Confirm the fix: RX `DEV(0) rx` Mb/s matches one stream, and `incomplete frames` stays 0.

## 14. RX window shows a picture, but it is not live

**Cause:** The X11 window keeps the **last rendered frame** after the stream stops or never fully arrives. A still image does not prove reception.

**Check:** RX stats show `fps 0.000000 frames 0` and `DEV(0) rx: 0.000000 Mb/s`. Live video changes (moving pattern or timecode); a stale frame stays perfectly still.

## 15. RX gets nothing for one multicast group, but another group works

**Signature:** MTL logs `mt_mcast_join(0), join group 239.168.85.90`, yet `DEV(0) rx: 0` for the whole run, while the TX shows full throughput. The same RX on `239.168.85.20` works.

**Cause:** The switch is forwarding multicast per group: IGMP snooping with no querier, or statically provisioned groups. A stream can also arrive only in short bursts (a few frames, then nothing) for the same reason.

**Fix:** On the media switch, enable an IGMP querier on the media VLAN or disable IGMP snooping there, or provision the groups in use. Until then, use a group known to be forwarded. Rule out #13 first: another host's stream on the "working" group can hide this.

## 16. TX fps slightly below target on E610 (for example 29.0–29.4 instead of 30)

**Cause:** MTL has no hardware rate-limit pacing for `net_ixgbe` (E610), only for `net_ice`/`net_iavf` (E810). Pacing falls back to TSC (`dev_if_init_pacing(0), use tsc as default`), which slips under load, especially with many sessions per port. This is not a dvledtx bug.

**Mitigate:** Use fewer sessions per port, spread sessions across NICs, or use an E810 (ice) NIC for RL pacing. Long soaks on E610 showed about 2–3 % epoch drops with 8 × 1080p30 `gbrp12le` sessions.

Also check the decoder side: the log line `scaler ... (sws_threads=N)` should show N > 1. The swscale thread count is `min(online_cpus/2, 8)`, and a single-threaded scaler cannot keep up with 4K or many sessions.

## 17. PTP silently not used

**Signature:** `PTP requested but MTL FFmpeg plugin lacks '<option>' AVOption; continuing without PTP`. TX runs normally on TSC pacing.

**Cause:** FFmpeg was built with an older `mtl_st20p` plugin that lacks `ptp_enable`/`ptp_pi`/`ptp_unicast`/`pacing_way`.

**Fix:** Rebuild FFmpeg with the plugin from the installed MTL commit (see #7). If the options exist but fail to set, dvledtx fails at startup instead, by design.

## 18. Multi-NIC or multi-VF TX fails at session setup (FFmpeg path)

**Signature:** `mtl_st20p_write_header, mtl dev get fail`, then `avformat_write_header (mtl_st20p) failed: Input/output error`, then `Failed to initialize session manager`.

**Cause (seen on an earlier build):** Only session 0 registers every NIC with MTL. The plugin compares later sessions' device params with `memcmp` and rejects the mismatch.

**Workaround:** Build with `bash scripts/build.sh -Denable_mtl_tx=true`, which passed with 4 VFs × 1 session. Re-verify on the current `dev` before treating this as open.

**VF setup notes (TX):** create the VFs with `nicctl.sh create_vf <PF>`, then set each VF to `trust on` and `spoofchk off` with a unique MAC, and give each session its own `sip` and `dip:udp_port`.

## 19. RX shows wrong colours, stripes, or no frames despite packets arriving

**Cause:** TX and RX disagree on the stream format.
- Wrong colours or stripes: `fmt` differs (for example TX `gbrp12le`, RX `gbrp10le`).
- `pkts` > 0 but `frames 0` or many `incomplete frames`: RX `video.width`/`height` does not match the TX session's **crop** `w`/`h` (each TX session is a separate stream of its crop size).
- Nothing at all: `udp_port`, `dip`, or `payload_type` mismatch.

**Fix:** Derive the RX config from the TX config: each RX session's `video.width`/`height` = that TX session's `crop.w`/`h`, with the same `fmt`, `fps`, `dip`, `udp_port` and `payload_type`.
