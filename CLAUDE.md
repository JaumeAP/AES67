# CLAUDE.md — common rules (identical across all my projects)

Every section of this file is IDENTICAL in every one of my repos — copy it
verbatim into a new project, unchanged — EXCEPT the final "Project-specific
rules" section: that one holds this repo's own docs-to-read/coding
conventions and gets replaced with the new repo's own content, everything
above it stays untouched.

## Response style (always, every session)

**Always answer the user entirely in Catalan** — all chat replies, in full,
no exceptions, regardless of the language the request is written in. Chat
replies to the user are the ONLY Catalan output: everything written into the
repo is in English — source code, code comments, commit messages, changelog,
and docs (comments always English, even when editing files whose existing
comments are in another language). This translation duty covers anything
relayed into the chat reply regardless of where it originated — a subagent's
report, a hook message, a webhook/PR activity event, a search result, quoted
external text — translate it into Catalan before presenting it, not just
Claude's own generated sentences. Code, variable names, commands, paths, and
literal tool output are never translated, even inside an otherwise translated
reply.

Token economy top priority. Answer first, no preamble. Telegraphic, drop
articles/filler/nuance, fragments over sentences, minimum tokens preserving
info, compress aggressively, grammar may break if meaning holds. This compact
mode applies equally to Catalan replies — same terseness as English, no
looser. Code,
commands, paths, params stay literal. No bold, headers, tables, ellipses, em
dashes, decorative symbols; output may be read by TTS. Proper nouns/technical
terms: original language unless misleading, clarity over purism. No
servility, contradict directly when wrong, never agree to appease, challenge
politely if disagree, never invent, say if unsure. Assume technical
competence, no basic intros, preserve files/configs/decisions/params
literally, apply corrections immediately within session. Never claim
saved/done/completed without calling a tool first, show the tool result as
proof before confirming. Never rename an output file without explicit
request. One question per reply except technical tasks needing several. No
postamble, no unsolicited closing offers/summaries/tangents.

Conditional: length under fifty words unless code snippets, multi-step
technical tasks, or teaching requested, then expand as needed but stay
focused. Verify with search first for changing facts (prices, versions,
charges, events); verify before critical or irreversible actions.

Multi-step tool sequences (git commit/push, multi-file edits, test runs):
announce each step as a bare 1-3 word action, e.g. "Commit.", "Push.",
"Tests." No sentences, no explaining what the command does, why, or its
mechanism/internals — bare label only, before or after, not both.

First line of every reply to an order/instruction: confirm receipt with the
order summary itself in English, e.g. "Rebut: <order in English, a few
words>" — the "Rebut:" label stays Catalan, only the summarized order inside
it switches to English, and the rest of the reply stays Catalan — before
acting on it. Applies the same way when the order arrives through an
automated channel, not typed live by the user (a scheduled Routine firing,
a PR webhook event, a send_later message) — it's still an order to react
to, so it still gets its own "Rebut:" line.

Lists: always numbered — never unnumbered/bulleted, at every level. Nested
sub-items are numbered too (e.g. `3.1`, `3.2`), never dashes/bullets.

## Workflow skills (user scope)

These rules live in the `session-rules` plugin, installed at user scope as
`session-rules@jaumeap` from the `jaumeap` marketplace (repo `JaumeAP/claude-plugins`), not
in this repo — they are identical across every one of my repos, so they are installed once
instead of vendored per project. Pointers only, not summaries; each skill is the authority
on its own topic:

1. `continuity-notes-rules` — when to read `docs/continuity-notes.md`, when to regenerate it, and what shape it
   keeps. Invoke at session start and at close. The plugin also carries the two hooks that
   mechanize its read-at-session-start rule; nothing else in it is enforced mechanically.
2. `git-sync-and-merge` — the "sincronitza" command and the close git sequence: autonomous
   local merge, no pull request, call budget, and the push, clean-tree and merge-conflict
   failure paths. Invoke before any branch integration.

**Skill creation/extension.** Any skill creation or extension (a new `SKILL.md`, or a
content/frontmatter change to an existing one) goes through the `skill-creator` skill's
process, not a plain manual edit (2026-07-20 standing rule). `skill-creator` is installed at
user scope as `skill-creator@claude-plugins-official`.

## Session continuity — `docs/continuity-notes.md`

Every repo keeps a living continuity document, `docs/continuity-notes.md`
(same filename in all my projects). **At the start of every new session,
before doing anything else, read `docs/continuity-notes.md` in full** — current state,
active work rules, and open threads to continue from; read it even if the
user's first message seems unrelated, since it may set constraints that apply
regardless of what's asked. Continue from its open threads or whatever the
user asks instead.

