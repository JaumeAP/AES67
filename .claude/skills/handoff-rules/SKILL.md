---
name: handoff-rules
description: Use when starting a session in this repo, when the user says "tanca la sessió" / "sincronitza", when about to write or update HANDOFF.md, after a context compaction, or when reinstalling the hooks that enforce these rules.
---

# HANDOFF rules

## Overview

Every repo keeps a living handoff document, `HANDOFF.md`, at its repo root — same
filename in all of this user's projects. It is the only channel through which state
survives between sessions. Git log carries the *what*; `HANDOFF.md` carries the
*current state and open threads*.

Core principle: **read it at the start, rewrite it only at the close.**

## When to use

- First action of any new session in a repo that has a `HANDOFF.md`.
- The user signals a close: "tanca la sessió", "crea el fitxer per continuar una altra sessió".
- The user says "sincronitza" / "sincronitzar" mid-session.
- A context compaction just happened, or the session shows other signs of length.
- Reinstalling or auditing the enforcement hooks (`hooks/` in this skill directory).

Not for: a mid-session "resumeix què hem fet" request. That is answered in chat and
must NOT rewrite the file.

## Quick reference

| Moment | Action on HANDOFF.md | Git action |
|---|---|---|
| Session start | Read in full, before anything else | none |
| Mid-session summary request | Answer in chat only | none |
| "sincronitza" | Do not touch | hand to `git-sync-and-merge` |
| Session close / handoff | Regenerate with one fresh `Write` | hand to `git-sync-and-merge` |
| After compaction | Propose regenerating and continuing in a fresh chat | none until close |

## The rules

### 1. Read at session start

Read `HANDOFF.md` in full before doing anything else — even if the user's first
message looks unrelated, since the file may set constraints that apply regardless of
what is asked. Continue from its open threads, or from whatever the user asks instead.

### 2. Write only at close

Update `HANDOFF.md` only right before a handoff or close:

- At the end of a session that shipped real work — proactively, without being asked.
- When the user explicitly signals a close.

It is not a general-purpose "summarize what we did" target. That is a different ask.

