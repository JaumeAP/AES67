# Continuity notes

## Evergreen state

Active branch: `claude/auditoria-m5jumq`, merged into `main` at session close.

Configuration: CLAUDE.md in repo; workflow skills at user scope as the `session-rules@jaumeap` plugin (repo `JaumeAP/claude-plugins`), not vendored here. `.claude/` no longer carries skills or settings.

Git workflow: autonomous local merge (no pull request) per git-rules. Response style: Catalan replies, English code/commits. Start replies with `Rebut: <order>`. Numbered lists only, no bold/headers/tables.

Architecture invariants: RT-safe thread boundaries (IO thread touches only `RTSafeStreamInterface` and the lock-free ring buffers, never `StreamManager`), AudioServerPlugIn via libASPL. Stream configs persist as JSON.

Platform-free core: `packages/aes67-core`, a sibling package in this monorepo, consumed with `add_subdirectory` — SDP, RTP wire header, jitter buffer, packet pool, PLL, resampling, channel mapping, configuration, the ring buffer, the RT-safe view over it, the PTP time types and clock-source interface, and the PTP peer, RTCP receiver and Dolby model tables. What stays here is macOS plus `aes67_net`, the socket layer, which supplies the four `NetworkUtils` symbols the core declares without implementing. The boundary was reviewed on 2026-09-03 and deliberately left where it is. Its test suites run from the root `ctest` since 2026-09-05; before that they were forced off here and had no other gate. Clone with `--recurse-submodules`: doctest, at the root `external/doctest`, is the one submodule left.

Build/test: macOS (Apple Silicon + x86_64), CMake out-of-source. Suites are doctest, labelled `unit`/`timing`/`network`/`integration`/`interop`, each with `TIMEOUT 60` (`PTPLoopback` 90). `IntegrationAudioPath` needs real multicast and fails outside the gate by design. The driver target no longer recompiles the portable translation units: it links `aes67_net`, which brings `aes67_core` with it.

**This repository cannot be configured off a Mac**: `project()` declares OBJCXX, so `cmake` fails on Linux before anything builds. A remote session can still check a change by compiling and linking the affected targets by hand (`g++ -std=c++17 -I. -I../aes67-core`), with a local shim for `net/if_dl.h` and `CommonCrypto/CommonDigest.h` and a stub for the CoreAudio-bound symbols (`PTPClockManager`, `AudioThreadPriority`, `MDNSBrowser`) — neither committed. That is how the 2026-09-04 work was verified: every portable suite built against `aes67_core` and `aes67_net` exactly as `Tests/CMakeLists.txt` links them, and run.

CI: local only, GitHub Actions disabled and deleted. `scripts/ci-local.sh` is the gate, run by `.githooks/pre-push` (opt in per clone with `git config core.hooksPath .githooks`). It now builds everything, tools and examples included. Two speeds: build, `ctest -LE timing` and the core's platform contract by default; clang-tidy only with `--analyse` or `AES67_ANALYSE=1`. `--sanitize` and `--tsan` run everything but `network`. `scripts/coverage.sh` reports llvm-cov.

Static analysis: `.clang-tidy` runs the defect families and leaves style off; `WarningsAsErrors` is deliberately narrower than `Checks`. Install with `python3 -m venv ~/.local/venvs/cpptools && ~/.local/venvs/cpptools/bin/pip install clang-tidy==21.1.6`.

Manager app previews: `#Preview` blocks live in `ManagerApp/Views/Previews/`, kept out of `build.sh`'s source list because the macro needs full Xcode. New previews go there, and into the Xcode target, never into `build.sh`.

Related repositories, and where they stand: `JaumeAP/aes67-core` `main` at 36c8b47; `JaumeAP/t41-ptp` (the Teensy PTP library) `integration/master-box` at b8b723f; `JaumeAP/AES67-master-box-` (the grandmaster firmware) `main` at c4b6593, carrying t41-ptp as `lib/t41-ptp`. The box does NOT consume `aes67-core`, and after weighing it on 2026-09-03 it stays that way.

