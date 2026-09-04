# AES67 macOS Audio Driver

> **EXPERIMENTAL SOFTWARE — USE WITH CAUTION**
>
> This is a work-in-progress open-source AES67 audio driver for macOS. The RX (receive) path has been **verified working with real AES67 hardware** (Riedel Artist intercom system) and audio successfully flows into DAW software (Reaper). The TX (transmit) path has been built but not yet tested with real hardware.
>
> **Production use is not recommended without thorough testing in your environment.** This project is under active development.

A work-in-progress open-source virtual audio driver for macOS that aims to provide AES67 network audio support. Built as a user-space AudioServerPlugIn using the libASPL framework.

## Where the portable code went

The platform-free core is no longer here. It lives in
[`JaumeAP/aes67-core`](https://github.com/JaumeAP/aes67-core) and arrives as the
`external/aes67-core` submodule: SDP parsing, the RTP wire header, the jitter
buffer and packet pool, the media-clock PLL, the resampling chain, channel
mapping, compatibility profiles and stream configuration.

It moved out because while it lived here, every other implementation that
wanted a jitter buffer had to consume a macOS driver. The ESP32-P4 firmware, a
Linux daemon and this driver are peers.

What stays here is what is genuinely macOS — the `AudioServerPlugIn`, libASPL,
the CoreAudio clock sources, the Accelerate codec — plus `aes67_net`, the socket
layer, which is portable in practice but is not the core's business and which
supplies the three `NetworkUtils` symbols the core declares without
implementing.

Clone with `--recurse-submodules`, or run `git submodule update --init
--recursive` afterwards. `scripts/ci-local.sh` runs the core's own contract
check, so a platform header finding its way in fails here too.

### What consuming it costs

This repository is GPL-3.0, and that stays. A project that links `aes67_core`
or `aes67_net` and is then **distributed** — firmware on a board handed to
someone, a binary shipped — has to be GPL-3.0 too, sources included. Used
in-house, on your own machines and boards, the licence obliges nothing.

So the split above is an offer of code, not an offer of terms. If a consumer
cannot be GPL, the options are the usual ones: reimplement the part it needs,
or keep it in-house. `dts-dsp`, the DTS decoding library, is LGPL-2.1 instead
and is the easier dependency for a closed consumer — linking is what that
licence exists to permit.

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
- Discovery both ways: SAP in and out, RTSP DESCRIBE as client and server, DNS-SD browsing
- NMOS IS-04 registration and IS-05 connection management, against loopback registries and controllers
- Test sender/receiver tools exercise the network path over loopback
- 26 test suites pass here, plus the platform-free core's own (see `Tests/CMakeLists.txt` for the current list)
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
- NMOS against a commercial registry or controller — only against our own loopback fixtures

There is a meaningful gap between "paths exercised with test tools" and "works with real audio." This project has not yet crossed the second threshold.

## Known Limitations

### PTP — Code Written, Not Tested Against Real Hardware
The PTP subsystem has two layers:

- **Media clock recovery (implemented):** `PTPClock` correlates RTP timestamps with local time per AES67-2018 Section 8.2. A Phase-Locked Loop tracks clock drift between the remote source and local audio hardware. Reference point history enables drift ratio calculation for adaptive resampling. In local-clock fallback mode, this is sufficient for single-device operation — audio can flow through the driver using local timing.

- **Network PTP synchronisation (code written, untested):** `PTPSlave` implements IEEE 1588 slave-only mode — Sync/Follow_Up/Delay_Req/Delay_Resp message exchange, offset and path delay calculation, 8-sample moving average filtering, lock detection with hysteresis, and frequency drift estimation. It joins the 224.0.1.129 multicast group on ports 319/320 and feeds measurements into the existing PLL via `PTPDInterface`. However, this code has **never been tested against a real PTP grandmaster**. It auto-falls back to stub mode if the PTP ports cannot be opened. (An earlier note here said that needed root: it does not. On macOS 26.6.2 an unprivileged process binds UDP 319 and 320 without trouble — measured, along with 80 and 443.) Until verified with real hardware, multi-device synchronisation should not be relied upon.

  Interoperability with a foreign grandmaster rests on the intervals being the master's to announce (IEEE 1588-2008 §7.7.2.4, §9.5.11.2): `PTPSlave` follows the `logMinDelayReqInterval` carried in Delay_Resp and the `logAnnounceInterval` carried in Announce, falling back to its configured values until a master advertises something usable and refusing anything outside 1/32 s to 32 s (`followAdvertisedIntervals`, `minLogInterval`/`maxLogInterval`). It also checks `majorSdoId`, the top nibble of octet 0, so a gPTP master on the same domain is no longer followed as if it belonged to this profile (`enforceMajorSdoId`, default on). `PTPMaster` in turn advertises the intervals it actually sends, instead of the hard-coded 2^0 = 1 s it wrote into every message while sending Sync eight times a second.

  Both delay mechanisms are implemented. End to end (Delay_Req/Delay_Resp with the master) stays the default, which is what an AES67 grandmaster expects; `PTPSlaveConfig::delayMechanism = DelayMechanism::PeerToPeer` switches to peer delay (IEEE 1588-2008 §11.4), where the port joins 224.0.0.107, measures the link to its neighbour with `Pdelay_Req`/`Pdelay_Resp`/`Pdelay_Resp_Follow_Up` and feeds that link delay into the same offset arithmetic. It also answers a neighbour's `Pdelay_Req` (`respondToPdelayReq`), since a peer-to-peer port that stays silent leaves the other end unable to measure anything, and it measures the link whether or not a grandmaster has been chosen.

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
│   ├── StreamManager        # RX/TX stream lifecycle, IO-gated start/stop
│   ├── Resampling/          # Sample rate conversion
│   └── Discovery/           # Finding streams, and being found
│       ├── SAPListener      # SAP announcements in (RFC 2974)
│       ├── SAPAnnouncer     # our own transmit streams announced
│       ├── RTSPClient       # DESCRIBE against a device that publishes one
│       ├── RTSPServer       # DESCRIBE for our own streams
│       ├── SDPFetcher       # an SDP from a file, http:// or rtsp:// URL
│       ├── MDNSBrowser      # DNS-SD browsing over the system responder
│       ├── NMOSRegistrationClient  # IS-04 node/device/sender/receiver
│       ├── ConnectionAPIServer     # IS-05 connection management
│       └── RTCPMonitor      # sender reports from the streams we receive
├── Shared/                  # Common components
│   │                        # (RingBuffer.hpp and the rest of the portable
│   │                        #  pieces live in external/aes67-core)
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

### NMOS (IS-04 / IS-05)

The driver registers itself with an NMOS registry when it finds one, and
accepts connection management over IS-05.

- **IS-04 registration** (`NMOSRegistrationClient`): the registry is found by
  DNS-SD (`_nmos-register._tcp`) or configured by hand; the driver then
  registers a node, a device, one sender per transmit stream and one receiver
  per receive stream, and keeps the registration alive with the heartbeat the
  specification asks for. Resource ids are derived, not random, so a restart
  re-registers the same node rather than a second one.
- **IS-05 connection management** (`ConnectionAPIServer`): a small HTTP
  endpoint serving `/x-nmos/connection/v1.1/`. Controllers read
  `constraints`, `staged` and `active` for each sender and receiver, read a
  sender's `transportfile` (the SDP), and PATCH a receiver's `staged` to
  re-point it — by transport parameters, or by handing over an SDP. An
  activation is applied immediately; nothing is staged for later, and `active`
  is read-only.

Both are exercised by `TestNMOSRegistration` and `TestConnectionAPI` against a
registry and a controller made of loopback sockets. Neither has been tested
against a commercial NMOS registry or controller.

The endpoint has no authentication — IS-05 does not define one at this level —
so it is only as safe as the network it is on. It answers cross-origin reads
but not cross-origin activations, and a receiver only follows an unsolicited
SAP announcement when the announcement comes from the host the stream is
already bound to.

### The PTP daemon (`aes67ptpd`)

`Daemon/aes67ptpd.cpp` builds a small daemon that runs one PTP engine for the
host and publishes offset, path delay and lock state on a Unix-domain socket
(`/var/run/aes67ptpd.sock`, wire format in `Shared/PTPServiceProtocol.h`).
When that socket exists, `PTPDInterface` reads it instead of starting a second
PTP engine inside coreaudiod; when it does not, the in-process path runs
exactly as before, and `setPreferPrivilegedDaemon(false)` refuses the daemon
outright.

It is a LaunchDaemon for lifecycle, not for privilege: one engine per host
rather than one per process, alive across coreaudiod restarts and plugin
reloads, and readable by the Manager app at the same time. A reader stops
believing a status older than two seconds, so a daemon that dies cannot leave
a stale offset looking like a lock.

Started as root it drops to `nobody` as soon as the status socket and the PTP
sockets exist, so the part that runs for the life of the machine — parsing
packets off an unauthenticated multicast group — never runs privileged.
Started unprivileged it stays that way, and needs a `--socket` path it can
write.

```bash
cmake --build build --target aes67ptpd
sudo ./build/aes67ptpd --interface en0 --domain 0 --verbose
```

The installer places it at `/usr/local/libexec/aes67ptpd` with
`Installer/com.aes67driver.ptpd.plist`, and `Installer/uninstall.sh` removes
both.

### Validate the installed driver

`scripts/validate-hal.sh` checks the plugin the way Core Audio itself sees it,
never through this source tree: the installed bundle in
`/Library/Audio/Plug-Ins/HAL` and its signature, whether `coreaudiod` is
running, what `system_profiler` reports, the last ten minutes of `coreaudiod`
log lines mentioning the plugin, and then `Tools/HALValidate`, which drives
the device through Apple's HAL client API -- the same
`AudioObjectGetPropertyData` / `AudioDeviceStart` path HALLab uses.

```bash
cmake -S . -B build -DBUILD_TOOLS=ON
cmake --build build --target HALValidate
scripts/validate-hal.sh
```

`HALValidate` checks the property contract (name, manufacturer, UID, model
UID, transport type, alive, clock domain), the timing properties (latency and
safety offset per scope, buffer frame size inside its advertised range), the
streams (count, channel configuration, virtual and physical formats,
available formats), sample-rate negotiation (every advertised rate is set and
read back, then the original restored), and a live IOProc run (callback rate
against the buffer size, monotonic sample time, device clock against the wall
clock). Exit status is non-zero if any check failed. It takes `--list`,
`--uid`, `--name`, `--id`, `--seconds`, `--skip-io`, `--skip-rates` and
`--force-io`.

Opening a device that has input streams goes through TCC, and from a terminal
without microphone access the open blocks instead of prompting -- so the IO
section is skipped with an explanation unless that access is granted in
System Settings > Privacy & Security > Microphone.

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

*This is experimental software. The RX path has been verified with real AES67 hardware (see Current Status); everything else — TX, network PTP, the Manager app — compiles, loads and passes synthetic tests only.*
