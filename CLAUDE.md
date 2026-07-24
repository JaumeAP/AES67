# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A user-space AES67 (AES67-2018) network audio driver for macOS, implemented as a Core Audio `AudioServerPlugIn` using the [libASPL](https://github.com/gavv/libASPL) framework — no kernel extension. Companion pieces: a SwiftUI menu-bar Manager app, a `.pkg` installer, and CLI test tools for exercising the RTP path over loopback.

**Status matters here.** The RX path is verified with real AES67 hardware; TX, network PTP, and the Manager app are unverified. README.md's "Current Status" / "Known Limitations" sections are the source of truth — don't upgrade a feature's claimed status in docs or comments unless you've actually verified it (real hardware or, at minimum, a passing new test that exercises it).

## Build & test commands

Primary target is macOS (Apple Silicon + x86_64); the CMake config gates driver/Manager-app targets on `APPLE` but tests/tools build cross-platform.

```bash
# Prerequisites (macOS)
brew install cmake
brew tap gavv/gavv && brew install libaspl

# Configure + build everything (driver, tests, tools, examples, Manager app)
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j

# Run the full test suite
ctest --output-on-failure

# Run a single test suite
ctest -R StreamManager --output-on-failure
# or run the binary directly, e.g.:
./Tests/TestStreamManager

# Doxygen API docs (requires `brew install doxygen`)
make docs

# Install the driver, then reload coreaudiod to pick it up
sudo cp -R AES67Driver.driver /Library/Audio/Plug-Ins/HAL/
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
system_profiler SPAudioDataType | grep -A 5 "AES67"   # verify it loaded
```

Build options (pass as `-DOPTION=OFF` to skip): `BUILD_TESTS`, `BUILD_EXAMPLES`, `BUILD_TOOLS` — all `ON` by default.

Manager app can be built standalone: `cd ManagerApp && ./build.sh` (add `--force` to skip its up-to-date check; it does a raw `swiftc` compile, not SwiftPM, though `Package.swift` exists for editor/IDE support).

CTest names map 1:1 to `Tests/*.cpp` (`SDPParser`, `ChannelMapper`, `RingBuffer`, `RTPReceiver`, `RTPTransmitter`, `PTPClock`, `StreamManager`, `MultiStream`, `IntegrationAudioPath`). `BenchmarkIOHandler` is built but not registered as a CTest — run it directly for RT performance characterisation.

Ignore the root-level `Makefile`, `CTestTestfile.cmake`, `CPackConfig.cmake`/`CPackSourceConfig.cmake` — these are stale CMake-generated artifacts from a prior in-source build (see the absolute `/Users/maxbarlow/...` paths inside `Makefile`), accidentally committed. Always build out-of-source in a `build/` directory as shown above; don't edit or rely on those root files.

## Architecture

```
Driver/          AudioServerPlugIn (libASPL): device declaration, IO callbacks, SDP parsing
NetworkEngine/   RTP, PTP, stream lifecycle, resampling, SAP/RTSP discovery
Shared/          Cross-cutting: lock-free ring buffer, types, config, logging, error recovery
Tools/           CLI sender/receiver for exercising the RTP path over loopback (no hardware needed)
ManagerApp/      SwiftUI menu-bar app; talks to the driver via DriverManager.cpp (Core Audio APIs)
Tests/           One CMake target + CTest entry per subsystem, plus multi-stream/full-path integration tests
```

### Data flow and thread boundaries

This is the load-bearing concept in the codebase: **three thread domains connected only through lock-free structures.**

1. **Core Audio IO thread** (real-time, deadline-bound, owned by `coreaudiod`) — runs `AES67IOHandler`, called by libASPL's `Device::onReadClientInput`/`onWriteMixedOutput` per audio cycle.
2. **Network threads** (per-stream RTP receive/transmit loops, owned by `RTPReceiver`/`RTPTransmitter`) — socket recv/send, codec decode/encode, jitter buffer management.
3. **Control thread(s)** (init, Manager app IPC, SAP/RTSP discovery) — owns `StreamManager`, may block, lock, allocate.

The only thing the IO thread is allowed to touch is `NetworkEngine/RTSafeStreamInterface.h`: a non-owning view over per-channel `SPSCRingBuffer<float>` (one producer, one consumer, `Shared/RingBuffer.hpp`) plus atomic counters/flags. It is deliberately banned from holding a reference to `StreamManager` and every method is `noexcept`, lock-free, non-blocking — this is enforced by construction, not convention, so preserve that shape when touching IO-thread code: **never add a mutex, allocation, or `StreamManager` pointer reachable from `AES67IOHandler`.**