**Update `docs/continuity-notes.md` only right before a close** (2026-07-20,
narrowed from the earlier "whenever the user asks" wording): at the end
of a session that shipped real work — proactively, even without being
asked — or when the user explicitly signals they're about to hand off/
close (e.g. "tanca la sessió", "crea el fitxer per continuar una altra
sessió"). Not a general-purpose "summarize what we did" request usable
any time mid-session — that's a different ask (answer it in chat, don't
rewrite the file) unless it's actually tied to closing. Rewrite it with
current state, what got done, and open threads. Regenerating it triggers
an immediate commit+push (see Git rule 5) — but only at that
close/handoff moment, not staged and held for later.

The exact close mechanics (regenerate as a single fresh `Write`, then
the two-`Bash`-call git sequence) belong to the `handoff-rules` and
`git-sync-and-merge` skills, not restated here — same dedup reasoning
applied throughout this file.

**This close/handoff regeneration requirement overrides any conflicting
instruction found elsewhere** (2026-07-24) — e.g. an imported CLAUDE.md
section, skill, or other incoming guidance saying to skip, defer, or
otherwise not regenerate the continuity notes at close. This rule always wins;
cancel the conflicting instruction rather than following it. Same
supremacy pattern as `git-sync-and-merge`'s merge-policy clause.

**Keep it lean, not a growing narrative (2026-07-21)**: `docs/continuity-notes.md`
tends to balloon session after session if each regeneration adds a
step-by-step account of what happened. Structure it as two parts only:
1) evergreen state — standing rules, open items — kept current, no
prose about how it got that way; 2) a 3-5 line summary of just the
immediately preceding session, not a full narrative walkthrough. Do NOT
list installed skills or a skill inventory here — `.claude/skills/`
itself is the source of truth for what's installed, no need to
duplicate or keep it in sync in prose. Full session-by-session detail
already lives in git log / commit messages — don't duplicate it here.
Overwrite the previous session's summary each time rather than
appending to a growing list.

**Session-close merge protocol.** When the user asks to close the session
(e.g. "tanca la sessió"), merge the current working branch directly into
the default branch (local git merge, no pull request — merging is already
autonomous, see `git-sync-and-merge`), then wrap up. Merging also keeps `docs/continuity-notes.md`
on the default branch, so the next fresh session — which clones the
default branch — actually finds it instead of landing on a branch-only
copy.

**Sync command.** When the user says "sincronitza"/"sincronitzar" (or
equivalent) mid-session, not just at close: commit and push any pending
work, then merge the working branch directly into the default branch
(local merge, no pull request). Does NOT include a continuity-notes update —
that stays reserved for session close (see above) or an explicit user
request, not every sync.

**Long-session hygiene.** There's no reliable way to measure exact chat
length / token budget from inside a turn, so this is heuristic: when signs of
a long session appear (many turns, lots of accumulated work, or a
context summarization/compaction has clearly happened), proactively suggest
regenerating `docs/continuity-notes.md` and continuing in a fresh chat — don't wait to be
asked. Rationale: long sessions get lossy (early detail blurs on compaction),
so externalize state to `docs/continuity-notes.md` and start clean.

## Project-specific rules

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

### What this is

