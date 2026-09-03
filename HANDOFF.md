# HANDOFF.md

## Evergreen state

Active branch: `claude/aes67-gy7n0u`, merged into `main` at session close.

Configuration: CLAUDE.md in repo; workflow skills at user scope as the `session-rules@jaumeap` plugin (repo `JaumeAP/claude-plugins`), not vendored here. `.claude/` no longer carries skills or settings.

Git workflow: autonomous local merge (no pull request) per git-rules. Response style: Catalan replies, English code/commits. Start replies with `Rebut: <order>`. Numbered lists only, no bold/headers/tables.

Architecture invariants: RT-safe thread boundaries (IO thread touches only `RTSafeStreamInterface` and the lock-free ring buffers, never `StreamManager`), AudioServerPlugIn via libASPL. Stream configs persist as JSON.

Platform-free core: `JaumeAP/aes67-core`, consumed as the `external/aes67-core` submodule — SDP, RTP wire header, jitter buffer, packet pool, PLL, resampling, channel mapping, configuration, and since 2026-09-03 the ring buffer, the RT-safe view over it, the PTP time types and clock-source interface, and the PTP peer, RTCP receiver and Dolby model tables. What stays here is macOS plus `aes67_net`, the socket layer, which supplies the four `NetworkUtils` symbols the core declares without implementing. The boundary was reviewed on 2026-09-03 and deliberately left where it is: moving sockets into the core would cost the platform-free contract, two commits per network fix, and `#ifdef`s for `IP_MULTICAST_IF`, `net/if_dl.h` and `SO_TIMESTAMP`. Clone with `--recurse-submodules`.

Build/test: macOS (Apple Silicon + x86_64), CMake out-of-source. Suites are doctest, labelled `unit`/`timing`/`network`/`integration`/`interop`, each with `TIMEOUT 60`. `IntegrationAudioPath` needs real multicast and fails outside the gate by design.

**This repository cannot be configured off a Mac**: `project()` declares OBJCXX, so `cmake` fails on Linux before anything builds. A remote session can still check a change by compiling the translation units it touches by hand (`g++ -fsyntax-only -I. -Iexternal/aes67-core`), which is how the PTP work of 2026-09-03 was verified — with a local shim for `net/if_dl.h` and a stub for `NetworkUtils::setQoSTrafficClass`, neither committed.

CI: local only, GitHub Actions disabled and deleted. `scripts/ci-local.sh` is the gate, run by `.githooks/pre-push` (opt in per clone with `git config core.hooksPath .githooks`). Two speeds: build, `ctest -LE timing` and the core's platform contract by default, about a second; clang-tidy only with `--analyse` or `AES67_ANALYSE=1`, minutes. `--sanitize` and `--tsan` run everything but `network`. `scripts/coverage.sh` reports llvm-cov.

Static analysis: `.clang-tidy` runs the defect families and leaves style off; `WarningsAsErrors` is deliberately narrower than `Checks`. Install with `python3 -m venv ~/.local/venvs/cpptools && ~/.local/venvs/cpptools/bin/pip install clang-tidy==21.1.6`.

Manager app previews: `#Preview` blocks live in `ManagerApp/Views/Previews/`, kept out of `build.sh`'s source list because the macro needs full Xcode. New previews go there, and into the Xcode target, never into `build.sh`.

Related repositories, and where they stand: `JaumeAP/aes67-core` `main` at 36c8b47; `JaumeAP/t41-ptp` (the Teensy PTP library) `integration/master-box` at b8b723f; `JaumeAP/AES67-master-box-` (the grandmaster firmware) `main` at c4b6593, carrying t41-ptp as `lib/t41-ptp`. The box does NOT consume `aes67-core`, and after weighing it on 2026-09-03 it stays that way.

## Session summary — Core headers, PTP interop, the box's servo (2026-09-03)

Nine platform-free headers and their five suites went to `aes67-core`, plus `RTPHeader.h`, which was a byte-identical copy shadowing the core's. Two portability defects surfaced when the core was first built off a Mac (`<atomic>`, `<cstdint>` missing) and were fixed there.

`PTPSlave` was read against the master box's real traffic, which had only ever been compared by eye. `TestPTPMasterBoxInterop` replays that traffic through a new `deliverMessage()` seam; `twoStepOnly` is now actually read; the BMCA compares the whole dataset in 1588's order instead of three of its six fields; the receive loop's two copies of the profile checks are one `dispatchMessage`.

In `t41-ptp`, `updateController()`'s decision was split into `servoUpdate()` over plain numbers, with the hardware calls left behind and 51 host checks in front of it — the first tests that library has had, and what `COMPARATIVA-SERVO.md` asks for before touching the servo. The box's submodule was bumped to it.

Rejected on purpose, after weighing: replacing the box's PI loop with the core's `PIController` (different loop — it integrates over dt and the box does not; it would have been a servo change on unmeasured hardware).

## Open items

1. `ManagerApp` builds and is signed, but remains unverified against a live driver, per README.
2. Coverage of this repository's own code is around 9.5% of lines. `RTPReceiver`, `RTPTransmitter`, `PTPSlave`, `PTPMaster`, `AES67IOHandler`, `NetworkUtils` and the CoreAudio clock sources are at 0%. The `deliverMessage()` seam added for PTP is the shape the rest needs: bytes in, state out.
3. Nothing from 2026-09-03 has been through the macOS gate — not the header move, not the PTP changes. Run `scripts/ci-local.sh` on the Mac before trusting any of it.
4. The `t41-ptp` servo split has not been through the Teensy toolchain either. `pio run` on the box before flashing anything.
5. `TestPTPMasterBoxInterop` replays bytes built from the library's source. A real capture off the box would be worth more and drops into the same shape.
6. `NetworkErrorHandler::attemptRecovery` in the core clears its own latch before returning, so `isInRecovery()` is false wherever a caller can observe it and a second recovery always starts. Pinned by a test that documents the behaviour; making the latch real is a behaviour change.
7. The firmware PR `JaumeAP/DTS-Player#33` needs `pio test -e native` before merging, and a decision on `-DDTS_DSP_QMF_FLOAT`.
8. `dts-dsp` needs `COPYING.LESSER`, the LGPL-2.1 text, which nobody has downloaded yet.
9. `PTPMasterSettings` keeps the two message intervals in milliseconds, and `PTPMaster` derives the wire byte from them with `MsToLogInterval` (`PTPMaster.cpp:107`), which rounds: `std::lround(std::log2(seconds))`. The transmit loop uses the milliseconds directly (`PTPMaster.cpp:393`), so the announced rate and the sent rate are two numbers and they part company whenever the configured interval is not a power of two seconds — 100 ms announces as 125. That is the lie `MsToLogInterval`'s own comment says it exists to prevent, and a conforming slave times its master-lost window and its Delay_Req rate off the announced field. Milliseconds also cannot express the rates that are legal: 16 sync per second is 62.5 ms and the field is an `int`. Fix: store the exponent — `int8_t logSyncInterval`, `int8_t logAnnounceInterval` — and derive both the wire byte and the send period from it, so there is only one number to be wrong; milliseconds become a rendering for `ManagerApp`. Costs a version bump in the settings file and a one-time read path mapping an old `syncIntervalMs` to the nearest exponent. `JaumeAP/AES67-master-box-` is already built this way (`src/profiles.h`), and its `HANDOFF.md` records the one-field rule as a trap worth not rediscovering.
