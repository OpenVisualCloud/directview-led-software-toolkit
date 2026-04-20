# TxApp - TX-Only Media Transport Application

## Overview

TxApp is a simplified, standalone transmitter application using FFMPEG APIs and Media Transport Library (MTL) plugin. It provides clean TX-only functionality without the complexity of RX/TX interdependencies.

## Features

- **ST20P Video Transmission**: Uncompressed video over SMPTE ST 2110-20
- **Multi-session Support**: Multiple concurrent video streams
- **JSON Configuration**: Per-session crop and network settings via JSON config
- **Memory Efficient**: Uses hugepages for optimal performance

## Building

### Prerequisites
- MTL library installed (`libmtl`)
- Meson build system
- GCC compiler with pthread support

### Build Steps
```bash
./build.sh
```

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
| | `fmt` | Pixel format (`yuv422p10le`, `yuv420p`, `yuv444p10le`, `gbrp10le`) |
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
- **yuv420custom8**: YUV 4:2:0 8-bit
- **yuv444p10le**: YUV 4:4:4 10-bit little endian
- **gbrp10le**: RGB (GBR planar) 10-bit little endian

### Frame Rates
- 25 fps
- 30 fps
- 50 fps
- 60 fps

### Resolutions
- Any resolution supported by ST 2110-20
- Common: 1920x1080, 3840x2160, 1280x720

## Performance Considerations

### Optimization Features
- **Hugepage Memory**: All buffers allocated on hugepages
- **Zero-copy Design**: Minimal memory copying
- **Hardware Acceleration**: Uses MTL's hardware features
- **Efficient Threading**: Minimal context switching

## Troubleshooting

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

## Notices

### FFmpeg

FFmpeg is an open source project licensed under LGPL and GPL. See https://www.ffmpeg.org/legal.html. You are solely responsible for determining if your use of FFmpeg requires any additional licenses. Intel is not responsible for obtaining any such licenses, nor liable for any licensing fees due, in connection with your use of FFmpeg.
