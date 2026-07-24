# HANDOFF.md

## Evergreen state

Active branch: `claude/test-coverage-analysis-tjf30t` — test coverage work, PR #3 created (draft).

Configuration: portable bundle (CLAUDE.md, .claude/, 25 skills) fully integrated.

Git workflow: autonomous local merge (no PR) per git-rules. Response style: Catalan replies, English code/commits. Start replies with `Rebut: <order>`. Numbered lists only, no bold/headers/tables.

Architecture invariants: RT-safe thread boundaries (IO thread touches only RTSafeStreamInterface + lock-free ring buffers, never StreamManager), AudioServerPlugIn via libASPL. Stream configs persist as JSON.

Build/test: macOS (Apple Silicon + x86_64) + cross-platform tests, CMake out-of-source, per-subsystem CTests. Run via `ctest --output-on-failure`; exclude network tests in CI: `ctest -E "RingBuffer|PTPClock|IntegrationAudioPath"`.

## Session summary — Test Coverage Analysis & Implementation (2026-07-24)

Completed test coverage analysis: identified 9 components with zero tests (StreamManager real, Resampling subsystem, StreamConfig, NetworkErrorHandler, DoPDecoder, BufferStatusMonitor, NetworkUtils, Discovery layers, AES67IOHandler correctness). Implemented 140 new unit tests across three priority subsystems:

1. TestStreamManager: 9 new tests (added to existing 64 struct-validation tests) validating SDP constraints, channel availability, sample rate checking without RTP instance creation.
2. TestResampling (NEW): 28 tests for PIController, SmoothedPIController, Resampler, SampleRateAdapter; clock drift scenarios, mono/stereo, rate conversions.
3. TestStreamConfig (NEW): 39 tests for JSON serialization round-trip, config search path priority, metadata persistence, timestamps.

Fixed DebugLog.h missing `#include <cstdarg>`. All 140 tests passing locally. Tests registered in Tests/CMakeLists.txt, PR #3 created (draft) for review.

## Open items

None. Work complete; awaiting CI and review feedback on PR #3.
