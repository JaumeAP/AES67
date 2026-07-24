#!/bin/bash
# PreToolUse hook: blocks every tool call except Read until HANDOFF.md has
# actually been read this session (flag set by rule-check-reminder.sh's
# PostToolUse companion once a Read(HANDOFF.md) succeeds). Enforces
# CLAUDE.md's "before doing anything else, read HANDOFF.md in full"
# structurally instead of only hoping it's remembered.
set -euo pipefail

handoff="${CLAUDE_PROJECT_DIR:-}/HANDOFF.md"
if [ ! -f "$handoff" ]; then
  echo '{}'   # no HANDOFF.md in this project -- nothing to gate on
  exit 0
fi

input="$(cat)"
sid="$(echo "$input" | jq -r '.session_id // "default"' 2>/dev/null || echo "default")"
flag="/tmp/claude_handoff_read_${sid}"

if [ -f "$flag" ]; then
  echo '{}'
  exit 0
fi

tool_name="$(echo "$input" | jq -r '.tool_name // ""' 2>/dev/null || echo "")"
if [ "$tool_name" = "Read" ]; then
  echo '{}'   # let the Read through -- it might be the HANDOFF.md read itself
  exit 0
fi

jq -n '{hookSpecificOutput: {hookEventName: "PreToolUse", permissionDecision: "deny", permissionDecisionReason: "Has de llegir HANDOFF.md (repo root) sencer abans de fer cap altra acció aquesta sessió -- usa Read primer."}}'
