# AES67 macOS Audio Driver

> **UNTESTED EXPERIMENTAL SOFTWARE**
>
> This is a work-in-progress attempt at an open-source AES67 audio driver for macOS. The code compiles and the driver loads into Core Audio, but **no audio has ever been sent or received**. No testing has been performed with real AES67 hardware, DAW software, or network audio of any kind.
>
> **Do not use this for any audio work.** This project exists for development and experimentation only.

A work-in-progress open-source virtual audio driver for macOS that aims to provide AES67/RAVENNA network audio support. Built as a user-space AudioServerPlugIn using the libASPL framework.

## Honest Status

**What actually works:**

- The code compiles on Apple Silicon (arm64)
- The driver installs and loads into coreaudiod without crashing
- The device appears as "AES67 Device" in Audio MIDI Setup
- 128 input + 128 output channels are reported to the system
- The Manager app builds and displays its UI
- Components pass basic synthetic unit tests (isolated, no real audio/network data)

**What has never been tested:**

- Audio flowing through the driver in any direction
- Any application playing or recording audio through the device
- RTP packets being sent to or received from a real network
- PTP synchronization with any clock source (PTP is completely stubbed)
- Compatibility with any AES67, Dante, or RAVENNA hardware
- Compatibility with any DAW (Logic Pro, Pro Tools, Ableton, etc.)
- Latency, stability, glitching, or performance under any real workload
- Sample rates beyond what the system reports as available
- Any channel count beyond what Audio MIDI Setup displays

There is a large gap between "code compiles and driver loads" and "audio works." This project has only crossed the first threshold.

## Known Limitations

### PTP Synchronization — Not Functional
PTP is completely stubbed. The driver uses the local system clock and has no network time synchronization. The stub reports itself as "locked" after 500ms for testing convenience, but this is fake — there is no actual PTP implementation. Multi-device synchronization will not work.

### Audio Path — Completely Unverified
No audio has ever passed through this driver. The I/O handler, RTP receiver, RTP transmitter, jitter buffer, and ring buffer have code written but have never been exercised with real audio data.

### Manager App — UI Only
The SwiftUI Manager app renders its interface but has not been tested controlling actual streams. The UI includes screens for stream management, channel mapping, and PTP diagnostics, but whether these function beyond displaying placeholder data is unknown.

## Architecture

```
AES67Driver/
├── Driver/                  # AudioServerPlugIn (libASPL)
│   ├── AES67Device          # Core Audio device declaration
│   ├── AES67IOHandler       # Audio I/O callbacks (lock-free design)
│   ├── PlugInMain           # AudioServerPlugIn entry point
│   └── SDPParser            # SDP file parser (RFC 4566)
├── NetworkEngine/           # Network audio code
│   ├── RTP/
│   │   ├── SimpleRTP        # RTP socket layer (RFC 3550)
│   │   ├── RTPReceiver      # Packet receive + decode
│   │   ├── RTPTransmitter   # Packet encode + send
│   │   └── LockFreeCircularJitterBuffer
│   ├── PTP/
│   │   ├── PTPClock         # Clock interface
│   │   └── PTPDInterface    # PTP daemon interface (STUB — not functional)
│   ├── Resampling/          # Sample rate conversion
│   └── Discovery/           # SAP stream discovery (RFC 2974)
├── Shared/                  # Common components
│   ├── RingBuffer.hpp       # Lock-free SPSC ring buffer
│   └── Types.h              # Common data structures
├── ManagerApp/              # SwiftUI configuration app
└── Tests/                   # Unit tests (synthetic/isolated)
```

## Code Specifications

These describe what the code is written to target, not what has been verified to work.

| Feature | Code Target | Verified? |
|---------|-------------|-----------|
| Channels | 128 in/out | Reported to system only |
| Sample Rates | 44.1kHz - 384kHz | Declared to HAL, never tested |
| Bit Depths | L16, L24 | Code exists, never tested with audio |
| Jitter Buffer | 256 packets, lock-free | Code exists, never received real packets |
| PTP Sync | Stubbed (local clock) | Not functional |
| Driver Transport | AudioServerPlugIn | Loads into coreaudiod |

## Building

### Prerequisites

```bash
brew install cmake boost

# Install libASPL (AudioServerPlugIn framework)
brew tap gavv/gavv
brew install libaspl
```

### Build & Install

```bash
git clone https://github.com/maxajbarlow/AES67_macos_Driver.git
cd AES67_macos_Driver

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
make -j

# Install the driver
sudo cp -R AES67Driver.driver /Library/Audio/Plug-Ins/HAL/

# Restart Core Audio to load the driver
sudo killall coreaudiod

# Verify it appears
system_profiler SPAudioDataType | grep -A 5 "AES67"
```

### Build Manager App

```bash
cd ManagerApp
./build.sh
open AES67Manager.app
```

## Why No Kernel Extension Required

This driver uses Apple's AudioServerPlugIn architecture:

- Runs entirely in user space within coreaudiod
- No SIP changes or "Reduced Security" boot mode required
- Standard file copy installation
- Apple-supported approach for modern macOS

## Help Wanted

This project needs real-world testing before any audio claims can be made. If you have access to:

- AES67 network audio devices
- Dante-enabled equipment
- RAVENNA systems
- Professional audio software (Logic Pro, Pro Tools, etc.)

Please try building and testing. Open GitHub issues with detailed results — even "it didn't work" reports are valuable.

## Contributing

Contributions welcome, especially:

- **Hardware testing reports** (most needed)
- Bug fixes with reproduction steps
- PTP integration (replacing the current stub with real ptpd)

### Guidelines

- C++17 standard
- Maintain lock-free audio thread safety
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

*This is untested experimental software. The driver compiles and loads but no audio functionality has been verified.*
