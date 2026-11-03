# dvledrx — Receiver

dvledrx receives one or more SMPTE ST 2110-20 video streams through the Media Transport Library
(MTL) and renders them on X11 displays. It is the counterpart of [dvledtx](../dvledtx/README.md):
each stream is typically one crop region of a larger frame, and dvledrx can reassemble the regions
into one composite picture or drive each region on its own physical output (HDMI/DP) to build a
multi-monitor video wall.

> Install, host setup, build and the basic run command are shared with the transmitter and live in
> the [top-level README](../README.md).

## Table of Contents

- [Features](#features)
- [Usage](#usage)
  - [Command-Line Options](#command-line-options)
  - [X11 Requirements](#x11-requirements)
- [JSON Configuration](#json-configuration)
  - [Configuration Reference](#configuration-reference)
  - [Validation Rules](#validation-rules)
  - [Example: Single Session](#example-single-session)
  - [Sample Configurations](#sample-configurations)
  - [Display Modes](#display-modes)
  - [PTP](#ptp)
  - [CPU Usage (`tasklet_sleep`)](#cpu-usage-tasklet_sleep)
- [Logging](#logging)
- [Supported Formats](#supported-formats)
- [Multi-NIC and Bandwidth](#multi-nic-and-bandwidth)
- [Virtual Functions (VFs)](#virtual-functions-vfs)
- [Troubleshooting](#troubleshooting)

## Features

- **ST 2110-20 reception** of up to 16 sessions on up to 8 NICs
- **Three display modes**: single window, composite of all sessions, or one window per XRandR
  output
- **Video-wall placement**: put sessions on named outputs (`DP-1`, `HDMI-2`, …) at fixed
  positions, scaled, and mirrored on up to 4 additional outputs
- **7 pixel formats** including 12-bit YUV/RGB
- **Optional MTL PTP client**
- **JSON configuration** with strict validation and per-NIC bandwidth checks
- **Rotating, compressed log files**

## Usage

From the repository root, after [building](../README.md#build):

```bash
export DISPLAY=:0
./build/dvledrx/dvledrx --config dvledrx/config/rx_fullhd_1session.json

# receive for 60 seconds then exit
./build/dvledrx/dvledrx --config dvledrx/config/rx_fullhd_1session.json --test-time 60
```

### Command-Line Options

| Option | Description |
|--------|-------------|
| `-C, --config <file>` | JSON config file (required) |
| `-t, --test-time <secs>` | Receive for N seconds (1..86400), then exit |
| `-v, --version` | Show version and exit |
| `--help` | Show help message |

### X11 Requirements

dvledrx renders with Xlib (MIT-SHM when available), so it needs:

- A running **X11 (Xorg)** server — Wayland sessions are not supported. On Ubuntu choose
  **"Ubuntu on Xorg"** at the login screen or set `WaylandEnable=false` in
  `/etc/gdm3/custom.conf` (see [dvledtx — Ensuring an X11 Session](../dvledtx/README.md#ensuring-an-x11-session)).
- `DISPLAY` set to that server (e.g. `export DISPLAY=:0`). Without it dvledrx logs
  `DISPLAY environment variable not set` and runs without a window.
- XRandR for per-output placement (`xrandr --listmonitors` shows the output names to use).

When running as root via `sudo`, allow root to connect and preserve the environment:

```bash
export DISPLAY=:0
xhost +si:localuser:root
sudo -E ./build/dvledrx/dvledrx --config <file>
```

dvledrx hands its shared-memory segments to the invoking user (`SUDO_UID`) so a rootless Xorg can
attach to them.

## JSON Configuration

dvledrx is configured entirely by a JSON file passed with `--config`. Sample files are in
[config/](config/).

### Configuration Reference

| Section | Field | Description |
|---------|-------|-------------|
| **log_file** | `log_file` | (Optional) Log output file (e.g. `RxApp.log`). If omitted, logs go to the console. See [Logging](#logging). |
| **log_level** | `log_level` | (Optional) `debug`, `info` (default), `warn` or `error` |
| **display_mode** | `display_mode` | (Optional) `composite` (default) or `per_output`. See [Display Modes](#display-modes). |
| **tasklet_sleep** | `tasklet_sleep` | (Optional) Let the MTL scheduler sleep when idle to save a busy core. Default `false`. See [CPU Usage](#cpu-usage-tasklet_sleep). |
| **interfaces[]** | `nic_index` | (Optional) ID referenced by `rx_sessions[].nic_index`. Default: the entry's position in the array. |
| | `name` | PCI BDF address of the NIC or VF (e.g. `0000:03:00.1`) |
| | `sip` / `SIP` | Source IP address of this NIC |
| | `dip` / `DIP` | Multicast group to join (the transmitter's `dip`) |
| **video** | `width` | (Optional) Full frame width. Default: bounding box of all crops. |
| | `height` | (Optional) Full frame height. Default: bounding box of all crops. |
| | `fps` | (Optional) 24, 25, 29 (29.97), 30, 50, 59 (59.94) or 60. Default `30`. |
| | `fmt` | (Optional) Pixel format (see [Supported Formats](#supported-formats)). Default `yuv422p10le`. |
| **ptp** | `enable` | (Optional) Enable MTL's built-in PTP client. Default `false`. See [PTP](#ptp). |
| | `pi` | (Optional) Use the PI controller (physical-function NICs only). Default `false` |
| | `unicast` | (Optional) Send `PTP_DELAY_REQ` unicast. Default `false` |
| **rx_sessions[]** | `nic_index` | (Optional) Interface (`interfaces[].nic_index`) this stream arrives on. Default `0` |
| | `udp_port` | (Optional) UDP port of the stream. Default `20000` |
| | `payload_type` | (Optional) RTP payload type, 96..127. Default `96` |
| | `crop` | Size and position of this stream in the full frame: `x`, `y`, `w`, `h`. `w`×`h` must equal the transmitted stream size. |
| | `output` | (Optional, `per_output`) XRandR output name, e.g. `HDMI-1`. Empty: the *i*-th active monitor. |
| | `window` | (Optional, `per_output`) `x`, `y`, `w`, `h` — fixed-size window at `x,y` on the output instead of full screen (`w`/`h` `0` = crop size) |
| | `scale` | (Optional, `per_output`) `w`, `h` — render size; overrides `window.w/h` |
| **displays[]** | `output` | XRandR output name. Presence of `displays` implies `per_output`. |
| | `sessions[]` | Sessions placed on this output: `session` (index into `rx_sessions[]`), `x`, `y`, `w`, `h`, `scale_w`, `scale_h` |

### Validation Rules

The config is validated at start-up; on error dvledrx logs the offending field and exits.

- At least one interface and one session; up to 8 interfaces and 16 sessions (extra entries are
  ignored with a warning).
- Each interface needs a PCI `name` and valid `sip` and `dip`; `nic_index` values and names must be
  unique.
- Frame size at most 4K (3840×2160 or 2160×3840).
- Each crop must be non-empty and lie inside the `video` frame.
- `udp_port` 1..65535; `payload_type` 96..127; sessions on the same NIC must use different ports.
- `rx_sessions[].nic_index` must match an interface.
- `displays[].sessions[].session` must reference an existing session; a session can be placed on at
  most 5 outputs. Window/scale values must be 0..3840 and `scale_w`/`scale_h` set together.
- The log file path must be safe (see [Logging](#logging)).

### Example: Single Session

[`config/rx_fullhd_1session.json`](config/rx_fullhd_1session.json):

```json
{
  "log_file": "RxApp.log",
  "log_level": "info",
  "interfaces": [
    { "name": "0000:03:00.1", "SIP": "192.168.50.65", "DIP": "239.168.85.20" }
  ],
  "video": {
    "width": 960, "height": 1080,
    "fps": 30,
    "fmt": "gbrp10le"
  },
  "ptp": { "enable": false, "pi": false, "unicast": false },
  "rx_sessions": [
    { "udp_port": 20000, "payload_type": 96, "crop": { "x": 0, "y": 0, "w": 960, "h": 1080 } }
  ]
}
```

The multicast `DIP`, `udp_port`, `payload_type`, `fmt`, `fps` and crop size must match the
transmitter — see the [end-to-end table](../README.md#end-to-end-example).

### Sample Configurations

Edit `interfaces[].name`, the IP addresses and output names for your setup before use.

| File | Frame | Sessions | NICs | Format | Display |
|------|-------|----------|------|--------|---------|
| [`rx_fullhd_1session.json`](config/rx_fullhd_1session.json) | 960×1080 | 1 | 1 | `gbrp10le` | Single window; includes a disabled `ptp` block |
| [`rx_crop_1session.json`](config/rx_crop_1session.json) | 1920×1080 | 1 (640×1080 strip) | 1 | `yuv444p12le` | Single window |
| [`rx_2k_1session.json`](config/rx_2k_1session.json) | 2560×1440 | 1 | 1 | `yuv422p10le` | Single window |
| [`rx_4k_1session.json`](config/rx_4k_1session.json) | 3840×2160 | 1 | 1 | `gbrp12le` | Single window |
| [`rx_crop_3sessions.json`](config/rx_crop_3sessions.json) | 1920×1080 | 3 | 1 | `gbrp12le` | Composite |
| [`rx_8nic_8sessions.json`](config/rx_8nic_8sessions.json) | 1920×1080 | 8 | 8 | `yuv422p10le` | Composite of 240×1080 strips |
| [`rx_crop_3sessions_per_output.json`](config/rx_crop_3sessions_per_output.json) | 1920×1080 | 2 | 2 | `yuv422p10le` | Per output: both sessions on `DP-1` and mirrored on `HDMI-2` |
| [`rx_4k_2nic_2sessions_per_output.json`](config/rx_4k_2nic_2sessions_per_output.json) | 3840×2160 (from crops) | 2 | 2 | `yuv422p10le` | Per output on `DP-1` + `HDMI-2`; `tasklet_sleep` |

### Display Modes

The mode is chosen from the number of sessions and the optional `display_mode` / `displays` keys:

| Sessions | `display_mode` | Result |
|----------|----------------|--------|
| 1 | absent | **Single** — one window sized to the session's crop |
| > 1 | absent or `composite` | **Composite** — one window containing all sessions at their crop positions |
| any | `per_output` (or a `displays` section is present) | **Per output** — one borderless full-screen window per session, on an XRandR output |

#### Single Session

One window the size of `crop.w × crop.h` (or `video.width × height` if no crop is given). A
dedicated display thread renders the newest frame; if rendering falls behind, older frames are
dropped so reception is never blocked.

#### Composite

A single window sized to the bounding box of all crop regions is created. Each session's frame is
written at its `crop.x, crop.y` position, and a per-frame barrier flushes the full picture once
every session has delivered. Use this to view a tiled stream on one monitor
([`config/rx_crop_3sessions.json`](config/rx_crop_3sessions.json)):

```json
"video": { "width": 1920, "height": 1080, "fps": 30, "fmt": "gbrp12le" },
"rx_sessions": [
  { "udp_port": 20000, "payload_type": 96, "crop": { "x": 0,    "y": 0, "w": 640, "h": 1080 } },
  { "udp_port": 20002, "payload_type": 96, "crop": { "x": 640,  "y": 0, "w": 640, "h": 1080 } },
  { "udp_port": 20004, "payload_type": 96, "crop": { "x": 1280, "y": 0, "w": 640, "h": 1080 } }
]
```

#### Per Output (Video Wall)

Each session gets its own window on a physical output. There are two ways to configure it:

**1. Per-session keys** with `"display_mode": "per_output"` — set `output`, and optionally
`window` and `scale`, on each `rx_sessions[]` entry:

```json
"display_mode": "per_output",
"rx_sessions": [
  { "udp_port": 20000, "crop": { "x": 0,   "y": 0, "w": 960, "h": 1080 }, "output": "HDMI-1" },
  { "udp_port": 20002, "crop": { "x": 960, "y": 0, "w": 960, "h": 1080 }, "output": "HDMI-2",
    "window": { "x": 100, "y": 0 }, "scale": { "w": 540, "h": 1080 } }
]
```

**2. A top-level `displays` section** (implies `per_output`), which lists the sessions placed on
each output ([`config/rx_crop_3sessions_per_output.json`](config/rx_crop_3sessions_per_output.json)):

```json
"displays": [
  {
    "output": "DP-1",
    "sessions": [
      { "session": 0, "x": 0,   "y": 0, "w": 960, "h": 1080, "scale_w": 540, "scale_h": 1920 },
      { "session": 1, "x": 540, "y": 0, "w": 960, "h": 1080, "scale_w": 540, "scale_h": 1920 }
    ]
  },
  {
    "output": "HDMI-2",
    "sessions": [
      { "session": 0, "x": 0,   "y": 0, "w": 960, "h": 1080, "scale_w": 540, "scale_h": 1920 },
      { "session": 1, "x": 540, "y": 0, "w": 960, "h": 1080, "scale_w": 540, "scale_h": 1920 }
    ]
  }
]
```

- `session` is the index into `rx_sessions[]`; `x`/`y` position the window on the output and
  `scale_w`/`scale_h` (if set) give its size.
- The **first** placement of a session is its primary window; each further placement on another
  output is a **mirror** of the same frames (up to 4 mirrors).

#### Keyboard and Window Controls

| Action | Effect |
|--------|--------|
| `Esc` | In per-output mode, toggles every wall window between borderless full screen and normal titled windows (works from any window). Press again to return. |
| Close a window | Stops dvledrx |
| `Ctrl+C` / `SIGTERM` / `Ctrl+\` | Clean shutdown |

### PTP

dvledrx can opt in to MTL's built-in PTP client with the same top-level block used by dvledtx:

```json
"ptp": {
  "enable":  true,
  "pi":      false,
  "unicast": false
}
```

The option is `false` by default. RX PTP only initializes MTL's clock client; it does **not**
schedule frames for presentation or guarantee synchronized physical display scan-out across
receivers. A grandmaster must be present on the network for the client to lock.

### CPU Usage (`tasklet_sleep`)

MTL's scheduler busy-polls a core by default. Set `"tasklet_sleep": true` to let it sleep when
idle (capped at roughly 128 packet times for RX sessions), saving a core at a small latency cost.

## Logging

| Setting | Behaviour |
|---------|-----------|
| `log_file` (config) | Write logs to this file instead of the console. |
| `log_level` (config) | `debug`, `info` (default), `warn` or `error`. |
| `RXAPP_LOG_LEVEL` (environment) | Sets the level before the config is read (the config value wins). |

- The log file rotates at **20 MB**; the old file is gzipped in the background, keeping
  `<log>.1.gz` … `<log>.10.gz`.
- For safety the log path is rejected (falling back to the console) when its directory does not
  exist, any directory on the path is world-writable (e.g. `/tmp`) or owned by another user, or
  the file is a symlink or device.

## Supported Formats

`video.fmt` must match the transmitter's `tx_video.fmt`:

| Format | Chroma | Bit Depth | Color Space | Wire bits/pixel |
|--------|--------|-----------|-------------|-----------------|
| `yuv422p10le` (default) | 4:2:2 | 10-bit | YUV | 20 |
| `yuv422p12le` | 4:2:2 | 12-bit | YUV | 24 |
| `yuv444p10le` | 4:4:4 | 10-bit | YUV | 30 |
| `yuv444p12le` | 4:4:4 | 12-bit | YUV | 36 |
| `gbrp10le` | 4:4:4 | 10-bit | RGB | 30 |
| `gbrp12le` | 4:4:4 | 12-bit | RGB | 36 |
| `yuv420` | 4:2:0 | 8-bit | YUV | 12 |

Frame rates: 24, 25, 29 (29.97), 30, 50, 59 (59.94) and 60 fps. Maximum frame size is 4K
(3840×2160, or 2160×3840 in portrait).

## Multi-NIC and Bandwidth

- Up to 8 `interfaces[]` (MTL port limit) and 16 `rx_sessions[]`.
- Each session selects its interface with `nic_index`, which refers to the interface's
  `nic_index` value.
- At start-up dvledrx estimates the bandwidth per NIC (`w × h × bpp × fps` + 5 % header overhead)
  and warns when a NIC would need more than 10 Gb/s:
  ```
  interfaces[0] (0000:03:00.0) needs ~12345 Mb/s, above a 10000 Mb/s link
  ```

## Virtual Functions (VFs)

When an interface is an SR-IOV VF, dvledrx prepares its parent PF so multicast reaches the VF:
it enables PF promiscuous mode, sets the VF to `trust on` and `spoofchk off`, and brings the PF
link up (`ip link …`). This needs root or `CAP_NET_ADMIN`; without it a warning is logged
(`VF setup: ... failed (need root/CAP_NET_ADMIN?)`) and reception may not work. Alternatively
create trusted VFs up front with `nicctl.sh create_tvf <PF BDF>`.

## Troubleshooting

Common install/host issues (MTL init, hugepages, VFIO) are in the
[top-level troubleshooting](../README.md#troubleshooting).

| Symptom | Cause / Fix |
|---------|-------------|
| `DISPLAY environment variable not set` | `export DISPLAY=:0` (or the right display). Under `sudo`, use `sudo -E`. |
| `Cannot open X11 display ':0'` | Xorg not running, or no permission — run `xhost +si:localuser:root` when using `sudo`. |
| Black window, nothing received | Check the multicast `dip`, `udp_port`, `payload_type`, `fmt`, `fps` and crop size match the transmitter (see the [end-to-end table](../README.md#end-to-end-example)). |
| `rv_slot_by_tmstamp` errors on every packet | `video.fps` does not match the sender's frame rate. |
| `No active monitor matches HDMI-1 ...` | Output name is wrong or the monitor is off — check `xrandr --listmonitors`. |
| `VF setup: ... failed (need root/CAP_NET_ADMIN?)` | Run as root, or pre-create trusted VFs (`nicctl.sh create_tvf`). |
| `log_file '...' rejected: ... world-writable` | Use a private directory for the log file (not `/tmp`). |
| `Config file ... is invalid` | The preceding log line names the field; see [Validation Rules](#validation-rules). |
