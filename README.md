# AES67 macOS Audio Driver

> **EXPERIMENTAL SOFTWARE - NOT VALIDATED**
>
> This driver has been written and unit tested, but has **never been tested with real AES67 hardware** or validated in production environments. Core functionality (audio actually flowing through the driver) is **unverified**. The driver may not load, may not work with DAWs, and audio may not flow even if configured correctly.
>
> **Do not use for production audio work.** Suitable for development, experimentation, and helping test only.

An open-source virtual audio driver for macOS that aims to bring AES67/RAVENNA/Dante network audio support to Mac. Built as a user-space AudioServerPlugIn using the libASPL framework.

## Project Status

| Aspect | Status | Notes |
|--------|--------|-------|
| Code Written | ~85% | Core components implemented |
| Unit Tests | Pass (577+ assertions) | Isolated component testing only |
| Compilation | Not compling | build errors |
| Hardware Testing | **0%** | Never tested with real devices |
| End-to-End Validation | **Unknown** | Audio path unverified |
| DAW Compatibility | **Unknown** | Not tested with Logic, Pro Tools, QLab |
| Production Ready | **No** | Development/testing only |

### What's Actually Known to Work

---

### What's Unknown (Never Tested)

- Whether Core Audio (coreaudiod) successfully loads the driver
- Whether audio callbacks fire correctly
- Whether audio data actually flows through the device
- Whether DAWs recognize and can use the device
- Whether RTP packets are correctly received from real networks
- Whether PTP synchronization works with real PTP masters
- Actual latency, stability, and performance characteristics
- Compatibility with Dante, RAVENNA, or AES67 hardware

## Architecture Overview

```
AES67Driver/
├── Driver/                  # AudioServerPlugIn (libASPL)
│   ├── AES67Device          # 128-channel Core Audio device structure
│   ├── AES67IOHandler       # RT-safe audio I/O (lock-free ring buffers)
│   ├── PlugInMain           # AudioServerPlugIn C API entry point
│   └── SDPParser            # SDP file parser (RFC 4566)
├── NetworkEngine/           # Network audio processing
│   ├── RTP/
│   │   ├── SimpleRTP        # Minimal RTP implementation (RFC 3550)
│   │   ├── RTPReceiver      # Network audio receiver with jitter buffer
│   │   ├── RTPTransmitter   # Network audio transmitter
│   │   └── LockFreeCircularJitterBuffer  # Packet reordering buffer
│   ├── PTP/
│   │   ├── PTPClock         # IEEE 1588 clock interface
│   │   ├── PhaseLockedLoop  # Clock drift compensation
│   │   └── PTPDInterface    # ptpd daemon wrapper
│   ├── Discovery/
│   │   ├── SAPListener      # SAP discovery (RFC 2974)
│   │   └── RTSPClient       # RTSP client (RFC 2326)
│   └── StreamManager        # Stream lifecycle management
├── Shared/                  # Common components
│   ├── RingBuffer.hpp       # Lock-free SPSC ring buffer
│   └── Types.h              # Common data structures
├── ManagerApp/              # SwiftUI configuration app
│   └── Views/               # Stream management UI
└── Tests/                   # Unit tests (577+ assertions)
```

## Technical Design

### Audio Path (Intended)

```
Network → RTPReceiver → JitterBuffer → Decode → ChannelMapper → RingBuffer → CoreAudio
```

**Note**: This path is designed but not validated end-to-end.

### Key Design Decisions

- **User-space driver**: AudioServerPlugIn runs in coreaudiod process, no kernel extensions
- **Lock-free audio thread**: Ring buffers and atomic operations, no allocations or locks in audio path
- **Jitter buffer**: 256-slot circular buffer with atomic state machine for packet reordering
- **PTP support**: Multi-domain IEEE 1588 with phase-locked loop for drift compensation
- **128 channels**: Bidirectional, configurable per-stream mapping

### Intended Specifications

| Feature | Design Target | Validated |
|---------|---------------|-----------|
| Channels | 128 in/out | No |
| Sample Rates | 44.1kHz - 384kHz | No |
| Bit Depths | L16, L24 | No |
| Latency | ~2-3ms (configurable) | No |
| Jitter Buffer | 256 packets | No |

## System Requirements

- macOS 13.0 (Ventura) or later
- Apple Silicon Mac (M1/M2/M3/M4)
- Network connection for AES67 streams
- Optional: ptpd for PTP synchronization

## Building from Source

### Prerequisites

