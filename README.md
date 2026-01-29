# AES67 macOS Audio Driver

> **EXPERIMENTAL SOFTWARE - DRIVER LOADS BUT AUDIO UNTESTED**
>
> This driver compiles and loads into macOS Core Audio successfully. The device appears in Audio MIDI Setup with 128 channels. However, **actual audio flow has not been tested** with real AES67 hardware or DAW software.
>
> **Do not use for production audio work.** Suitable for development, experimentation, and testing only.

An open-source virtual audio driver for macOS that brings AES67/RAVENNA/Dante network audio support to Mac. Built as a user-space AudioServerPlugIn using the libASPL framework.

## Project Status

| Aspect | Status | Notes |
|--------|--------|-------|
| Code Written | ~90% | Core components implemented |
| Compilation | ✅ **Verified** | Builds on Apple Silicon |
| Driver Loading | ✅ **Verified** | Loads into coreaudiod successfully |
| System Recognition | ✅ **Verified** | Appears in Audio MIDI Setup |
| Manager App | ✅ **Verified** | SwiftUI app builds and launches |
| Hardware Testing | **0%** | Never tested with real devices |
| DAW Compatibility | **Unknown** | Not tested with Logic, Pro Tools, etc. |
| Audio Flow | **Unknown** | End-to-end audio path unverified |

### What's Verified Working

- ✅ Code compiles without errors on Apple Silicon (arm64)
- ✅ Driver installs to `/Library/Audio/Plug-Ins/HAL/`
- ✅ coreaudiod loads the driver without crashing
- ✅ Device appears as "AES67 Device" in system audio preferences
- ✅ 128 input + 128 output channels reported correctly
- ✅ Manager app launches and displays UI
- ✅ Individual components pass unit tests

### What's Unknown (Needs Testing)

- Whether audio callbacks fire correctly when used by apps
- Whether audio data actually flows through the device
- Whether DAWs (Logic Pro, Pro Tools, Ableton) can record/playback
- Whether RTP packets are correctly received from real networks
- Whether PTP synchronization works with real PTP masters
- Actual latency, stability, and performance characteristics
- Compatibility with Dante, RAVENNA, or AES67 hardware

## Quick Start

### Prerequisites

```bash
# Install build tools
brew install cmake boost

# Install libASPL (AudioServerPlugIn framework)
brew tap gavv/gavv
brew install libaspl
```

### Build & Install

```bash
git clone https://github.com/yourusername/AES67_macos_Driver.git
cd AES67_macos_Driver

# Build the driver
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
make -j

# Install the driver
sudo cp -R AES67Driver.driver /Library/Audio/Plug-Ins/HAL/

# Restart Core Audio to load the driver
sudo killall coreaudiod

# Verify installation - should show "AES67 Device"
system_profiler SPAudioDataType | grep -A 5 "AES67"
```

### Build Manager App

```bash
cd ManagerApp
./build.sh

# Launch the app
open AES67Manager.app
```

## Architecture Overview

```
AES67Driver/
├── Driver/                  # AudioServerPlugIn (libASPL)
│   ├── AES67Device          # 128-channel Core Audio device
│   ├── AES67IOHandler       # RT-safe audio I/O (lock-free)
│   ├── PlugInMain           # AudioServerPlugIn entry point
│   └── SDPParser            # SDP file parser (RFC 4566)
├── NetworkEngine/           # Network audio processing
│   ├── RTP/
│   │   ├── SimpleRTP        # RTP implementation (RFC 3550)
│   │   ├── RTPReceiver      # Network audio receiver
│   │   ├── RTPTransmitter   # Network audio transmitter
│   │   └── LockFreeCircularJitterBuffer
│   ├── PTP/
│   │   ├── PTPClock         # IEEE 1588 clock interface
│   │   ├── PhaseLockedLoop  # Clock drift compensation
│   │   └── PTPDInterface    # PTP daemon interface (stub)
│   ├── Resampling/          # Sample rate conversion
│   └── Discovery/           # SAP/RTSP discovery
├── Shared/                  # Common components
│   ├── RingBuffer.hpp       # Lock-free SPSC ring buffer
│   ├── Types.h              # Common data structures
│   └── NonBlockingLogger    # RT-safe logging
├── ManagerApp/              # SwiftUI configuration app
└── Tests/                   # Unit tests
```

## Technical Specifications

| Feature | Implementation | Status |
|---------|---------------|--------|
| Channels | 128 in/out | Configured |
| Sample Rates | 44.1kHz - 384kHz | Configured |
| Bit Depths | L16, L24 | Implemented |
| Jitter Buffer | 256 packets, lock-free | Implemented |
| PTP Sync | Stub (local clock fallback) | Partial |
| Transport | Virtual (AudioServerPlugIn) | Working |

## Manager Application

The SwiftUI Manager app provides:

- **Stream List** - View and manage AES67 streams
- **Add Stream** - Configure multicast IP, port, channels, sample rate
- **Channel Mapping** - Visual 128-channel routing grid
- **PTP Diagnostics** - Clock sync status and troubleshooting
- **Quick Start Wizard** - First-run configuration guide
- **Audio Status** - Signal presence indicators

## Why No Kernel Extension Required

This driver uses Apple's modern AudioServerPlugIn architecture:

- Runs entirely in user space within coreaudiod
- No System Integrity Protection (SIP) changes needed
- No "Reduced Security" boot mode required
- Standard file copy installation
- Apple-supported approach for modern macOS

## Current Limitations

### PTP Synchronization
PTP support is currently stubbed - the driver uses local system clock. Full ptpd integration requires fixing vendored header dependencies. Audio will work but won't be network-synchronized.

### Testing Required
- No testing with actual AES67/Dante/RAVENNA equipment
- No DAW integration testing
- Audio flow through the driver unverified

## Help Wanted

This project needs real-world testing. If you have access to:

- AES67 network audio devices
- Dante-enabled equipment
- RAVENNA systems
- Professional audio software (Logic Pro, Pro Tools, etc.)

Please try building and testing! Open GitHub issues with detailed results.

## Contributing

Contributions welcome:

- Hardware testing reports
- Bug fixes with reproduction steps
- PTP integration improvements
- Documentation

### Guidelines

- C++17 standard
- Maintain lock-free audio thread
- Add unit tests for new code

## License

MIT License - See LICENSE file.

### Dependencies

- **libASPL**: MIT License - AudioServerPlugIn framework
- **Boost**: Boost Software License - Lock-free containers

## Acknowledgments

- [libASPL](https://github.com/gavv/libASPL) - Modern C++ AudioServerPlugIn framework
- AES67-2018 specification
- RFC 3550 (RTP), RFC 4566 (SDP), RFC 2974 (SAP)

---

*Build #20 - Driver loads and appears in system, audio flow untested*

*This is experimental software. The driver loads successfully but audio functionality requires real-world testing.*
