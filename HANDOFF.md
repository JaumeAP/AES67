# HANDOFF.md

## Evergreen state

Active branch: `chore/skills-to-user-scope`, merged into `main` at session close.

Configuration: CLAUDE.md in repo; workflow skills at user scope as the `session-rules@jaumeap` plugin (repo `JaumeAP/claude-plugins`), not vendored here. `.claude/` no longer carries skills or settings.

Git workflow: autonomous local merge (no PR) per git-rules. Response style: Catalan replies, English code/commits. Start replies with `Rebut: <order>`. Numbered lists only, no bold/headers/tables.

Architecture invariants: RT-safe thread boundaries (IO thread touches only RTSafeStreamInterface + lock-free ring buffers, never StreamManager), AudioServerPlugIn via libASPL. Stream configs persist as JSON.

Build/test: macOS (Apple Silicon + x86_64) + cross-platform tests, CMake out-of-source, per-subsystem CTests. Run via `ctest --output-on-failure`; exclude network tests in CI: `ctest -E "RingBuffer|PTPClock|IntegrationAudioPath"`.

## Session summary — Workflow-skill plugin migration and doc sync (2026-08-25)

Audited the session-rules skills and their hooks. Two of the six hooks were dead — the Stop hook emitted a field that event's output schema does not accept, and the periodic re-read reminder aborted silently in every repo — and nothing had degraded while they were, so the set was cut to two: the PreToolUse gate that denies every tool call until `HANDOFF.md` has been read, and the PostToolUse companion that releases it. The gate now also clears on a Bash read; it previously deadlocked any session running in a Bash-preferring mode.

Packaged the plugin as a marketplace release: `JaumeAP/claude-plugins` (private), installed as `session-rules@jaumeap` 0.2.1, tagged `session-rules--v0.2.1`. The plugin cache is keyed by version, so any content change needs a version bump to reach an installation.

In this repo, CLAUDE.md and HANDOFF.md still described the old skills-dir layout and six hooks. Both corrected, along with a stale active branch and a stale "no open items", then merged to `main` (ae1bdcd).

## Open items

None.
