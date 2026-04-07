# TxApp - TX-Only Media Transport Application

## Overview

TxApp is a simplified, standalone transmitter application using FFMPEG APIs and Media Transport Library (MTL) plugin. It provides clean TX-only functionality without the complexity of RX/TX interdependencies.

## Features

- **ST20P Video Transmission**: Uncompressed video over SMPTE ST 2110-20
- **Multi-session Support**: Multiple concurrent video/audio streams
- **JSON Configuration**: Per-session crop and network settings via JSON config
- **Memory Efficient**: Uses hugepages for optimal performance
- **Signal Handling**: Graceful shutdown on SIGINT/SIGTERM

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
  --port <pci_addr>       Network port PCI address (default: 0000:af:01.0)
  --dip <ip>              Destination IP address (default: 239.168.85.20)
  --udp_port <port>       Base UDP port (default: 20000)
  --width <width>         Video width (default: 1920)
  --height <height>       Video height (default: 1080)
  --fps <fps>             Frame rate: 25, 30, 50, 60 (default: 25)
  --fmt <format>          Pixel format: yuv422p10le, yuv420p (default: yuv422p10le)
  --tx_url <file>         Video source file
  --st20p_sessions <n>    Number of ST20P sessions (default: 1)
  --time <seconds>        Test duration in seconds (0=infinite)
  --help                  Show help
```

### Examples

#### Using JSON Configuration (recommended)
```bash
./build/TxApp --config config/tx_3sessions.json
```



## Supported Formats

### Video Formats
- **yuv422p10le**: YUV 4:2:2 10-bit little endian (default)
- **yuv420p**: YUV 4:2:0 8-bit
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

## Architecture

### Core Components

1. **Main Application Context** (`tx_app_context`)
   - MTL library management
   - Global configuration
   - Session coordination

2. **ST20P TX Context** (`st20p_tx_ctx`)
   - Video session management
   - Frame processing thread
   - Source data handling


### Threading Model

- **Main Thread**: Application control and coordination
- **ST20P Threads**: One per video session for frame processing
- **Signal Handler**: Graceful shutdown management

### Memory Management

- **Hugepages**: Used for all media buffers for performance
- **Circular Buffers**: Continuous playback from source files
- **Automatic Cleanup**: Proper resource deallocation on exit

## Network Configuration

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

## Performance Considerations

### Optimization Features
- **Hugepage Memory**: All buffers allocated on hugepages
- **Zero-copy Design**: Minimal memory copying
- **Hardware Acceleration**: Uses MTL's hardware features
- **Efficient Threading**: Minimal context switching

### Resource Usage
- **Memory**: ~100MB per video session + source file size
- **CPU**: ~5-10% per session (varies by resolution/fps)
- **Network**: Bandwidth = width × height × fps × bits_per_pixel

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

### Debug Information

The application provides runtime information including:
- Session creation status
- Frame/packet transmission counters
- Error messages and warnings
- Graceful shutdown confirmation


## Source File Format

### Video Files
- Raw YUV format matching specified parameters
- File size should be multiple of frame size
- Continuous looping playback


## Integration

### With Other Applications
- Standard ST 2110 output compatible with professional equipment  
- Works with MTL RX applications
- Compatible with broadcast infrastructure

### Monitoring
- Built-in frame counters
- Runtime statistics
- Graceful error handling

This application provides a solid foundation for ST 2110 transmission testing and can be extended for specific use cases.