`StreamManager` (`NetworkEngine/StreamManager.h`) is the non-RT coordinator: owns all RX/TX stream lifecycles, channel mapping (`StreamChannelMapper`), config persistence (`StreamConfig`), and PTP clock manager. Every public method takes a mutex — it must never be called from the IO thread. `setIOActive(bool)` is the bridge between domains: Core Audio's `StartIO`/`StopIO` toggles it, and `StreamManager` starts/stops the dormant RTP receiver/transmitter threads accordingly (RTP threads have zero idle CPU when no client is running).

Audio channel buffers (`DeviceChannelBuffers = std::array<SPSCRingBuffer<float>, 128>`) are owned by `AES67Device` and referenced by both `RTSafeStreamInterface` (IO thread) and `StreamManager` (network threads write/read the same buffers from their own non-RT side). 128 channels in, 128 out, fixed.

### PTP has two independent layers — don't conflate them

- **`PTPClock`** (media clock recovery, AES67-2018 §8.2): correlates RTP timestamps against local time via a PLL (`PhaseLockedLoop`) to track drift between a remote source and local hardware clock. This is implemented and usable today via local-clock fallback — sufficient for single-device operation.
- **`PTPSlave`** (network IEEE 1588 slave-only sync): full Sync/Follow_Up/Delay_Req/Delay_Resp exchange on 224.0.1.129:319/320, feeding measurements into the same PLL via `PTPDInterface`. Code is complete but **has never been run against a real grandmaster**; it auto-falls back to stub mode without root (can't bind privileged multicast ports).
- `NetworkEngine/PTP/vendor/ptpd/` is a vendored ptpd C implementation that is **explicitly unused** — real PTP sync is native C++17 in `PTPSlave.cpp`. Don't wire it in or treat it as live code.

### Dead code to be aware of (not in the CMake build)

`NetworkEngine/RTP/` contains several jitter-buffer/pool implementations not referenced by any `CMakeLists.txt` target: `CircularJitterBuffer`, `JitterBuffer`, `TemporalJitterBuffer`, `LockFreePriorityQueue`, `LockFreeRingBuffer`, `RTPPacketPool`, `SimplifiedLockFreePacketPool`. The active implementations are `LockFreeCircularJitterBuffer` and `LockFreePacketPool` (both are in `SHARED_SOURCES`/test sources). If you touch jitter-buffer or packet-pool logic, confirm which file the target you're building actually compiles before assuming a change takes effect — check `CMakeLists.txt` / `Tests/CMakeLists.txt` source lists, not just file presence.

### Configuration & persistence

- Stream configs persist as JSON, search order in `StreamConfig.cpp`: `$AES67_CONFIG_PATH` env var → `~/Library/Application Support/AES67Driver/streams.json` → `/Library/Application Support/AES67Driver/streams.json` (system-wide default).
- Network interface (`Config`/`StreamConfig`) accepts an interface name (`"en0"`), a literal IP, or auto-detects if unset — see `NetworkEngine/NetworkInterfaceDetection.cpp`. Multicast joins bind to this interface explicitly (`IP_MULTICAST_IF`) to avoid duplicate packets on multi-NIC machines — preserve that behavior in any RTP socket changes.
- Runtime debug logging for the driver goes to `/tmp/aes67driver_debug.log` via `Driver/DebugLog.h` (`AES67_LOG`/`AES67_LOGF`); `Shared/NonBlockingLogger` is the RT-safe logger for use from audio-adjacent paths.

### Manager app

SwiftUI app in `ManagerApp/`; `Models/DriverManager.swift` wraps `DriverManager.cpp` (a small C++ shim over Core Audio HAL APIs) to talk to the installed driver. Build with `ManagerApp/build.sh` (plain `swiftc`, not SwiftPM, despite `Package.swift` existing). Its functional status against a live driver is unverified — treat UI claims skeptically per README.

## Conventions

- C++17 throughout the native code (`CMAKE_CXX_STANDARD 17`, enforced). Warnings are `-Wall -Wextra -Wpedantic` with unused-parameter and missing-field-initializer silenced; keep new code warning-clean under those flags.
- Everything native lives in the `AES67` namespace.
- Header/impl pairs use `.h`/`.cpp` except `Shared/RingBuffer.hpp` (header-only) and `Shared/Config.hpp`.
- Preserve the RT-safety boundary described above in any change touching `Driver/AES67IOHandler.*` or `NetworkEngine/RTSafeStreamInterface.h` — this is the one architectural invariant the codebase is built around, and it's checked by convention/review, not by a static analyzer.
- When adding new functionality to `NetworkEngine`/`Driver`, add a corresponding CTest target in `Tests/CMakeLists.txt` following the existing per-subsystem pattern (own `add_executable` compiling only the sources it needs, plus `add_test`).
