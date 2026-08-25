# AES67 macOS Audio Driver

> **EXPERIMENTAL SOFTWARE — USE WITH CAUTION**
>
> This is a work-in-progress open-source AES67 audio driver for macOS. The RX (receive) path has been **verified working with real AES67 hardware** (Riedel Artist intercom system) and audio successfully flows into DAW software (Reaper). The TX (transmit) path has been built but not yet tested with real hardware.
>
> **Production use is not recommended without thorough testing in your environment.** This project is under active development.

A work-in-progress open-source virtual audio driver for macOS that aims to provide AES67 network audio support. Built as a user-space AudioServerPlugIn using the libASPL framework.

## Consuming this repository

Other projects build on this one by adding it as a submodule — the ESP32-P4
firmware in `JaumeAP/DTS-Player` among them:

```bash
git submodule add https://github.com/JaumeAP/aes67_macos_driver.git third_party/aes67
```

Two CMake targets say what is safe to take:

| Target | What it holds | Needs |
|---|---|---|
| `aes67_core` | 23 files: SDP parsing, jitter buffer and packet pool, the PLL and PTP settings, the resampling chain, the channel mapper, configuration | a C++17 compiler, nothing else |
| `aes67_net` | 14 more: the RTP layer, `StreamManager`, PTP slave and master, SAP/RTSP/RTCP discovery | BSD sockets (lwIP provides them on an ESP32) |

Three files are in neither, and they are what a non-Apple consumer has to
replace: `NetworkEngine/RTP/PCMCodec.cpp` (Accelerate),
`NetworkEngine/PTP/AudioClockDeviceList.cpp` and
`NetworkEngine/PTP/CoreAudioClockSource.cpp` (CoreFoundation, CoreAudio).

The split is checked, not asserted: `scripts/ci-local.sh` fails if anything
reachable from `aes67_core` — including through a header two levels down —
includes an Apple framework or a socket header. That check earned its place
immediately: the first version of the list was drawn from the `.cpp` files
alone and got five entries wrong, because `SimpleRTP.h` opens sockets and
`PTPClockSource.h` reaches CoreAudio without either showing up where you would
look for it.

## Current Status

**Verified Working (Real Hardware):**

- **RX Path Tested with Riedel Artist:** Audio successfully received from Riedel Artist intercom system via AES67 multicast and recorded in Reaper
- Multicast interface binding verified on multi-NIC machine (correctly binds to specified interface)
- L24 encoding at 48kHz, 1ms packet time verified working with professional broadcast hardware

**What has been built and passes synthetic tests:**

- The code compiles on Apple Silicon (arm64) with zero warnings
- The driver installs and loads into coreaudiod without crashing
- The device appears as "AES67 Device" in Audio MIDI Setup
- 128 input + 128 output channels are reported to the system
- RTP receiver: joins multicast, decodes L16/L24, writes to ring buffers
- RTP transmitter: reads from ring buffers, encodes L16/L24, sends multicast
- Lock-free SPSC ring buffers bridge network and Core Audio IO threads
- IO handler reads/writes Core Audio buffers in the real-time callback
- Lock-free jitter buffer absorbs network timing variation (configurable depth, 32–4096 slots)
- Stream manager handles RX/TX stream lifecycle, channel mapping, and SDP import/export
- Stream configurations persist across reboots (stored in `/Library/Application Support/AES67Driver/`)
- RT-safe interface boundary prevents accidental mutex access from the audio callback at compile time
- Multicast receiver can bind to a specific network interface (prevents duplicate packets on multi-NIC machines)
- RTP threads are deferred to Core Audio IO lifecycle (zero idle CPU when no client is running)
- PTP slave-only implementation written (IEEE 1588 message exchange, offset/delay calculation, lock detection)
- Test sender/receiver tools exercise the network path over loopback
- 9 test suites pass (SDP parser, channel mapper, ring buffer, RTP receiver, RTP transmitter, PTP clock, stream manager, multi-stream, integration audio path)
- IO handler benchmark exists for real-time performance characterisation
- Doxygen API documentation can be generated via `make docs`
- Flexible configuration: supports interface name ("en0") or IP address, auto-detects if not specified
- Multiple config search paths: environment variable, user-level, and system-wide

**What has NOT been tested:**

- TX path (sending audio to AES67 devices) — code written but not verified with real hardware
- PTP synchronization with any real network clock source — the PTP slave code has been written but never run against a real grandmaster
- The configurable jitter buffer under varied network jitter conditions
- Long-term stability under real workloads
- Multi-device synchronisation
- Sample rates beyond 48kHz in practice
- The Manager app controlling live streams

There is a meaningful gap between "paths exercised with test tools" and "works with real audio." This project has not yet crossed the second threshold.

## Known Limitations

### PTP — Code Written, Not Tested Against Real Hardware
The PTP subsystem has two layers:

- **Media clock recovery (implemented):** `PTPClock` correlates RTP timestamps with local time per AES67-2018 Section 8.2. A Phase-Locked Loop tracks clock drift between the remote source and local audio hardware. Reference point history enables drift ratio calculation for adaptive resampling. In local-clock fallback mode, this is sufficient for single-device operation — audio can flow through the driver using local timing.