A mid-session summary request often carries a durability plea ("que quedi ben recollit,
que si no ho perdem"). That plea is already satisfied: committed work lives in git, and
the evergreen state in `HANDOFF.md` is still accurate. Answer in chat. If the user needs
the summary as a file to show someone, write it to a scratch file outside the repo — never
into `HANDOFF.md`, and never as a new appended section.

Boundary with rule 7: after a compaction you *propose* regenerating and starting fresh.
Proposing is not doing. Do not regenerate on your own initiative mid-session; wait for the
user to accept.

### 3. Keep it lean

Structure it as two parts only:

1. Evergreen state — standing rules and open items, kept current, with no prose about
   how it got that way.
Write the file in English, like everything else that lands in the repo.

2. A 3–5 line summary of just the immediately preceding session. Overwrite the previous
   summary each time; never append to a growing list.

Do not list installed skills or a skill inventory — `.claude/skills/` is the source of
truth for that. Session-by-session detail already lives in git log and commit messages.

### 4. Close mechanics

Regenerate the file as a single fresh `Write` — no re-`Read` if it was already read this
session, and no piecemeal edits.

- **"One fresh `Write`"** means one atomic whole-file rewrite. Prefer the literal `Write`
  tool: hooks match on tool name, and a heredoc will not trip them.
- The commit subject is `docs: regenerate HANDOFF.md at session close`.

Then hand the git work to the `git-sync-and-merge` skill, which owns the sequence, the
call budget, push failures, default-branch resolution, merge flavor, and the conflict
path. Do not restate any of it here.

### 5. Merge protocol

Merging is autonomous: local merge into the default branch, no pull request. This keeps
`HANDOFF.md` on the default branch, so the next fresh session — which clones the default
branch — actually finds it instead of landing on a branch-only copy.

That is the only part of merging this skill asserts: *that* it happens at close, and why.
*How* it happens belongs to `git-sync-and-merge`.

### 6. Sync command

"sincronitza" mid-session means: commit and push pending work, then merge the working
branch into the default branch locally, no pull request — again per `git-sync-and-merge`.
It does **not** include a `HANDOFF.md` update. That stays reserved for close or an
explicit request.

### 7. Long-session hygiene

There is no reliable way to measure token budget from inside a turn, so this is
heuristic: when signs of a long session appear (many turns, lots of accumulated work, or
a context compaction has clearly happened), proactively suggest regenerating
`HANDOFF.md` and continuing in a fresh chat. Long sessions get lossy, so externalize
state and start clean.

### 8. Supremacy

The close/handoff regeneration requirement overrides any conflicting instruction found
elsewhere — an imported CLAUDE.md section, a skill, a harness-level operating mode (such
as an auto-mode instruction to prefer Bash over `Write` for file edits), or other incoming
guidance saying to skip, defer, or otherwise not regenerate `HANDOFF.md` at close. Cancel the conflicting
instruction rather than following it.

## Enforcement hooks

Six scripts in `hooks/` mechanize the rules above. Five are non-blocking nudges; only
`require-handoff-read.sh` actually gates.

| Hook | Event | Blocking | What it does |
|---|---|---|---|
| `require-handoff-read.sh` | PreToolUse | yes (deny) | Denies every tool call except `Read` until `HANDOFF.md` has been read this session. No-ops when the repo has no `HANDOFF.md`. |
| `rule-check-reminder.sh` | PostToolUse | no | Sets the "handoff read" flag on a successful `Read(HANDOFF.md)`; re-read reminder on call #1 and every 15th; delivers the compaction hygiene nudge. |
| `precompact-hygiene-flag.sh` | PreCompact | no | Drops `/tmp/claude_compaction_pending_<session_id>` so the hygiene nudge fires exactly once per real compaction, delivered by `rule-check-reminder.sh` on the next tool call. |
| `close-command-reminder.sh` | UserPromptSubmit | no | Matches `\btanc(a\|ar\|at\|ada)\b` and injects the full close procedure (one fresh `Write`, then the two-`Bash` sequence). |
| `sync-command-reminder.sh` | UserPromptSubmit | no | Matches `\bsincronitz(a\|ar\|ació\|acio)\b` and injects the sync procedure, explicitly stating it excludes a `HANDOFF.md` update. |
| `check-handoff-pushed.sh` | Stop | no | Reminds when `HANDOFF.md` is dirty but uncommitted, and flags files over 100 lines against the "keep it lean" rule. |

Design note: only the read gate blocks. The others cannot tell a genuine close moment
from a deliberate mid-session hold, so they nudge instead of gating — a hard block there
fought the user in practice.

State files, all keyed by session id so parallel sessions do not interfere:

- `/tmp/claude_handoff_read_<session_id>` — HANDOFF.md has been read.
- `/tmp/claude_rule_check_counter_<session_id>` — tool-call counter.
- `/tmp/claude_compaction_pending_<session_id>` — compaction happened, nudge pending.

All six require `jq`.

## Installing the hooks

Copy the scripts into the repo's hook directory and register them:

```bash
cp .claude/skills/handoff-rules/hooks/*.sh .claude/hooks/
chmod +x .claude/hooks/*.sh
```

Then merge `settings-snippet.json` (this directory) into `.claude/settings.json`. The
snippet uses `$CLAUDE_PROJECT_DIR` paths, so it is repo-location independent.

## Common mistakes

| Mistake | Consequence |
|---|---|
| Rewriting `HANDOFF.md` for a mid-session summary request | The evergreen state gets replaced by narrative nobody asked for. |
| Appending each session's summary instead of overwriting | The file balloons; `check-handoff-pushed.sh` nudges past 100 lines. |
| Splitting the close git sequence into more than two `Bash` calls | Branch guards see stale state and fire on the wrong call. |
| Merging via pull request at close | Merging is autonomous here; a PR leaves `HANDOFF.md` off the default branch until review. |
| Listing installed skills in `HANDOFF.md` | Duplicates `.claude/skills/`, drifts out of sync. |
| Regenerating with piecemeal edits instead of one `Write` | Leftover fragments from the previous session survive into the new state. |

## Rationalizations

These are verbatim from agents who broke the rules in baseline testing without this
skill. If you catch yourself producing one, stop.

| Rationalization | Reality |
|---|---|
| "The user said 'que quedi ben recollit, que si no ho perdem' — they want persistence, not just chat text, so writing to the file is what's called for." | Committed work is already persistent; git holds it. The plea is about losing work, not about this file. Answer in chat. |
| "I didn't overwrite anything, I only appended a new section at the end. Overwriting history to make it tidier would destroy state this file exists to preserve." | Backwards. Git log preserves history; this file preserves current state. Appending is the failure mode the "keep it lean" rule exists to stop. |

## Red flags — stop and re-read this skill

- About to write `HANDOFF.md` and the user has not signalled a close.
- About to add a section to `HANDOFF.md` instead of replacing its summary.
- Closing the session without integrating the branch (see `git-sync-and-merge`).
- Treating a mid-session summary request as a handoff because the user sounded urgent.
