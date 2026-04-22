# TxApp - TX-Only Media Transport Application

## Overview

TxApp is a simplified, standalone transmitter application using FFMPEG APIs and Media Transport Library (MTL) plugin. It provides clean TX-only functionality without the complexity of RX/TX interdependencies.

## Notices

### FFmpeg

FFmpeg is an open source project licensed under LGPL and GPL. See https://www.ffmpeg.org/legal.html. You are solely responsible for determining if your use of FFmpeg requires any additional licenses. Intel is not responsible for obtaining any such licenses, nor liable for any licensing fees due, in connection with your use of FFmpeg.

## Features

- **ST20P Video Transmission**: Uncompressed video over SMPTE ST 2110-20
- **Multi-session Support**: Multiple concurrent video streams
- **JSON Configuration**: Per-session crop and network settings via JSON config
- **Memory Efficient**: Uses hugepages for optimal performance

## Building

### Prerequisites

#### Hardware Requirements
- Intel 12th Generation CPU or above 
- Intel Ethernet Controller I225 or above

#### Software Requirements
- [Ubuntu 22.04 LTS](https://releases.ubuntu.com/22.04)
- [Media Transport Library (MTL) v26.01](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/v26.01/doc/build.md)
  - Follow these steps
    - [Install APT packages](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/v26.01/doc/build.md#111-ubuntudebian)
    - Clone Media-Transport-Library
      ```
      git clone https://github.com/OpenVisualCloud/Media-Transport-Library.git
      git checkout v26.01
      export mtl_source_code=${PWD}/Media-Transport-Library
      ```
    - [Build and install DPDK](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/v26.01/doc/build.md#2-dpdk-build-and-install)
    - [Build and install MTL](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/v26.01/doc/build.md#3-build-media-transport-library-and-app)
- [FFmpeg 7.0 with MTL Plugin](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/v26.01/ecosystem/ffmpeg_plugin/README.md#1-build)

### Build Steps

Once all dependencies are installed, clone this repository and run the build script:

```bash
git clone https://github.com/intel-innersource/applications.media.ledvideowall.gardencove-next.transmitter-app
cd applications.media.ledvideowall.gardencove-next.transmitter-app
./build.sh
```

The built binary will be available at `build/TxApp`.

## Usage

### JSON Configuration

TxApp uses a JSON config file with three sections:

| Section | Field | Description |
|---------|-------|-------------|
| **log_file** | `log_file` | (Optional) Path/name of the log output file (e.g. `TxApp.log`). If omitted, logging goes to console only. |
| **interfaces** | `name` | PCI BDF address of the NIC (e.g. `0000:06:00.0`) |
| | `sip` | Source IP address |
| | `dip` | Destination multicast IP address |
| **video** | `width` | Frame width in pixels |
| | `height` | Frame height in pixels |
| | `fps` | Frames per second (25, 30, 50, 60) |
| | `fmt` | Pixel format (`yuv422p10le`, `yuv420`, `yuv444p10le`, `gbrp10le`) |
| | `tx_url` | Path to the source video file |
| **tx_sessions[]** | `udp_port` | UDP port for the session |
| | `payload_type` | RTP payload type (typically 96) |
| | `crop` | Region to transmit: `x`, `y`, `w`, `h` in pixels |

Example (`config/tx_1session.json`):
```json
{
  "log_file": "TxApp.log",
  "interfaces": [
    { "name": "0000:06:00.0", "sip": "192.168.50.29", "dip": "239.168.85.20" }
  ],
  "video": {
    "width": 1920, "height": 1080, "fps": 30,
    "fmt": "yuv422p10le",
    "tx_url": "bbb_sunflower_1080p_30fps_normal.mp4"
  },
  "tx_sessions": [
    { "udp_port": 20000, "payload_type": 96, "crop": { "x": 0, "y": 0, "w": 1920, "h": 1080 } }
  ]
}
```

Multiple sessions can be defined in `tx_sessions` to transmit different crop regions of the same video simultaneously (see `config/tx_3sessions.json`).

## Logging

TxApp includes a built-in logger with configurable output targets and log levels.

### Log File

Specify a log file in the JSON config with the top-level `log_file` field:

```json
{
  "log_file": "TxApp.log",
  ...
}
```

When `log_file` is set, log output is written to that file in addition to the console. If the field is omitted, output goes to the console only.

### Log Levels

| Level | Description |
|-------|-------------|
| `ERROR` | Critical failures only |
| `WARN` | Non-fatal warnings |
| `INFO` | General operational messages (default) |
| `DEBUG` | Verbose diagnostic output |

### Examples

#### Using JSON Configuration (recommended)
```bash
./build/TxApp --config config/tx_1session.json
```

## Supported Formats

### Video Formats
- **yuv422p10le**: YUV 4:2:2 10-bit little endian (default)
- **yuv420**: YUV 4:2:0 8-bit
- **yuv444p10le**: YUV 4:4:4 10-bit little endian
- **gbrp10le**: RGB (GBR planar) 10-bit little endian

### Frame Rates
- 25 fps
- 30 fps
- 50 fps
- 60 fps

### Resolutions
- Tested with 1920x1080

## Performance Considerations

### Optimization Features
- **Hugepage Memory**: All buffers allocated on hugepages
- **Zero-copy Design**: Minimal memory copying
- **Hardware Acceleration**: Uses MTL's hardware features
- **Efficient Threading**: Minimal context switching

## Troubleshooting

### IOMMU / VFIO Kernel Parameters (GRUB)

MTL requires IOMMU and VFIO kernel parameters to be set. If MTL fails to initialize or VFIO devices are not accessible, check that the following parameters are present in `/etc/default/grub`:

```
intel_iommu=on iommu=pt pcie_aspm=off pcie_port_pm=off vfio-pci.disable_idle_d3=1
```

To add them manually:

1. Open `/etc/default/grub` in a text editor:
   ```bash
   sudo nano /etc/default/grub
   ```

2. Append the missing parameters to `GRUB_CMDLINE_LINUX_DEFAULT`, for example:
   ```
   GRUB_CMDLINE_LINUX_DEFAULT="quiet splash intel_iommu=on iommu=pt pcie_aspm=off pcie_port_pm=off vfio-pci.disable_idle_d3=1"
   ```

3. Update GRUB and reboot:
   ```bash
   sudo update-grub
   sudo reboot
   ```

> **Note:** A reboot is required for the IOMMU/VFIO parameters to take effect.

### VFIO Group Setup

MTL uses VFIO to access the NIC. The current user must belong to the `vfio` group.

1. Create the `vfio` group if it does not exist:
   ```bash
   sudo groupadd vfio
   ```

2. Add your user to the group:
   ```bash
   sudo usermod -aG vfio $USER
   ```

3. Apply the group membership without logging out:
   ```bash
   newgrp vfio
   ```

   Or log out and back in for the change to take effect permanently.

4. Verify group membership:
   ```bash
   id -nG $USER
   ```

### Common Issues

1. **MTL Initialization Failed**
   - Check network port PCI address
   - Ensure MTL library is properly installed
   - Verify network card is supported

2. **Cannot Load Source File**  
   - Check file permissions
   - Ensure sufficient disk space
   - Verify file format matches parameters

3. **Network Transmission Issues**
   - Verify multicast routing
   - Check firewall settings
   - Ensure network card supports required bandwidth

