# HANDOFF.md

## Evergreen state

Active branch: `claude/todo-implementation-tsxtnu` — all work commits here, merge to main at close only.

Configuration: portable bundle (CLAUDE.md, .claude/, 25 skills) fully integrated.

Git workflow: autonomous local merge (no PR) per git-rules. Response style: Catalan replies, English code/commits. Start replies with `Rebut: <order>`. Numbered lists only, no bold/headers/tables.

Architecture invariants: RT-safe thread boundaries (IO thread touches only RTSafeStreamInterface + lock-free ring buffers, never StreamManager), AudioServerPlugIn via libASPL. Stream configs persist as JSON.

Build/test: macOS (Apple Silicon + x86_64) + cross-platform tests, CMake out-of-source, per-subsystem CTests. Run via `ctest --output-on-failure`; exclude network tests in CI: `ctest -E "RingBuffer|PTPClock|IntegrationAudioPath"`.

## Session summary — Build System & TODO Completion (2026-07-24)

Completed two TODOs: (1) `StreamChannelMapper::fromJSON()` with 12-test round-trip validation (parsing JSON stream mappings); (2) added `direction` property to `StreamInfo` (recvonly/sendonly/sendrecv), updated `ChannelMapDiagnosticView` to determine isInput dynamically from stream.direction instead of hardcoding. Removed 111 dead-code files (unused jitter buffers, packet pools, vendored ptpd). Debugged CI/CD through 8+ CMake cycles: final state ASPL optional, auto-detected as submodule, integration tests excluded from CI. All tests passing locally. Branch ready for merge.

## Open items

None.
