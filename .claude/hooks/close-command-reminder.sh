#!/bin/bash
# UserPromptSubmit hook: mechanizes CLAUDE.md's session close/handoff
# procedure -- the "tanca" chat command. Moved here 2026-07-24 so the
# procedure lives in exactly one place (this hook) instead of also being
# restated as prose in CLAUDE.md, same dedup pattern already applied to
# the config export/import rule.
#
# Non-blocking (additionalContext) like every other reminder hook here --
# it cannot literally force a specific tool-call sequence, only state the
# required one clearly enough that deviating from it is a visible choice,
# not an accident.
set -euo pipefail

input="$(cat)"
prompt="$(echo "$input" | jq -r '.prompt // ""' 2>/dev/null || echo "")"

if [ -z "$prompt" ]; then
  echo '{}'
  exit 0
fi

lower="$(echo "$prompt" | tr '[:upper:]' '[:lower:]')"

if echo "$lower" | grep -qE '\btanc(a|ar|at|ada)\b'; then
  msg="Aixo sembla l'ordre 'tanca' (CLAUDE.md, Session continuity): regenera HANDOFF.md sencer amb un sol Write fresc, sense re-Read si ja s'ha llegit aquesta sessio, i sense edicions a trossos. Despres nomes 2 crides Bash, mai mes: 1) git checkout <branca>, aillada, sola -- garanteix la branca real abans de res mes, un guard tipus no-commit-on-main.sh mira l'estat real de git en el moment, no el que faria un checkout mes endavant dins la mateixa cadena. 2) tota la resta encadenada amb &&: add, commit, push la branca, checkout main, merge, push main, tornar a la branca. No dividir la segona crida en mes trossos."
  jq -n --arg msg "$msg" '{hookSpecificOutput: {hookEventName: "UserPromptSubmit", additionalContext: $msg}}'
else
  echo '{}'
fi