```bash
# Install build tools
brew install cmake

# Install libASPL (AudioServerPlugIn framework)
brew tap gavv/gavv
brew install libaspl

# Optional: Boost for some lockfree containers
brew install boost
```

### Build

```bash
git clone https://github.com/maxajbarlow/AES67_macos_Driver.git
cd AES67_macos_Driver

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j

# Run unit tests
ctest

# Driver bundle at: build/AES67Driver.driver
```

### Installation (Manual)

```bash
# Copy driver to HAL plugins directory
sudo cp -R build/AES67Driver.driver /Library/Audio/Plug-Ins/HAL/

# Restart Core Audio
sudo killall coreaudiod

# Check if driver appears (may not work - untested)
# Look for "AES67 Device" in Audio MIDI Setup
```

**Warning**: The driver may not load or function correctly. This installation process is documented but not validated.

## Why No Kernel Extension Required

This driver uses Apple's modern AudioServerPlugIn architecture rather than deprecated kernel extensions (KEXTs):

- Runs entirely in user space within coreaudiod
- No System Integrity Protection (SIP) changes needed
- No "Reduced Security" boot mode required
- No Recovery Mode steps necessary
- Standard file copy installation

This is the correct, Apple-supported approach for modern macOS audio drivers.

## Manager Application

A SwiftUI configuration app is included at `ManagerApp/`:

**UI Features** (implemented):
- Stream list and detail views
- Add/remove stream configuration
- 128-channel mapping grid visualizer
- SDP file import (drag-and-drop)
- Quick Start wizard for first-run experience
- PTP diagnostic guidance
- Audio level meters
- Settings panel

**Backend Integration** (unverified):
- Communication with driver is designed but not tested
- Stream status monitoring may not reflect actual state
- Statistics display accuracy unknown

Build with:
```bash
cd ManagerApp
swift build
```

## Known Limitations

### Critical

- **No hardware validation** - May not work with any real AES67/Dante/RAVENNA equipment
- **No DAW testing** - Unknown if Logic Pro, Pro Tools, QLab, etc. will work
- **Audio path unverified** - Packets may not actually result in playable audio
- **PTP untested** - Clock synchronization may not function with real PTP masters

### Incomplete

- Installer package not created
- Code signing and notarization not done
- User documentation incomplete
- Error recovery under real network conditions unknown
- Buffer sizing may need adjustment for real-world latency

## Unit Test Coverage

The codebase includes comprehensive unit tests for isolated components:

```bash
./Tests/TestSDPParser        # SDP parsing
./Tests/TestChannelMapper    # Channel routing
./Tests/TestRingBuffer       # Lock-free buffers
./Tests/TestRTPReceiver      # RTP packet handling
./Tests/TestRTPTransmitter   # RTP sending
./Tests/TestPTPClock         # Timing logic
./Tests/TestStreamManager    # Stream orchestration
./Tests/TestMultiStream      # Multi-stream scenarios
```

**Important**: These tests validate component logic in isolation. They do not test real network conditions, actual Core Audio integration, or hardware compatibility.

## Help Wanted

This project needs real-world testing more than anything else.

### If You Have Access To:

- AES67 network audio devices
- Dante-enabled equipment
- RAVENNA systems
- PTP-capable network infrastructure
- Professional audio environments for testing

### What to Expect:

- The driver will likely not work correctly at first
- You will encounter bugs, crashes, or silent failures
- Debugging will require patience and technical skill
- Your feedback is essential to making this functional

### How to Help:

1. Try building and installing (expect issues)
2. Document exactly what happens (or doesn't)
3. Open GitHub issues with detailed logs
4. Be willing to iterate on fixes

## Contributing

Contributions welcome, especially:

- Hardware testing and validation reports
- Bug fixes with reproduction steps
- Documentation improvements
- Build system enhancements

### Guidelines

- C++17 standard
- Maintain lock-free audio thread
- Add unit tests for new code
- Update documentation

## License

MIT License - See LICENSE file.

### Dependencies

- **libASPL**: MIT License
- **Boost**: Boost Software License
- **ptpd**: BSD License (embedded)

## Acknowledgments

- [libASPL](https://github.com/gavv/libASPL) - Modern C++ AudioServerPlugIn framework
- AES67-2018 specification
- RFC 3550 (RTP), RFC 4566 (SDP), RFC 2974 (SAP)

---

*Build #19 - Code ~85% complete, hardware validation 0%*

*This is experimental software. The code exists and compiles, but whether it actually functions as an audio driver is unknown. Use at your own risk for testing and development only.*