A user-space AES67 (AES67-2018) network audio driver for macOS, implemented as a Core Audio `AudioServerPlugIn` using the [libASPL](https://github.com/gavv/libASPL) framework — no kernel extension. Companion pieces: a SwiftUI menu-bar Manager app, a `.pkg` installer, and CLI test tools for exercising the RTP path over loopback.

**Status matters here.** The RX path is verified with real AES67 hardware; TX, network PTP, and the Manager app are unverified. README.md's "Current Status" / "Known Limitations" sections are the source of truth — don't upgrade a feature's claimed status in docs or comments unless you've actually verified it (real hardware or, at minimum, a passing new test that exercises it).

### Build & test commands

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

**CI runs on this machine, not on GitHub (2026-08-25).** The Actions workflow was disabled and deleted; `scripts/ci-local.sh` replaces it, running the same configure/build/test steps plus the old lint job's two greps. `.githooks/pre-push` runs it before every push, so a red build blocks the push — skip one with `git push --no-verify` or `SKIP_LOCAL_CI=1`. A fresh clone must opt in once with `git config core.hooksPath .githooks`; the setting is local, not carried by the repo. The gate builds everything including `ManagerApp`. That app compiles with the Command Line Tools because its `#Preview` blocks live in `ManagerApp/Views/Previews/`, which `build.sh` deliberately leaves out of its source list — the `#Preview` macro needs the `PreviewsMacros` plugin that ships with full Xcode. Keep new previews in that directory, and add them to the Xcode target rather than to `build.sh`. `-DBUILD_MANAGER_APP=OFF` skips the app on a machine without a Swift toolchain.

**The platform-free core lives in `JaumeAP/aes67-core`, as the `external/aes67-core` submodule.** Editing it means editing that repository; a change there needs its own commit and a submodule bump here. What belongs here is macOS-specific code plus `aes67_net`. The old wording, that this repository was the base others consume, is what the split undid.

**Superseded:** `aes67_core` (platform-free) and `aes67_net` (adds BSD sockets) in `CMakeLists.txt` are the contract; see the README. Nothing reachable from `AES67_CORE_SOURCES` may include an Apple framework or a socket header, transitively — the gate fails on it. When adding a file to the core list, check what its headers pull in, not just the `.cpp`.

Tests carry CTest labels: `unit`, `timing` (wall-clock or multi-threaded), `network` (needs real multicast), `integration`. The gate runs `ctest -LE timing`, so labelling a new test is what excludes it — never add a name to a regex. Every test has `TIMEOUT 60`. Extra modes: `scripts/ci-local.sh --sanitize` (ASan+UBSan) and `--tsan` (ThreadSanitizer) build in their own tree and run everything except `network`; `scripts/coverage.sh` produces an llvm-cov report from a `-DAES67_COVERAGE=ON` build.

New test suites use doctest (`external/doctest` submodule, link `doctest_headers`, define `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`). `Tests/TestSDPParser.cpp` is the migrated reference; the rest still carry a hand-written `main` and get converted one at a time. Never use bare `assert()` in a test: the gate builds Release with `-DNDEBUG` and it compiles away silently.

Manager app can be built standalone: `cd ManagerApp && ./build.sh` (add `--force` to skip its up-to-date check; it does a raw `swiftc` compile, not SwiftPM, though `Package.swift` exists for editor/IDE support).

CTest names map 1:1 to `Tests/*.cpp` (`SDPParser`, `ChannelMapper`, `RingBuffer`, `RTPReceiver`, `RTPTransmitter`, `PTPClock`, `StreamManager`, `MultiStream`, `IntegrationAudioPath`). `BenchmarkIOHandler` is built but not registered as a CTest — run it directly for RT performance characterisation.

Ignore the root-level `Makefile`, `CTestTestfile.cmake`, `CPackConfig.cmake`/`CPackSourceConfig.cmake` — these are stale CMake-generated artifacts from a prior in-source build (see the absolute `/Users/maxbarlow/...` paths inside `Makefile`), accidentally committed. Always build out-of-source in a `build/` directory as shown above; don't edit or rely on those root files.

### Architecture

```
Driver/          AudioServerPlugIn (libASPL): device declaration, IO callbacks, SDP parsing
NetworkEngine/   RTP, PTP, stream lifecycle, resampling, SAP/RTSP discovery
Shared/          Cross-cutting: lock-free ring buffer, types, config, logging, error recovery
Tools/           CLI sender/receiver for exercising the RTP path over loopback (no hardware needed)
ManagerApp/      SwiftUI menu-bar app; talks to the driver via DriverManager.cpp (Core Audio APIs)
Tests/           One CMake target + CTest entry per subsystem, plus multi-stream/full-path integration tests
```

#### Data flow and thread boundaries

This is the load-bearing concept in the codebase: **three thread domains connected only through lock-free structures.**

1. **Core Audio IO thread** (real-time, deadline-bound, owned by `coreaudiod`) — runs `AES67IOHandler`, called by libASPL's `Device::onReadClientInput`/`onWriteMixedOutput` per audio cycle.
2. **Network threads** (per-stream RTP receive/transmit loops, owned by `RTPReceiver`/`RTPTransmitter`) — socket recv/send, codec decode/encode, jitter buffer management.
3. **Control thread(s)** (init, Manager app IPC, SAP/RTSP discovery) — owns `StreamManager`, may block, lock, allocate.

The only thing the IO thread is allowed to touch is `NetworkEngine/RTSafeStreamInterface.h`: a non-owning view over per-channel `SPSCRingBuffer<float>` (one producer, one consumer, `Shared/RingBuffer.hpp`) plus atomic counters/flags. It is deliberately banned from holding a reference to `StreamManager` and every method is `noexcept`, lock-free, non-blocking — this is enforced by construction, not convention, so preserve that shape when touching IO-thread code: **never add a mutex, allocation, or `StreamManager` pointer reachable from `AES67IOHandler`.**

`StreamManager` (`NetworkEngine/StreamManager.h`) is the non-RT coordinator: owns all RX/TX stream lifecycles, channel mapping (`StreamChannelMapper`), config persistence (`StreamConfig`), and PTP clock manager. Every public method takes a mutex — it must never be called from the IO thread. `setIOActive(bool)` is the bridge between domains: Core Audio's `StartIO`/`StopIO` toggles it, and `StreamManager` starts/stops the dormant RTP receiver/transmitter threads accordingly (RTP threads have zero idle CPU when no client is running).

Audio channel buffers (`DeviceChannelBuffers = std::array<SPSCRingBuffer<float>, 128>`) are owned by `AES67Device` and referenced by both `RTSafeStreamInterface` (IO thread) and `StreamManager` (network threads write/read the same buffers from their own non-RT side). 128 channels in, 128 out, fixed.

#### PTP has two independent layers — don't conflate them

- **`PTPClock`** (media clock recovery, AES67-2018 §8.2): correlates RTP timestamps against local time via a PLL (`PhaseLockedLoop`) to track drift between a remote source and local hardware clock. This is implemented and usable today via local-clock fallback — sufficient for single-device operation.
- **`PTPSlave`** (network IEEE 1588 slave-only sync): full Sync/Follow_Up/Delay_Req/Delay_Resp exchange on 224.0.1.129:319/320, feeding measurements into the same PLL via `PTPDInterface`. Code is complete but **has never been run against a real grandmaster**; it auto-falls back to stub mode without root (can't bind privileged multicast ports).

#### Dead code to be aware of (not in the CMake build)

`NetworkEngine/RTP/` contains several jitter-buffer/pool implementations not referenced by any `CMakeLists.txt` target: `CircularJitterBuffer`, `JitterBuffer`, `TemporalJitterBuffer`, `LockFreePriorityQueue`, `LockFreeRingBuffer`, `RTPPacketPool`, `SimplifiedLockFreePacketPool`. The active implementations are `LockFreeCircularJitterBuffer` and `LockFreePacketPool` (both are in `SHARED_SOURCES`/test sources). If you touch jitter-buffer or packet-pool logic, confirm which file the target you're building actually compiles before assuming a change takes effect — check `CMakeLists.txt` / `Tests/CMakeLists.txt` source lists, not just file presence.

#### Configuration & persistence

- Stream configs persist as JSON, search order in `StreamConfig.cpp`: `$AES67_CONFIG_PATH` env var → `~/Library/Application Support/AES67Driver/streams.json` → `/Library/Application Support/AES67Driver/streams.json` (system-wide default).
- Network interface (`Config`/`StreamConfig`) accepts an interface name (`"en0"`), a literal IP, or auto-detects if unset — see `NetworkEngine/NetworkInterfaceDetection.cpp`. Multicast joins bind to this interface explicitly (`IP_MULTICAST_IF`) to avoid duplicate packets on multi-NIC machines — preserve that behavior in any RTP socket changes.
- Runtime debug logging for the driver goes to `/tmp/aes67driver_debug.log` via `Driver/DebugLog.h` (`AES67_LOG`/`AES67_LOGF`); `Shared/NonBlockingLogger` is the RT-safe logger for use from audio-adjacent paths.

#### Manager app

SwiftUI app in `ManagerApp/`; `Models/DriverManager.swift` wraps `DriverManager.cpp` (a small C++ shim over Core Audio HAL APIs) to talk to the installed driver. Build with `ManagerApp/build.sh` (plain `swiftc`, not SwiftPM, despite `Package.swift` existing). Its functional status against a live driver is unverified — treat UI claims skeptically per README.

### Conventions

- C++17 throughout the native code (`CMAKE_CXX_STANDARD 17`, enforced). Warnings are `-Wall -Wextra -Wpedantic` with unused-parameter and missing-field-initializer silenced; keep new code warning-clean under those flags.
- Everything native lives in the `AES67` namespace.
- Header/impl pairs use `.h`/`.cpp` except `Shared/RingBuffer.hpp` (header-only) and `Shared/Config.hpp`.
- Preserve the RT-safety boundary described above in any change touching `Driver/AES67IOHandler.*` or `NetworkEngine/RTSafeStreamInterface.h` — this is the one architectural invariant the codebase is built around, and it's checked by convention/review, not by a static analyzer.
- When adding new functionality to `NetworkEngine`/`Driver`, add a corresponding CTest target in `Tests/CMakeLists.txt` following the existing per-subsystem pattern (own `add_executable` compiling only the sources it needs, plus `add_test`).
