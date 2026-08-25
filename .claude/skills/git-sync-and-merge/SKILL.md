---
name: git-sync-and-merge
description: Use when the user says "sincronitza" or signals a session close, when integrating a working branch into the default branch, or when a push, merge, or the close git sequence has to run without stalling on a missing remote or a conflict.
---

# Git sync and merge

## Overview

Branch integration here is **autonomous and local**. No pull request, no approval wait.
The two moments that trigger it are the "sincronitza" command mid-session and a session
close. Both run the same sequence; only the surrounding work differs.

Core principle: **the sequence completes or the repo is left clean — never half-merged.**

## When to use

- The user says "sincronitza" / "sincronitzar".
- The user signals a session close and work has to be integrated.
- A branch is ready and needs to reach the default branch.
- A push fails or is refused and the chain must not stall.
- A merge conflicts mid-sequence.

Not for: deciding *whether* `HANDOFF.md` should be rewritten. That is a separate concern —
see the `handoff-rules` skill.

## Quick reference

| Question | Answer |
|---|---|
| Pull request or local merge? | Local merge, always. Never a PR, never a GitHub merge API call. |
| Merge flavor | `--no-ff`, so the branch stays visible in history. Never squash. |
| Default branch | `git symbolic-ref --short refs/remotes/origin/HEAD` when a remote exists; otherwise `main`. |
| How many Bash calls | Two: an isolated checkout, then everything else chained. |
| No remote configured | Omit the pushes entirely. |
| Push runs and fails | Guard it so the chain survives. |
| Merge conflicts | Cap suspended: resolve or `git merge --abort`, then tell the user. |

## The sequence

Exactly two Bash calls, never more:

**Call 1** — isolated, alone:

```bash
git checkout <branch>
```

It is isolated because branch guards inspect git state at the moment of the call. A
`checkout` buried later in a chain leaves those guards reading the branch you were on,
not the one you will be on.

**Call 2** — everything else, chained with `&&`:

```bash
git add -A && git commit -m "<subject>" \
  && { git push origin <branch> || echo "PUSH FAILED - continuing"; } \
  && git checkout <default> \
  && git merge --no-ff <branch> -m "merge: <branch> into <default>" \
  && { git push origin <default> || echo "PUSH FAILED - continuing"; } \
  && git checkout <branch>
```

## Failure paths

**No remote at all.** Check with `git remote -v` — you are already running it to resolve
the default branch. If it is empty, drop both push steps rather than guarding them. A
guard only catches a push that runs and fails; a push refused *before* it runs — permission
classifier, sandbox, missing credential helper — never reaches the `||`, and the chain dies
with it.

**Push runs and is rejected.** The `|| echo` guard absorbs it. Report the failure in your
reply; do not silently swallow it.

**Merge conflict.** The chain stops with HEAD on the default branch and a dirty tree. The
two-call cap is suspended from that point: resolve the conflict, or `git merge --abort`,
using as many calls as it takes. Tell the user either way. Never leave the repo mid-merge.

**A call is denied.** A denied call changes no git state. Re-issuing it corrected is not a
third call — only executed calls count against the cap.

## Commit subjects

Conventional Commits. The merge commit is `merge: <branch> into <default>`. A commit that
carries a regenerated handoff document is `docs: regenerate HANDOFF.md at session close`.

## Common mistakes

| Mistake | Consequence |
|---|---|
| Opening a pull request | Integration stalls waiting for a review nobody is going to do. |
| Merging through the GitHub merge API or `gh pr merge` | The commit is attributed to the API's identity, not this session's configured git identity. |
| Squashing | Loses the branch's individual commits; explicitly forbidden. |
| Putting the checkout inside the chain | Branch guards read stale state and fire on the wrong call. |
| Bare `git push` in a remoteless repo | Aborts the `&&` chain before the merge; costs a third call. |
| Leaving a conflicted merge in place to ask what to do | The repo sits mid-merge, on the wrong branch, with a dirty tree. |

## Rationalizations

Verbatim from agents who skipped the merge in baseline testing, and why each is wrong.

| Rationalization | Reality |
|---|---|
| "'Tanca la sessió' means close, not promote code to main. Merging under time pressure without review is exactly when you don't touch the main branch." | Merging here is autonomous by design and time pressure changes nothing about it. |
| "'Sync' is not 'integrate'. Merging off one ambiguous word would be a scope expansion." | "sincronitza" is a defined command in this repo, not an ambiguous word. |
| "Nobody approved integrating this branch, so I'll leave it as an open item." | No approval exists to wait for. Merge locally. |
| "The push failed, so I stopped and used another call." | Guard it or omit it. A failed push must not abort the sequence. |

## Red flags — stop and re-read this skill

- Reaching for a third Bash call in the sequence.
- About to open a pull request.
- Leaving a branch unmerged because it "wasn't reviewed".
- A bare `git push` inside an `&&` chain.
- Walking away from a conflicted merge.