## Session summary — Full-repo audit, and closing it (2026-09-04)

A full audit of this repository produced 23 findings; 22 are fixed and the 23rd is closed for everything that can be tested off a Mac.

The security half: auto sink-follow matched a SAP announcement on session name alone (its source-address guard could never fire, because no SDP parse fills that field), so any host could re-point a live receiver — it now needs the SDP origin, known on both sides, and `SAPListener` refuses an announcement whose origin disagrees with its sender. The IS-05 server got a receive timeout, a `select`-based accept loop, looped writes, cross-origin reads but not activations, and JSON fields read from the object they belong to. Six `select()` loops had three different reactions to a negative return; `NetworkEngine/SelectWait.h` gives them one. `aes67ptpd` drops to `nobody` once its sockets exist.

The correctness half: `StreamManager` no longer raises callbacks or writes its config under `streamsMutex_`; RTP framing skips the CSRC list and extension header and strips padding, as `RTPSocket::parseFrame`; two `std::stoi`/`std::stoul` throw paths inside coreaudiod are bounded.

Coverage went from 18 suites to 28. Ten components that had none now have one, four of them through a new bytes-in/state-out seam. Two more defects fell out of writing them: `isValidMulticastAddress` accepted the classful shorthands, and `RTSPClient::parseResponse` padded a short body with NULs.

## Open items

1. **Nothing from 2026-09-03 or 2026-09-04 has been through the macOS gate.** Run `scripts/ci-local.sh` on the Mac before trusting any of it. Two changes there are structural and unverified off a Mac: the driver now links `aes67_net` instead of recompiling those translation units, and the gate builds `Tools/` and `Examples/`, which had been excluded long enough to rot.
2. `ManagerApp` builds and is signed, but remains unverified against a live driver, per README.
3. Still at zero coverage, and not by oversight: `AES67Device`, `AES67IOHandler`, `PTPArbitrator` and the CoreAudio clock sources. Each needs CoreAudio or libASPL, so a suite for them cannot be run from a remote session; writing one is a job for a session on the Mac. `BenchmarkIOHandler` exercises the IO handler but is not a CTest.
4. The `t41-ptp` servo split has not been through the Teensy toolchain. `pio run` on the box before flashing anything.
5. `TestPTPMasterBoxInterop` replays bytes built from the library's source. A real capture off the box would be worth more and drops into the same shape.
6. `NetworkErrorHandler::attemptRecovery` in the core clears its own latch before returning, so `isInRecovery()` is false wherever a caller can observe it and a second recovery always starts. Pinned by a test that documents the behaviour; making the latch real is a behaviour change.
7. The firmware PR `JaumeAP/DTS-Player#33` needs `pio test -e native` before merging, and a decision on `-DDTS_DSP_QMF_FLOAT`.
8. `dts-dsp` needs `COPYING.LESSER`, the LGPL-2.1 text, which nobody has downloaded yet.
9. `PTPMasterSettings` keeps the two message intervals in milliseconds, and milliseconds cannot express every legal rate: 16 Sync per second is 62.5 ms and the field is an `int`. The half that mattered is fixed — both rates come from one exponent settled in the constructor, and a non-power-of-two setting is rounded to the nearest legal interval and sent at that rate rather than misdeclared. What is left is where the rounding happens. Storing `int8_t logSyncInterval` and `int8_t logAnnounceInterval` in the settings, with milliseconds as a rendering for `ManagerApp`, would put it where the value is chosen and shown instead of silently at the port. That is a settings file format change — a version bump plus a read path mapping an old `syncIntervalMs` to the nearest exponent — and the struct lives in `aes67-core`, not here. `JaumeAP/AES67-master-box-` is already built that way (`src/profiles.h`).
