#!/bin/bash
# Stop hook: was a hard block on any dirty HANDOFF.md; downgraded to a
# non-blocking reminder (2026-07-20) after CLAUDE.md's own rule was
# narrowed -- HANDOFF.md now only gets regenerated+pushed right before an
# actual close/handoff, not on every mid-session request that happens to
# touch it (a plain "resumeix l'estat" ask mid-session is explicitly NOT a
# close signal anymore). This script cannot tell a genuine close moment
# from a deliberate mid-session hold, so hard-blocking would fight the
# user exactly like it did the day this changed: they asked to defer the
# commit, and the old hard block kept firing anyway. Same honest
# limitation as local-merge-reminder.sh/config-ingest-reminder.sh --
# nudge, don't gate.
set -euo pipefail

input="$(cat)"
stop_hook_active="$(echo "$input" | jq -r '.stop_hook_active // false' 2>/dev/null || echo false)"
if [ "$stop_hook_active" = "true" ]; then
  echo '{}'
  exit 0
fi

project_dir="${CLAUDE_PROJECT_DIR:-.}"
handoff="$project_dir/HANDOFF.md"
if [ ! -f "$handoff" ]; then
  echo '{}'   # no HANDOFF.md in this project -- nothing to gate on
  exit 0
fi

if ! git -C "$project_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo '{}'   # not a git repo -- nothing to check
  exit 0
fi

dirty="$(git -C "$project_dir" status --porcelain -- HANDOFF.md 2>/dev/null || true)"
if [ -z "$dirty" ]; then
  echo '{}'
  exit 0
fi

reason="Recordatori: HANDOFF.md esta modificat pero no comitejat/pujat. Si aixo es un tancament/traspas real, fes git commit + git push ara (CLAUDE.md, git-rules regla 6). Si nomes era un resum demanat a mig sessio (no un tancament), es correcte deixar-ho sense pujar fins al tancament real."

# 2026-07-22: also nudge on length -- CLAUDE.md's "Keep it lean, not a
# growing narrative" rule targets exactly the failure mode of HANDOFF.md
# ballooning session after session. A line-count threshold is a rough
# proxy (can't tell prose from legitimate long lists), but this is the
# one natural moment to check: right after a fresh regeneration, before
# it gets committed.
line_count="$(wc -l < "$handoff" 2>/dev/null || echo 0)"
if [ "$line_count" -gt 100 ]; then
  reason="${reason} A mes, HANDOFF.md te ${line_count} linies -- revisa si compleix la regla 'Keep it lean, not a growing narrative' (nomes estat evergreen + resum de 3-5 linies de la sessio anterior, sense inventari de skills ni narrativa pas a pas)."
fi

jq -n --arg reason "$reason" '{hookSpecificOutput: {hookEventName: "Stop", additionalContext: $reason}}'
