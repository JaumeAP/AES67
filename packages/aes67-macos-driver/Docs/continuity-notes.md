# Continuity notes

## Evergreen state

Active branch: none. Every session's branch is merged into `main` with `--no-ff` and deleted at close; `main` is clean.

Configuration: CLAUDE.md in repo; workflow skills at user scope as the `session-rules@jaumeap` plugin (repo `JaumeAP/claude-plugins`), not vendored here. `.claude/` no longer carries skills or settings.

Git workflow: autonomous local merge (no pull request) per git-rules. Response style: Catalan replies, English code/commits. Start replies with `Rebut: <order>`. Numbered lists only, no bold/headers/tables.

Architecture invariants: RT-safe thread boundaries (IO thread touches only `RTSafeStreamInterface` and the lock-free ring buffers, never `StreamManager`), AudioServerPlugIn via libASPL. Stream configs persist as JSON.

Platform-free core: `packages/aes67-core`, a sibling package in this monorepo, consumed with `add_subdirectory` — SDP, RTP wire header, jitter buffer, packet pool, PLL, resampling, channel mapping, configuration, the ring buffer, the RT-safe view over it, the PTP time types and clock-source interface, and the PTP peer, RTCP receiver and Dolby model tables. What stays here is macOS plus `aes67_net`, the socket layer, which supplies the four `NetworkUtils` symbols the core declares without implementing. The boundary was reviewed on 2026-09-03 and deliberately left where it is. Its test suites run from the root `ctest` since 2026-09-05; before that they were forced off here and had no other gate. Clone with `--recurse-submodules`: doctest, at the root `external/doctest`, is the one submodule left.

Build/test: macOS (Apple Silicon + x86_64), CMake out-of-source. Suites are doctest, labelled `unit`/`timing`/`network`/`integration`/`interop`, each with `TIMEOUT 60` (`PTPLoopback` 90). `IntegrationAudioPath` needs real multicast and fails outside the gate by design. The driver target no longer recompiles the portable translation units: it links `aes67_net`, which brings `aes67_core` with it.

**This repository cannot be configured off a Mac**: `project()` declares OBJCXX, so `cmake` fails on Linux before anything builds. A remote session can still check a change by compiling and linking the affected targets by hand (`g++ -std=c++20 -I. -I../aes67-core`), with a local shim for `net/if_dl.h` and `CommonCrypto/CommonDigest.h` and a stub for the CoreAudio-bound symbols (`PTPClockManager`, `AudioThreadPriority`, `MDNSBrowser`) — neither committed. That is how the 2026-09-04 work was verified: every portable suite built against `aes67_core` and `aes67_net` exactly as `Tests/CMakeLists.txt` links them, and run.

CI: local only, GitHub Actions disabled and deleted. `scripts/gate.sh` is the gate, run by `.githooks/pre-push` (opt in per clone with `git config core.hooksPath .githooks`). It now builds everything, tools and examples included. Two speeds: build, `ctest -LE timing` and the core's platform contract by default; clang-tidy only with `--analyse` or `AES67_ANALYSE=1`. `--sanitize` and `--tsan` run everything but `network`. `scripts/coverage.sh` reports llvm-cov.

Static analysis: `.clang-tidy` runs the defect families and leaves style off; `WarningsAsErrors` is deliberately narrower than `Checks`. Install with `python3 -m venv ~/.local/venvs/cpptools && ~/.local/venvs/cpptools/bin/pip install clang-tidy==21.1.6`.

Manager app previews: `#Preview` blocks live in `ManagerApp/Views/Previews/`, kept out of `build.sh`'s source list because the macro needs full Xcode. New previews go there, and into the Xcode target, never into `build.sh`.

Related repositories, and where they stand: `aes67-core` and `t41-ptp` are packages of this monorepo now, not separate repositories -- their GitHub originals are archived and read-only, and so is `JaumeAP/QNEthernet`, which arrives inside t41-ptp as `libraries/QNEthernet` and had a duplicate top-level package here until 2026-09-05. `JaumeAP/AES67-master-box-` (the grandmaster firmware) is the exception: it was a package here and is standalone again, `main` at 2f66e20, carrying t41-ptp as the `lib/t41-ptp` submodule at `bc9cd69`. The box does NOT consume `aes67-core`, and after weighing it on 2026-09-03 it stays that way.

