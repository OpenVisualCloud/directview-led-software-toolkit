# TxApp - TX-Only Media Transport Application

## Overview

TxApp is a simplified, standalone transmitter application using FFMPEG APIs and Media Transport Library (MTL) plugin. It provides clean TX-only functionality without the complexity of RX/TX interdependencies.


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

### Command Line Arguments

```
Usage: TxApp [OPTIONS]

Options:
  --port <pci_addr>       Network port PCI address (default: 0000:06:00.0)
  --dip <ip>              Destination IP address (default: 239.168.85.20)
  --udp_port <port>       Base UDP port (default: 20000)
  --width <width>         Video width (default: 1920)
  --height <height>       Video height (default: 1080)
  --fps <fps>             Frame rate: 30, 60 
  --fmt <format>          Pixel format: yuv422p10le
  --tx_url <file>         Video source file
  --st20p_sessions <n>    Number of ST20P sessions (default: 1)
  --time <seconds>        Test duration in seconds (0=infinite)
  --help                  Show help
```

### Examples

#### Basic Video Transmission
```bash
./build/TxApp --port 0000:06:00.0 --dip 239.168.85.20 --tx_url test.mp4
```

#### Multiple Video Sessions
```bash
./build/TxApp --st20p_sessions 2 --dip 239.168.85.20
```


# Supported Formats

### Video Formats
- **yuv422p10le**: YUV 4:2:2 10-bit little endian (default)

### Frame Rates
- 30 fps  
- 60 fps

### Default Settings
- **Port**: 0000:06:00.0 (modify for your network card)
- **Destination IP**: 239.168.85.20 (multicast)
- **Base UDP Port**: 20000
- **Port Allocation**: 
  - ST20P sessions: base_port + (session_id * 2)

### Multi-session Addressing
Each session gets unique UDP ports to avoid conflicts:
- Session 0: UDP 20000
- Session 1: UDP 20002  
- Session 2: UDP 20004
- etc.

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
