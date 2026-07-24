#!/bin/bash
# SessionStart hook: when a new chat/session begins, initialize and update
# every git submodule first, before any other work happens, so the working
# tree is complete from the session's very first tool call. No-op (empty
# output) when the repo has no .gitmodules. Exported: listed by name in
# export-config-skill.sh's hook allowlist, same as every other generic
# hook here.
#
# external/eines-de-creacio (JaumeAP/Eines) is PRIVATE and is not in a fresh
# session's GitHub scope by default, so a plain submodule update for it
# fails every time with a no-TTY auth prompt -- this is expected, not a
# transient error (2026-07-24 user request to stop treating it as a manual
# "revisa" chore). A hook is just a shell script: it cannot call the
# add_repo MCP tool itself, so it cannot fix this alone. What it CAN do is
# turn the failure into a precise, actionable instruction instead of a dead
# end -- Claude has a standing, dated, explicit instruction (CLAUDE.md,
# Project-specific rules) to act on it every session, and add_repo itself
# is pre-approved in .claude/settings.json so acting on it doesn't prompt.
#
# Status checks use command substitution + a here-string for grep, NOT a
# live `cmd | grep -q` pipe: under `set -o pipefail`, `grep -q` closes its
# stdin as soon as it finds a match, so the upstream writer (git) can get
# SIGPIPE (exit 141) even though grep matched correctly -- pipefail then
# reports the pipeline as failed. Capturing to a variable first sidesteps
# this entirely (git already exited before grep ever runs).
set -euo pipefail

project_dir="${CLAUDE_PROJECT_DIR:-.}"
cd "$project_dir"

if [ ! -f .gitmodules ]; then
  echo '{}'
  exit 0
fi

# Public submodules first -- no auth needed, safe to attempt unconditionally.
timeout 60s git submodule update --init external/ravenna-alsa-lkm external/aes67-macos-driver >/dev/null 2>&1 || true

public_status="$(git submodule status external/ravenna-alsa-lkm external/aes67-macos-driver 2>/dev/null || true)"
eines_status="$(git submodule status external/eines-de-creacio 2>/dev/null || true)"

msg=""

if grep -q '^-' <<<"$public_status"; then
  msg="Avis: algun submodul public (ravenna-alsa-lkm / aes67-macos-driver) no s'ha inicialitzat (timeout o error de xarxa) -- revisa manualment amb 'git submodule status'."
fi

if grep -q '^-' <<<"$eines_status"; then
  eines_msg="external/eines-de-creacio (JaumeAP/Eines, private) no esta inicialitzat. Regla fixa (CLAUDE.md, Project-specific rules, 2026-07-24): crida add_repo(owner: JaumeAP, repo: Eines) ara mateix, i despres 'git submodule update --init external/eines-de-creacio' amb un timeout generos (clona a traves del proxy git de la sessio) -- fes-ho proactivament, sense esperar que t'ho demanin."
  if [ -n "$msg" ]; then
    msg="${msg} ${eines_msg}"
  else
    msg="$eines_msg"
  fi
fi

if [ -n "$msg" ]; then
  jq -n --arg msg "$msg" '{hookSpecificOutput: {hookEventName: "SessionStart", additionalContext: $msg}}'
else
  echo '{}'
fi