Profiles: `packages/aes67-profiles` holds the compatibility profiles, the Dolby model catalog and the PTP profiles as dependency-free tables read by this core and by the Teensy firmware. A profile is a filter, never a capability grant. The flow limit is bytes, not channels: `aes67-core/NetworkEngine/RTP/PacketBudget.h` (1500-byte frame, 1472 RTP bytes) decides what fits at a packet time, `StreamManager::canAddStream()` refuses what does not in both directions, and the transmit splitter takes min(profile cap, `StreamChannelMapper::kMaxChannelsPerFlow` = 64, budget at the device rate and 1 ms). RAVENNA's profile permits 64 channels a flow; AES67, Dante, Dolby and ST 2110-30 keep 8.

Interop simulations: `Tools/AES67InteropSim` (the RAVENNA Linux daemon's wire formats) and `Tools/DanteInteropSim` (Dante Controller 4.18.1.1's SAP/SDP handling, both directions, every profile). Both build in the gate; run them after touching SDP generation, SAP or the profiles. The Eines repository carries `packages/dante-device-emulator`, a Dante device in AES67 mode for live tests, and `packages/aoip-stress-lab`.

## Session summary — Merging and Dante, virtually (2026-09-07)

The RAVENNA profile was aligned with the installed Merging plug-in (x86_64 only, so analysed rather than run): 352.8/384 kHz, DSCP 46, domain 0, and 64 channels a flow, which forced the byte-budget design above. Dante Controller 4.18.1.1 was taken apart (Java classes plus `libDanteController.dylib`) and found three defects here: the announced `o=` line had its type fields swapped and no address; `a=ts-refclk` wrote RFC 7273's `domain-nmbr=0`, which Dante's `Integer.parseInt` refuses, and now writes the bare `:0`; `SAPListener` kept the optional `application/sdp` payload type. Both simulations report THEY CONNECT. The whole monorepo gate ran on the Mac for every push.

## Open items

1. Dante live test needs a Dante device, or the Eines emulator on another host and this driver installed: whether Dante Controller lists a SAP announcement without the MIME type, what it does with a flow that has no `ts-refclk`, DSCP CS7 on PTP (`aes67ptpd --dscp 56`). Reception of a real 64-channel 125 us RAVENNA stream from Merging gear is untested too. The driver is not installed on this Mac.
2. `ManagerApp` builds and is signed, but remains unverified against a live driver, per README.
3. Still at zero coverage, and not by oversight: `AES67Device`, `AES67IOHandler`, `PTPArbitrator` and the CoreAudio clock sources. Each needs CoreAudio or libASPL, so a suite for them cannot be run from a remote session; writing one is a job for a session on the Mac. `BenchmarkIOHandler` exercises the IO handler but is not a CTest.
4. The `t41-ptp` servo split has not been through the Teensy toolchain. `pio run` on the box before flashing anything.
5. `TestPTPMasterBoxInterop` replays bytes built from the library's source. A real capture off the box would be worth more and drops into the same shape.
6. `NetworkErrorHandler::attemptRecovery` in the core clears its own latch before returning, so `isInRecovery()` is false wherever a caller can observe it and a second recovery always starts. Pinned by a test that documents the behaviour; making the latch real is a behaviour change.
7. The firmware PR `JaumeAP/DTS-Player#33` needs `pio test -e native` before merging, and a decision on `-DDTS_DSP_QMF_FLOAT`.
8. `dts-dsp` needs `COPYING.LESSER`, the LGPL-2.1 text, which nobody has downloaded yet.
9. `PTPMasterSettings` keeps the two message intervals in milliseconds, and milliseconds cannot express every legal rate: 16 Sync per second is 62.5 ms and the field is an `int`. The half that mattered is fixed — both rates come from one exponent settled in the constructor, and a non-power-of-two setting is rounded to the nearest legal interval and sent at that rate rather than misdeclared. What is left is where the rounding happens. Storing `int8_t logSyncInterval` and `int8_t logAnnounceInterval` in the settings, with milliseconds as a rendering for `ManagerApp`, would put it where the value is chosen and shown instead of silently at the port. That is a settings file format change — a version bump plus a read path mapping an old `syncIntervalMs` to the nearest exponent — and the struct lives in `aes67-core`, not here. `JaumeAP/AES67-master-box-` is already built that way (`src/profiles.h`).
