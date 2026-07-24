#!/bin/bash
# UserPromptSubmit hook: mechanizes detection of CLAUDE.md's "sincronitza"
# sync command (Session continuity section) -- the user typing
# "sincronitza"/"sincronitzar" mid-session means commit+push pending work
# then merge the working branch into main locally, no pull request. Does
# NOT include a HANDOFF.md update -- that stays reserved for session
# close or an explicit request, so the reminder says so too, to avoid the
# two commands being conflated.
#
# Non-blocking (additionalContext), same pattern as every other reminder
# hook here -- deciding whether this message actually is that command,
# and carrying it out, stays Claude's judgment call.
set -euo pipefail

input="$(cat)"
prompt="$(echo "$input" | jq -r '.prompt // ""' 2>/dev/null || echo "")"

if [ -z "$prompt" ]; then
  echo '{}'
  exit 0
fi

lower="$(echo "$prompt" | tr '[:upper:]' '[:lower:]')"

if echo "$lower" | grep -qE '\bsincronitz(a|ar|ació|acio)\b'; then
  msg="Aixo sembla l'ordre 'sincronitza' (CLAUDE.md, Session continuity): fes commit i push del treball pendent, despres fusiona la branca de treball a main en local (git checkout main && git merge <branca> && git push origin main), sense pull request. NO inclou actualitzar HANDOFF.md -- aixo nomes es fa en tancament real o si es demana explicitament."
  jq -n --arg msg "$msg" '{hookSpecificOutput: {hookEventName: "UserPromptSubmit", additionalContext: $msg}}'
else
  echo '{}'
fi
