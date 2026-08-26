# HANDOFF.md

## Evergreen state

Active branch: `chore/skills-to-user-scope`, merged into `main` at session close.

Configuration: CLAUDE.md in repo; workflow skills at user scope as the `session-rules@jaumeap` plugin (repo `JaumeAP/claude-plugins`), not vendored here. `.claude/` no longer carries skills or settings.

Git workflow: autonomous local merge (no PR) per git-rules. Response style: Catalan replies, English code/commits. Start replies with `Rebut: <order>`. Numbered lists only, no bold/headers/tables.

Architecture invariants: RT-safe thread boundaries (IO thread touches only RTSafeStreamInterface + lock-free ring buffers, never StreamManager), AudioServerPlugIn via libASPL. Stream configs persist as JSON.

Platform-free core: extracted to `JaumeAP/aes67-core`, consumed as the `external/aes67-core` submodule — SDP, RTP wire header, jitter buffer, packet pool, PLL, resampling, channel mapping, configuration. What stays here is macOS-specific plus `aes67_net`, the socket layer, which supplies the four `NetworkUtils` symbols the core declares without implementing. Clone with `--recurse-submodules`.

Build/test: macOS (Apple Silicon + x86_64), CMake out-of-source. 13 doctest suites, labelled `unit`/`timing`/`network`/`integration`, each with `TIMEOUT 60`. `IntegrationAudioPath` needs real multicast and fails outside the gate by design.

CI: local only, GitHub Actions disabled and deleted. `scripts/ci-local.sh` is the gate — build, `ctest -LE timing`, the core's own platform contract, and clang-tidy — run by `.githooks/pre-push` (opt in per clone with `git config core.hooksPath .githooks`). `--sanitize` and `--tsan` run everything but `network`. `scripts/coverage.sh` reports llvm-cov.

Static analysis: `.clang-tidy` runs the defect families and leaves style off; `WarningsAsErrors` is deliberately narrower than `Checks`. clang-tidy is not in the Command Line Tools — install with `python3 -m venv ~/.local/venvs/cpptools && ~/.local/venvs/cpptools/bin/pip install clang-tidy==21.1.6`, matching the system compiler. The script skips itself with a message when it finds none.

Manager app previews: `#Preview` blocks live in `ManagerApp/Views/Previews/`, kept out of `build.sh`'s source list because the macro needs full Xcode. New previews go there, and into the Xcode target, never into `build.sh`.

## Session summary — Core extraction, doctest, static analysis (2026-08-26)

The platform-free core moved to its own repository so the firmware, a Linux daemon and this driver are peers rather than consumers of a macOS driver, and this repo now takes it back as a submodule. The extraction surfaced a link-time seam the header check could not see: `StreamConfig` calls `NetworkUtils`, whose implementation opens sockets.

All thirteen suites moved to doctest — 3055 assertions run where 833 were written, because checks inside loops now count per execution. The same migration went through `aes67-core`, `dts-dsp`, `llibreries`, `subtitle-sync` and `DTS-Player-MacOS`; the firmware keeps Unity.

clang-tidy ran here for the first time and found nine real defects, three in the real-time path: products computed in `UInt32` then used as `size_t` byte counts and indices, `std::memset` over a struct of eleven atomics read from another thread, and three destructors that could let an exception out during unwinding.

One self-inflicted failure worth remembering: a regex edit to `Tests/CMakeLists.txt` deleted twelve of thirteen `add_executable` blocks, and nothing noticed because stale binaries in `build/` kept passing. `Examples/` and `Tools/` had also been broken since the extraction, invisible because the gate builds with both off.

## Open items

1. `ManagerApp` builds and is signed, but remains unverified against a live driver, per README.
2. Coverage of this repository's own code is 9.5% of lines. `RTPReceiver`, `RTPTransmitter`, `PTPSlave`, `PTPMaster`, `AES67IOHandler`, `NetworkUtils` and the CoreAudio clock sources are all at 0%. Raising it needs a seam for the transport and the clock, the way `NetworkUtils` already has one — not more mechanical tests.
3. `NetworkErrorHandler::attemptRecovery` in the core clears its own latch before returning, so `isInRecovery()` is false wherever a caller can observe it and a second recovery always starts. Pinned by a test that documents the behaviour rather than the comment; making the latch real is a behaviour change.
4. The firmware PR `JaumeAP/DTS-Player#33` needs `pio test -e native` before merging, and a decision on `-DDTS_DSP_QMF_FLOAT`: the default `double` is bit-exact with the reference, `float` costs 3 samples in 2048 and buys back software-emulated double on the P4's single-precision FPU.
5. `dts-dsp` needs `COPYING.LESSER` — the LGPL-2.1 text — which nobody has downloaded yet.
