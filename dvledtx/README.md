# dvledtx — Transmitter

dvledtx reads a video source (a file such as MP4, or a live X11 screen), decodes it with FFmpeg,
optionally scales it, and transmits uncompressed frames over IP using SMPTE ST 2110-20 through the
Media Transport Library (MTL). Multiple crop regions of the same frame can be transmitted
simultaneously as independent RTP streams — enabling tiled LED walls where each panel receives
its own portion of the frame.

> Install, host setup, build and the basic run command are shared with the receiver and live in the
> [top-level README](../README.md).

## Table of Contents

- [Features](#features)
- [Usage](#usage)
  - [Command-Line Options](#command-line-options)
  - [TX Paths](#tx-paths)
- [JSON Configuration](#json-configuration)
  - [Configuration Reference](#configuration-reference)
  - [Validation Rules](#validation-rules)
  - [Example: MP4 File Input](#example-mp4-file-input)
  - [Example: Multi-Session](#example-multi-session)
  - [Sample Configurations](#sample-configurations)
  - [PTP Timing](#ptp-timing)
  - [Hardware Decode (VA-API)](#hardware-decode-va-api)
  - [Screen Capture](#screen-capture)
- [Logging](#logging)
- [Supported Formats](#supported-formats)
- [Video Scaling](#video-scaling)
- [Performance Considerations](#performance-considerations)
- [Troubleshooting](#troubleshooting)

## Features

- **ST 2110-20 video transmission** of uncompressed video
- **Multi-session**: several crop regions of one source sent as independent streams
- **Multi-NIC**: up to 8 interfaces, each session selects its NIC
- **File or screen-capture input** (`x11grab`)
- **Scaling** (up or down) before cropping, up to 3840×2160
- **Optional PTP-paced transmission** via MTL's built-in PTP client
- **Optional VA-API hardware decode** with automatic CPU fallback
- **JSON configuration** with strict validation
- **Memory efficient**: hugepage-backed buffers

## Usage

From the repository root, after [building](../README.md#build):

```bash
./build/dvledtx/dvledtx --config dvledtx/config/tx_fullhd_single_session.json

# transmit for 60 seconds then exit
./build/dvledtx/dvledtx --config dvledtx/config/tx_fullhd_single_session.json --test-time 60
```

### Command-Line Options

| Option | Description |
|--------|-------------|
| `-C, --config <file>` | JSON config file (required) |
| `-t, --test-time <secs>` | Transmit for N seconds (1..86400), then exit |
| `-v, --version` | Show version and exit |
| `--help` | Show help message |

### TX Paths

dvledtx has two transmit back-ends, chosen at build time with the `enable_mtl_tx` Meson option:

| Build | TX path | Notes |
|-------|---------|-------|
| default (`-Denable_mtl_tx=false`) | FFmpeg `mtl_st20p` muxer (`libavdevice`) | Requires the MTL FFmpeg plugin. Up to 8 NICs via the `p_port`/`r_port` + `p2_port`..`p7_port` AVOptions of the pinned plugin; older plugin revisions only expose `p_port`/`r_port` (max 2 NICs). |
| `-Denable_mtl_tx=true` | MTL pipeline API (`st20p_tx_get_frame` / `st20p_tx_put_frame`) | Bypasses the FFmpeg muxer. Always supports up to 8 NICs. |

```bash
bash scripts/build.sh -Denable_mtl_tx=true
```

## JSON Configuration

dvledtx is configured entirely by a JSON file passed with `--config`. Sample files are in
[config/](config/).

### Configuration Reference

| Section | Field | Description |
|---------|-------|-------------|
| **log_file** | `log_file` | (Optional) Log output file (e.g. `dvledtx.log`), written in addition to the console. Must be under the working directory or `/var/log/`. See [Logging](#logging). |
| **hwaccel** | `hwaccel` | (Optional) Decode the input on the GPU via VA-API. Default `false`. Falls back to the CPU automatically. See [Hardware Decode](#hardware-decode-va-api). |
| **interfaces[]** | `nic_index` | (Optional) Must equal this entry's position in the array (0, 1, 2, …); informational only — interfaces are resolved by position. |
| | `name` | PCI BDF address of the NIC or VF (e.g. `0000:06:00.0`) |
| | `sip` | (Optional) Source IP address |
| | `dip` | Destination **multicast** IP address (224.0.0.0/4) |
| **video** | `width` | Source frame width in pixels |
| | `height` | Source frame height in pixels |
| | `input_mode` | (Optional) `file` (default) or `screen_capture` |
| | `tx_url` | Path to the source video file (`input_mode: file`) |
| | `screen_input` | x11grab source `<display>[+<x>,<y>]` (`input_mode: screen_capture`), e.g. `:0.0+0,0` |
| **tx_video** | `scale_width` | (Optional) Output width after scaling. See [Video Scaling](#video-scaling). |
| | `scale_height` | (Optional) Output height after scaling |
| | `fps` | Frames per second: 25, 30, 50, 60 |
| | `fmt` | Pixel format (see [Supported Formats](#supported-formats)) |
| **ptp** | `enable` | (Optional) Enable MTL's built-in PTP client and PTP-paced TX. Default `false`. See [PTP Timing](#ptp-timing). |
| | `pi` | (Optional) Use the PI controller (physical-function NICs only). Default `false` |
| | `unicast` | (Optional) Send `PTP_DELAY_REQ` to a unicast address instead of multicast. Default `false` |
| **tx_sessions[]** | `nic_index` | (Optional) Index into `interfaces[]` selecting this session's NIC. Default `0` |
| | `udp_port` | Destination UDP port (1024..65535) |
| | `payload_type` | (Optional) RTP payload type, 96..127. Default `96` |
| | `crop` | Region of the (scaled) frame to transmit: `x`, `y`, `w`, `h` in pixels |

### Validation Rules

The config is validated at start-up; on error dvledtx logs the offending field and exits.

- At least one interface and one session are required; up to 8 interfaces.
- `interfaces[].name` must be a PCI BDF (`DDDD:BB:DD.F`); `dip` must be multicast.
- Source and scaled sizes are limited to 3840×2160; `video.width` must be even for YUV formats.
- `scale_width` and `scale_height` must be set together.
- `screen_input` is required for `screen_capture`; `tx_url` must exist for `file`.
- Crops must lie inside the source frame — or the **scaled** frame when scaling is set — and
  respect the format's chroma alignment (even `x`/`w` for 4:2:2, even `x`/`y`/`w`/`h` for 4:2:0).
- `nic_index` must refer to an existing interface; two sessions on the same NIC cannot share a
  UDP port.
- The config file must not be a symlink.

### Example: MP4 File Input

[`config/tx_fullhd_single_session.json`](config/tx_fullhd_single_session.json):

```json
{
  "log_file": "dvledtx.log",
  "interfaces": [
    { "nic_index": 0, "name": "0000:01:00.1", "sip": "192.168.50.29", "dip": "239.168.85.20" }
  ],
  "video": {
    "width": 1920, "height": 1080,
    "tx_url": "bbb_sunflower_1080p_30fps_normal.mp4"
  },
  "tx_video": {
    "scale_width": 1920, "scale_height": 1080,
    "fps": 30,
    "fmt": "gbrp12le"
  },
  "tx_sessions": [
    { "nic_index": 0, "udp_port": 20000, "payload_type": 96, "crop": { "x": 0, "y": 0, "w": 1920, "h": 1080 } }
  ]
}
```

### Example: Multi-Session

Multiple sessions in `tx_sessions` transmit different crop regions of the same frame.
[`config/tx_fullhd_multi_session.json`](config/tx_fullhd_multi_session.json) splits 1920×1080
into three 640×1080 strips on one NIC:

```json
"tx_sessions": [
  { "nic_index": 0, "udp_port": 20000, "payload_type": 96, "crop": { "x": 0,    "y": 0, "w": 640, "h": 1080 } },
  { "nic_index": 0, "udp_port": 20002, "payload_type": 96, "crop": { "x": 640,  "y": 0, "w": 640, "h": 1080 } },
  { "nic_index": 0, "udp_port": 20004, "payload_type": 96, "crop": { "x": 1280, "y": 0, "w": 640, "h": 1080 } }
]
```

To spread sessions across NICs, add entries to `interfaces[]` (up to 8, MTL's port limit) and set
each session's `nic_index` — see
[`config/tx_fullhd_multi_nic.json`](config/tx_fullhd_multi_nic.json) for 8 NICs × 1 session.

### Sample Configurations

Edit `interfaces[].name`, the IP addresses and `tx_url` for your setup before use.

| File | Source | Output | Sessions | NICs | Format | Notes |
|------|--------|--------|----------|------|--------|-------|
| [`tx_fullhd_single_session.json`](config/tx_fullhd_single_session.json) | 1080p file | 1920×1080 | 1 | 1 | `gbrp12le` | Basic single stream |
| [`tx_1session_12bit.json`](config/tx_1session_12bit.json) | 1080p file | 1920×1080 | 1 | 1 | `yuv422p12le` | 12-bit YUV, no scaling |
| [`tx_fullhd_multi_session.json`](config/tx_fullhd_multi_session.json) | 1080p file | 1920×1080 | 3 | 1 | `yuv422p10le` | Three 640×1080 strips |
| [`tx_fullhd_multi_nic.json`](config/tx_fullhd_multi_nic.json) | 1080p file | 1920×1080 | 8 | 8 | `yuv422p10le` | One 240×1080 strip per NIC |
| [`tx_gbrp12le_pf_8session.json`](config/tx_gbrp12le_pf_8session.json) | 1080p file | 1920×1080 | 8 | 1 | `yuv422p12le` | 8 sessions on one PF; disabled `ptp` block |
| [`tx_2k_single_session.json`](config/tx_2k_single_session.json) | 2K file | 2560×1440 | 1 | 1 | `yuv422p10le` | |
| [`tx_2k_multi_session.json`](config/tx_2k_multi_session.json) | 2K file | 2560×1440 | 3 | 1 | `yuv422p10le` | |
| [`tx_2k_2nic.json`](config/tx_2k_2nic.json) | 2K file | 2560×1440 | 2 | 2 | `yuv422p10le` | `hwaccel: true` |
| [`tx_4k_single_session.json`](config/tx_4k_single_session.json) | 4K file | 900×600 | 1 | 1 | `yuv422p10le` | Downscale, 60 fps |
| [`tx_4k_multi_session.json`](config/tx_4k_multi_session.json) | 4K file | 1920×1080 | 3 | 1 | `yuv422p10le` | Downscale then split |
| [`tx_fullhd_screen_capture.json`](config/tx_fullhd_screen_capture.json) | X11 `:99` | 1920×1080 | 1 | 1 | `yuv422p10le` | Screen capture |
| [`tx_fullhd_screen_capture_multi_session.json`](config/tx_fullhd_screen_capture_multi_session.json) | X11 `:99` | 1920×1080 | 3 | 1 | `yuv422p10le` | Screen capture split into 3 strips |

### PTP Timing

ST 2110 senders normally derive their TX pacing from a PTP (IEEE 1588) clock. dvledtx exposes MTL's
built-in PTP client through an optional top-level `ptp` block:

```json
"ptp": {
  "enable":  true,
  "pi":      true,
  "unicast": false
}
```

**PTP is disabled by default.** When the block is absent — or `enable` is `false` — TX uses
TSC-based pacing. When enabled, dvledtx asks MTL's built-in PTP client to synchronize and selects
PTP pacing in both the direct-MTL and FFmpeg TX paths.

Enable PTP only when a **PTP grandmaster is present on the network** (a dedicated appliance or a
boundary-clock switch). MTL's built-in PTP client is *slave-only* — it cannot elect itself
grandmaster — so with `enable: true` and no grandmaster on the link the TX pacing clock never locks
and transmission stalls.

The startup log reports the active settings and the pacing mode chosen by MTL:

```
[INFO ] PTP enabled: pi_controller=1 unicast_delay_req=0
MTL: ... tv_attach(0), pacing way: ptp
```

With PTP disabled:

```
[INFO ] PTP disabled (default TSC-based TX pacing)
MTL: ... tv_attach(0), pacing way: tsc
```

For the FFmpeg TX path, the installed `mtl_st20p` muxer must expose the `ptp_enable`, `ptp_pi`,
`ptp_unicast` and `pacing_way` AVOptions. If PTP is requested and an option is unavailable,
dvledtx logs that PTP is unsupported by the installed plugin and continues without PTP. If the
options are advertised but cannot be set, initialization fails rather than silently ignoring a
configuration error.

MTL uses NIC hardware timesync/PHC when the selected NIC and driver support it. Some
configurations (including VFs) use a software/TSC time source instead. Enabling this option
requests MTL's built-in PTP behavior; it does not guarantee hardware clock availability,
grandmaster lock, or cross-host display genlock. `MTL_FLAG_ENABLE_HW_TIMESTAMP` is a separate
packet-capture/compliance option and is not enabled by this setting.

### Hardware Decode (VA-API)

By default dvledtx decodes on the CPU. The optional top-level `hwaccel` flag offloads the
compressed-bitstream decode to the GPU's fixed-function video engine via VA-API:

```json
"hwaccel": true
```

| Value | Behaviour |
|-------|-----------|
| absent / `false` | CPU decode (default) |
| `true` | Try VA-API; fall back to the CPU if it is unavailable |

**`hwaccel: true` never fails the run.** If FFmpeg was built without VA-API, no render node is
accessible, the codec rejects the hardware context, or the decoder cannot produce a GPU surface for
the stream, dvledtx logs a warning and continues on the CPU. Repeated GPU-to-system-memory transfer
failures mid-stream also trigger a reopen on the CPU rather than stalling transmission. Only the
decode stage is offloaded — decoded frames are downloaded to system memory so the existing
scale/crop/TX path is unchanged. MTL transmits from its own DMA buffers, so there is no GPU-to-NIC
zero-copy path.

Confirm which path was taken from the startup log:

```
[INFO ] ST20P TX(0): opened 'clip.mp4' Codec=hevc 1920x1080 yuv444p12le -> 1920x1080 yuv422p10le (hwaccel=vaapi)
```

> **Read `(hwaccel=vaapi)` carefully.** It is printed when the VA-API *device* opens, before the
> decoder negotiates a surface format, so it does **not** by itself prove the GPU is decoding. The
> reliable fallback indicator is:
>
> ```
> [WARN ] Hardware decode: vaapi surface unavailable for this stream, decoding on CPU
> ```

**Requirements and limitations**

- FFmpeg must be built with `--enable-vaapi` and the VA-API runtime installed (see the
  [install steps](../README.md#1-system-packages)). Verify with `ffmpeg -hwaccels | grep vaapi`
  and `vainfo | grep VAEntrypointVLD`.
- A DRM render node (e.g. `/dev/dri/renderD128`) must be readable and writable by the user running
  dvledtx. The device is selected automatically; there is no config key to pin a render node.
- The GPU must support the stream's decode profile — check `vainfo` for a matching `VAProfile*`
  with `VAEntrypointVLD`.
- **RGB (`gbrp10le` / `gbrp12le`) sources can never use VA-API** — FFmpeg's HEVC decoder only
  offers a VA-API surface for YUV formats. This is a decoder limitation, not a configuration error.
- This setting affects the **source** decode only and is independent of `tx_video.fmt`.

> **Note on throughput:** hardware decode reduces decode CPU time, but frames must then be
> downloaded from GPU to system memory, which costs roughly what the decode saved. Treat it as a
> way to free CPU cycles for other work on the host, not as a throughput optimisation.

### Screen Capture

Set `video.input_mode` to `screen_capture` to transmit whatever is rendered on an X11 display
(desktop, windows, applications) instead of a file.

**Requirements**

- FFmpeg built with the `x11grab` device — the `libxcb*` packages must be installed **before**
  configuring FFmpeg (see the [install steps](../README.md#1-system-packages)). Verify with
  `ffmpeg -devices | grep x11grab`; otherwise screen capture fails at runtime with
  `x11grab input format not found`.
- An **X11 (Xorg)** display — `x11grab` does not work against Wayland. See
  [Ensuring an X11 Session](#ensuring-an-x11-session).
- Headless machines additionally need a virtual display — see
  [Screen Capture on a Headless Machine](#screen-capture-on-a-headless-machine).

#### Example: Screen-Capture Input

[`config/tx_fullhd_screen_capture.json`](config/tx_fullhd_screen_capture.json):

```json
{
  "interfaces": [
    { "name": "0000:06:00.0", "sip": "192.168.50.29", "dip": "239.168.85.20" }
  ],
  "video": {
    "width": 1920,
    "height": 1080,
    "input_mode": "screen_capture",
    "screen_input": ":99.0+0,0"
  },
  "tx_video": {
    "fps": 30,
    "fmt": "yuv422p10le"
  },
  "tx_sessions": [
    { "udp_port": 20000, "crop": { "x": 0, "y": 0, "w": 1920, "h": 1080 } }
  ]
}
```

`screen_input` follows FFmpeg's `x11grab` syntax: `<display>[+<x>,<y>]` (e.g. `:99.0+0,0` captures
display `:99` starting at offset `0,0`). The capture size and frame rate come from
`video.width`/`height` and `tx_video.fps`. Multiple `tx_sessions` split the capture into regions
exactly as for file input — see
[`config/tx_fullhd_screen_capture_multi_session.json`](config/tx_fullhd_screen_capture_multi_session.json).

```bash
./build/dvledtx/dvledtx --config dvledtx/config/tx_fullhd_screen_capture.json
```

#### Ensuring an X11 Session

Ubuntu 22.04+ defaults GDM logins to **Wayland**, which makes `screen_capture` fail (or capture a
blank/incorrect frame) on a machine capturing its own physical desktop. Make sure the session is
X11:

- **Per login**: at the GDM login screen, click the gear icon next to the password field and choose
  **"Ubuntu on Xorg"** instead of **"Ubuntu"** (Wayland).
- **System-wide**: disable Wayland in GDM:
  ```bash
  sudo cp /etc/gdm3/custom.conf /etc/gdm3/custom.conf.bak
  sudo sed -i 's/^#\?WaylandEnable=.*/WaylandEnable=false/' /etc/gdm3/custom.conf
  grep -q '^WaylandEnable=false' /etc/gdm3/custom.conf || \
    sudo sed -i '/^\[daemon\]/a WaylandEnable=false' /etc/gdm3/custom.conf
  sudo systemctl restart gdm3   # logs out the current session; or: sudo reboot
  ```
  Verify after logging back in: `echo $XDG_SESSION_TYPE` should print `x11`.

The headless setup below is unaffected — it starts Xorg directly with `XDG_SESSION_TYPE=x11`.

#### Screen Capture on a Headless Machine

`x11grab` needs a real X11 display. On a machine with no monitor, create a virtual display with
Xorg's `dummy` driver and run a desktop session on it. Unlike Xvfb, a real Xorg server claims
physical input devices, so a local keyboard and mouse work on the virtual display.

> These packages are **not dvledtx dependencies**; this is one example of building a virtual
> display on Ubuntu — adapt as needed.

1. **Install** the virtual-display packages:
   ```bash
   sudo apt-get install -y xserver-xorg-video-dummy ubuntu-desktop
   ```

2. **Create an Xorg config** for the dummy driver:
   ```bash
   sudo tee /tmp/xorg-dummy.conf > /dev/null << 'EOF'
   Section "Device"
       Identifier "dummy"
       Driver "dummy"
       VideoRam 256000
   EndSection

   Section "Monitor"
       Identifier "monitor"
       HorizSync 28.0-80.0
       VertRefresh 48.0-75.0
       Modeline "1920x1080" 148.50 1920 2008 2052 2200 1080 1084 1089 1125 +hsync +vsync
   EndSection

   Section "Screen"
       Identifier "screen"
       Device "dummy"
       Monitor "monitor"
       DefaultDepth 24
       SubSection "Display"
           Depth 24
           Modes "1920x1080"
       EndSubSection
   EndSection

   Section "ServerLayout"
       Identifier "layout"
       Screen "screen"
       Option "AllowEmptyInput" "false"
   EndSection
   EOF
   ```

3. **Start Xorg** on display `:99`:
   ```bash
   sudo Xorg :99 -config /tmp/xorg-dummy.conf -noreset &
   ```

4. **Allow root access and start a GNOME session** (software GL is required without a GPU;
   `PULSE_SERVER` routes audio to the physical device):
   ```bash
   DISPLAY=:99 xhost +local:root
   mkdir -p /tmp/gnome99-runtime && chmod 700 /tmp/gnome99-runtime
   DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 XDG_SESSION_TYPE=x11 \
     XDG_RUNTIME_DIR=/tmp/gnome99-runtime \
     PULSE_SERVER=unix:/run/user/$(id -u)/pulse/native \
     dbus-run-session -- gnome-session --session=ubuntu &
   ```

5. **Verify** the display (give GNOME a few seconds to start):
   ```bash
   DISPLAY=:99 ffmpeg -f x11grab -video_size 1920x1080 -i :99.0+0,0 -frames:v 1 -update 1 /tmp/check.png
   ```

> **Audio:** if PulseAudio only has a `null` sink, load the physical device manually:
> ```bash
> aplay -l
> pactl load-module module-alsa-sink device=hw:1,0 sink_name=plantronics sink_properties=device.description="Plantronics_Headset"
> pactl set-default-sink plantronics
> ```
> Applications launched on the virtual display also need `PULSE_SERVER`, e.g.
> `DISPLAY=:99 PULSE_SERVER=unix:/run/user/$(id -u)/pulse/native google-chrome &`.

**Tear down** the virtual display when done:

```bash
pkill -f "gnome-session --session=ubuntu"
sudo pkill -f "Xorg :99"
```

## Logging

dvledtx includes a built-in logger with configurable output targets.

| Setting | Behaviour |
|---------|-----------|
| `log_file` (config) | Log to this file **in addition to** the console. Takes priority over `LOG_FILE`. |
| `LOG_FILE` (environment) | Used when the config has no `log_file`. |
| neither | Console only. |

- The log path must be under the current working directory or `/var/log/`; any other path is
  rejected and logging falls back to the console.
- The log file rotates at 20 MB: the old file is renamed with a timestamp suffix and gzipped (e.g.
  `dvledtx.log.2026-05-13_083000.gz`).

| Level | Description |
|-------|-------------|
| `ERROR` | Critical failures only |
| `WARN` | Non-fatal warnings |
| `INFO` | General operational messages (default) |
| `DEBUG` | Verbose diagnostic output |

## Supported Formats

### Video Formats (`tx_video.fmt`)

| Format | Chroma | Bit Depth | Color Space |
|--------|--------|-----------|-------------|
| `yuv422p10le` | 4:2:2 | 10-bit | YUV |
| `yuv420` | 4:2:0 | 8-bit | YUV |
| `yuv444p10le` | 4:4:4 | 10-bit | YUV |
| `gbrp10le` | 4:4:4 | 10-bit | RGB |
| `yuv422p12le` | 4:2:2 | 12-bit | YUV |
| `yuv444p12le` | 4:4:4 | 12-bit | YUV |
| `gbrp12le` | 4:4:4 | 12-bit | RGB |

12-bit formats require MTL and FFmpeg built from the pinned commit (see the
[install steps](../README.md#3-ffmpeg-with-the-mtl-plugin)).

### Frame Rates (`tx_video.fps`)

25, 30, 50 and 60 fps.

### Resolutions

| Resolution | Dimensions | Description |
|------------|------------|-------------|
| **1080p** (Full HD) | 1920×1080 | Standard HD resolution |
| **2K** (QHD) | 2560×1440 | Quad HD / 2K resolution |
| **4K** (UHD) | 3840×2160 | Ultra HD / 4K resolution |

> **Note:** Maximum supported resolution is 3840×2160. Width must be even for YUV formats.

## Video Scaling

Optional `tx_video.scale_width` / `scale_height` scale the decoded source before crop regions are
applied.

| Feature | Description |
|---------|-------------|
| **Upscale** | Scale a smaller source (e.g. 1080p) to a larger output (e.g. 4K) |
| **Downscale** | Scale a larger source (e.g. 4K) to a smaller output (e.g. 1080p) |
| **Single session** | Full scaled frame transmitted as one stream |
| **Multi-session** | Crop regions applied to the scaled frame for tiled LED walls |
| **Max output** | 3840×2160 (4K UHD) |

Example — upscale a 1080p source to 4K:

```json
{
  "video": {
    "width": 1920, "height": 1080,
    "tx_url": "source_1080p.mp4"
  },
  "tx_video": {
    "scale_width": 3840, "scale_height": 2160,
    "fps": 30, "fmt": "yuv422p10le"
  },
  "tx_sessions": [
    { "udp_port": 20000, "payload_type": 96, "crop": { "x": 0, "y": 0, "w": 3840, "h": 2160 } }
  ]
}
```

Log output when scaling is active:

```
[INFO ] Video: 1920x1080 -> scale 3840x2160 30fps yuv422p10le ...
[INFO ]   Session 0: udp_port=20000 pt=96 crop=[0,0 3840x2160]
```

> **Note:** When scaling is set, crop bounds are validated against the **scaled** dimensions. Both
> fields must be specified together, and the scaled size must satisfy the format's chroma
> alignment (e.g. `yuv420` requires even width and height; `yuv422*` requires even width).

## Performance Considerations

- **Hugepage memory**: all frame buffers are allocated on hugepages.
- **Minimal copying**: frames are cropped and handed directly to MTL.
- **Bandwidth**: uncompressed video is large — 1080p30 4:2:2 10-bit is ≈1.3 Gb/s per full-frame
  stream; 4:4:4 / 12-bit and higher frame rates scale proportionally. Size NICs accordingly.
- **Hardware decode** frees CPU but does not raise throughput.

## Troubleshooting

Common install/host issues (MTL init, hugepages, VFIO) are in the
[top-level troubleshooting](../README.md#troubleshooting).

### Cannot load the source file

```bash
ls -la /path/to/video.mp4   # exists and readable
ffprobe /path/to/video.mp4  # valid video stream
df -h .                     # enough disk space
```

- Make sure the video's resolution matches `video.width`/`height`.
- Symlinked config files are rejected for security reasons.

### Hardware decode falls back to the CPU

With `"hwaccel": true` the log shows `(hwaccel=none)`, or `(hwaccel=vaapi)` followed by
`Hardware decode: vaapi surface unavailable for this stream, decoding on CPU`. The run continues
on the CPU by design.

| Log message | Cause | Fix |
|-------------|-------|-----|
| `decoder '<name>' has no VA-API configuration` | FFmpeg built without `--enable-vaapi`, or the codec has no VA-API support | Rebuild FFmpeg with `--enable-vaapi`; verify with `ffmpeg -hwaccels \| grep vaapi` |
| `cannot open VA-API device` | No render node, or no permission on it | Check `/dev/dri/renderD128` exists and add the user to the `render` group |
| `surface unavailable for this stream, decoding on CPU` | GPU cannot decode this profile, **or** the source is RGB | Check `vainfo` for a matching `VAProfile*` with `VAEntrypointVLD`; RGB (`gbrp*`) sources never use VA-API |

If a system-wide FFmpeg without VA-API shadows a locally built one, point the loader at the
correct build: `export LD_LIBRARY_PATH=/path/to/vaapi-ffmpeg/lib`.

### Screen capture fails

- `x11grab input format not found` — FFmpeg was built without `x11grab`; install the `libxcb*`
  packages and rebuild FFmpeg.
- Blank or wrong frames — the desktop is running Wayland; see
  [Ensuring an X11 Session](#ensuring-an-x11-session).

### PTP enabled but transmission stalls

No grandmaster is reachable on the link. Either provide a grandmaster or set `ptp.enable` to
`false`.
