# HANDOFF.md

## Evergreen state

Active branch: `claude/todo-implementation-tsxtnu` — TODO implementation, dead code removal, CI workflow, libASPL submodule.

Configuration: portable bundle (CLAUDE.md, .claude/, 25 skills) fully integrated.

Git workflow: autonomous local merge (no PR) per git-rules. Response style: Catalan, telegraphic. Development: macOS (Apple Silicon + x86_64) + cross-platform tests, CMake out-of-source, per-subsystem CTests.

Architecture invariants: RT-safe thread boundaries (IO thread touches only RTSafeStreamInterface + lock-free ring buffers, never StreamManager), AudioServerPlugIn via libASPL. Stream configs persist as JSON.

## Session summary (compacted turn, 2026-07-24)

Completed TODO implementation: `StreamChannelMapper::fromJSON()` parses JSON stream mappings, added round-trip + clearing tests (all 12 tests passing). Removed 111 dead code files: 12 unused RTP implementations (CircularJitterBuffer, JitterBuffer, etc.), entire ptpd vendor directory. Created GitHub Actions workflow (cmake configure + make + ctest on macOS-latest). Added libASPL as git submodule (https://github.com/gavv/libASPL, external/libASPL). Made ASPL optional in CMakeLists.txt to allow CI to run without driver plugin when not on macOS. Fixed CMakeLists.txt flow control nesting error (removed orphaned else/endif). Pushed latest syntax fix. Awaiting CI success on current commit to proceed with PR.
