# HANDOFF.md

## Evergreen state

All work merged to main. No active feature branches. Configuration: user's portable bundle (CLAUDE.md, .claude/ config, 25 skills) fully integrated and binding.

Git workflow: autonomous local merge (no PR), per bundle's git-rules. Response style: Catalan, telegraphic. Development targets: macOS Apple Silicon + x86_64, CMake out-of-source, per-subsystem CTest targets.

## Session summary (2026-07-24)

Closed session by removing harness override that mandated branch+PR workflow. User requested bundle's autonomous local-merge rule be the only binding configuration. Edited CLAUDE.md to remove "Superseded in this repo (2026-07-24)" clauses from session-close and sync-command sections, restored to original bundle text. Edited HANDOFF.md to remove harness reference. Committed both edits, pushed branch, merged to main locally per git-rules, pushed main. PR #1 merged by user/automation. Configuration now: bundle's rules exclusively.