- **Network PTP synchronisation (code written, untested):** `PTPSlave` implements IEEE 1588 slave-only mode — Sync/Follow_Up/Delay_Req/Delay_Resp message exchange, offset and path delay calculation, 8-sample moving average filtering, lock detection with hysteresis, and frequency drift estimation. It joins the 224.0.1.129 multicast group on ports 319/320 and feeds measurements into the existing PLL via `PTPDInterface`. However, this code has **never been tested against a real PTP grandmaster**. It auto-falls back to stub mode if PTP ports are unavailable (e.g., without root privileges). Until verified with real hardware, multi-device synchronisation should not be relied upon.

### Audio Path — Exercised Synthetically Only
The RTP receiver/transmitter, jitter buffer, IO handler, and ring buffers have been exercised with test sender/receiver tools over loopback, but never with real audio content or real AES67 network traffic. Codec paths (L16/L24) are covered by unit tests but not verified for audible correctness.

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
│   │   ├── PTPClock         # Media clock recovery (AES67 Section 8.2)
│   │   ├── PTPSlave         # IEEE 1588 slave-only (written, untested)
│   │   ├── PhaseLockedLoop  # Audio clock drift tracking
│   │   ├── PTPDInterface    # PTP interface (stub fallback available)
│   │   └── vendor/ptpd/     # Vendored ptpd source (not used)
│   ├── StreamManager        # RX/TX stream lifecycle, IO-gated start/stop
│   ├── Resampling/          # Sample rate conversion
│   └── Discovery/           # SAP stream discovery (RFC 2974)
├── Shared/                  # Common components
│   ├── RingBuffer.hpp       # Lock-free SPSC ring buffer
│   └── Types.h              # Common data structures
├── Tools/                   # Test utilities
│   ├── AES67TestSender      # Sends RTP test packets over loopback
│   └── AES67TestReceiver    # Receives and validates RTP packets
├── ManagerApp/              # SwiftUI configuration app
└── Tests/                   # Unit & integration tests
```

## Code Specifications

These describe what the code is written to target, not what has been verified with real hardware.

| Feature | Code Target | Status |
|---------|-------------|--------|
| Channels | 128 in/out | Reported to system |
| Sample Rates | 44.1kHz - 384kHz | Declared to HAL, 48kHz verified |
| Bit Depths | L16, L24 | L24 verified with real hardware |
| RTP RX Path | Multicast join, decode, jitter buffer | **Verified with Riedel Artist** |
| RTP TX Path | Encode, multicast send | Exercised with test receiver, not hardware-verified |
| Jitter Buffer | Configurable 32–4096 slots, lock-free | Working in production use, default 256 |
| Multicast Binding | Interface-specific via IP_MULTICAST_IF | **Verified working on multi-NIC** |
| IO Lifecycle | RTP threads start/stop with Core Audio IO | Implemented, verified in DAW |
| RT-Safe Boundary | Compile-time separation of RT/non-RT paths | Implemented |
| Media Clock Recovery | RTP↔time correlation, PLL, drift tracking | Implemented, uses local clock fallback |
| PTP Network Sync | IEEE 1588 slave-only (PTPSlave) | Code written, never tested against real grandmaster |
| Stream Persistence | JSON config in /Library/Application Support/ | Implemented, survives reboot |
| Interface Config | Name ("en0"), IP, or auto-detect | **Implemented** |
| Driver Transport | AudioServerPlugIn | Loads into coreaudiod |

## Building

### Prerequisites

```bash
brew install cmake

# Install libASPL (AudioServerPlugIn framework)
brew tap gavv/gavv
brew install libaspl
```

### Build & Install

```bash
git clone https://github.com/maxajbarlow/AES67_macos_Driver.git
cd AES67_macos_Driver

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j

# Run tests
ctest --output-on-failure

# Generate API docs (requires doxygen)
make docs

# Install the driver
sudo cp -R AES67Driver.driver /Library/Audio/Plug-Ins/HAL/

# Restart Core Audio to load the driver
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod

# Verify it appears
system_profiler SPAudioDataType | grep -A 5 "AES67"
```

### Build Manager App

```bash
cd ManagerApp
./build.sh
open AES67Manager.app
```

## Standards Research

Notes in `Docs/` on what this driver interoperates with, and where it
actually stands against each standard:

| Document | Topic |
|---|---|
| `audio_over_ip_standards_landscape.md` | Which AoIP standards exist alongside AES67 (Dante, RAVENNA, Livewire+, Q-LAN, WheatNet-IP), which are reachable by implementing AES67, and which — AVB/TSN and Milan — are a different family entirely and are not |
| `st2110_30_vs_aes67.md` | SMPTE ST 2110-30 as a constrained subset of AES67: its six conformance levels, the RTP timestamp offset rule, where this driver already meets Level A, and what a configurable transmit packet time would unlock |

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
- Testing PTPSlave against a real IEEE 1588 grandmaster
- Testing multicast interface binding on multi-NIC setups
- DAW compatibility testing (Logic Pro, Pro Tools, Ableton, etc.)

### Guidelines

- C++17 standard
- Maintain lock-free audio thread safety
- Add unit tests for new code

## License

MIT License - See LICENSE file.

### Dependencies

- **libASPL**: MIT License - AudioServerPlugIn framework

## Acknowledgments

- [libASPL](https://github.com/gavv/libASPL) - Modern C++ AudioServerPlugIn framework
- AES67-2018 specification
- RFC 3550 (RTP), RFC 4566 (SDP), RFC 2974 (SAP)

---

*This is experimental software. The driver compiles, loads, and passes synthetic tests, but no real-world audio functionality has been verified.*
