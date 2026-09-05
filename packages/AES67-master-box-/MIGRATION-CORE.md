# Folding aes67-core back into aes67_macos_driver

A plan, not a change. Nothing here has been executed. Written on
2026-09-03 by reading `JaumeAP/aes67-core` at `36c8b47` and
`JaumeAP/aes67_macos_driver` at its current `main`.

The decision to merge was taken against the recommendation in
`COMPARATIVA-CORE.md`. That document's "What kills it" is not withdrawn:
the driver's `project()` declares OBJCXX, `cmake` fails on Linux before
anything builds, and merging therefore ends the ability to build and test
the core layer off a Mac. Step 6 below is the only mitigation, and it is
optional work rather than part of the merge.

**Everything in this plan has to be verified on a Mac.** The repository
cannot be configured anywhere else, so none of the steps below were run,
and the checks at the end are the ones that would settle whether it
worked.

## What reading the two trees establishes first

The merge is mechanically much smaller than it sounds, and it is worth
knowing that before planning around a difficulty that is not there.

**No path collides.** Core has 60 `.cpp`/`.h` files under `Driver/`,
`NetworkEngine/` and `Shared/`; the driver has 65 under the same three
directories; the intersection is empty. The two trees overlay without a
single file landing on another.

**No test name collides either.** Core has 19 suites in `Tests/`, the
driver has 19 plus a benchmark and its own `Tests/CMakeLists.txt`.
Intersection empty.

**Includes do not have to be rewritten.** Both sides already resolve
headers from the repository root — the driver with
`include_directories(${CMAKE_CURRENT_SOURCE_DIR})`, core with
`target_include_directories(aes67_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})`.
A file saying `#include "Shared/Types.h"` keeps resolving when `Shared/`
stops being two directories and becomes one. This was deliberate on the
way out — the driver's CMakeLists says so at the `add_subdirectory` — and
it is what makes the way back cheap.

So what actually has to be decided is the build files, the test wiring
and the platform-free gate. The source move is `git mv` and nothing else.

**What does collide**, and is therefore the conflict set to resolve by
hand: `.clang-tidy`, `.claude/`, `.githooks/`, `.gitignore`,
`.gitmodules`, `CMakeLists.txt`, `README.md` and
`scripts/check-tidy.sh`.

## Step 0. Before touching anything

- Push access on `JaumeAP/aes67_macos_driver`. This plan does not need
  write access to `aes67-core` until step 7.
- Record the decision in this repository's `COMPARATIVA-CORE.md`, which
  currently proposes the opposite move. A document that argues for
  something already decided against is worse than no document.
- Freeze `aes67-core`: no new commits on its `main` while the merge is in
  flight, or they are lost work.
- Do all of it on a branch. Every step below is reversible until the
  branch is merged.

## Step 1. Bring the history, not just the files

In the driver:

    git remote add core https://github.com/JaumeAP/aes67-core.git
    git fetch core
    git merge --allow-unrelated-histories core/main

Because core's layout already matches the driver's (`Driver/`,
`NetworkEngine/`, `Shared/`, `Tests/`, `scripts/`), the files land
straight where they belong and, with no path collisions, the only
conflicts are the eight entries listed above.

Do not copy the files in instead. The point of the merge is that `git
blame` and `git log` on a core file keep working afterwards; a copy
throws away the history that the split was careful to preserve.

## Step 2. CMake: keep the target, drop the subdirectory

In the driver's `CMakeLists.txt`:

- Delete `set(AES67_CORE_TESTS OFF CACHE BOOL "" FORCE)` and
  `add_subdirectory(external/aes67-core EXCLUDE_FROM_ALL)` (lines 211 and
  212 today).
- Reinstate `AES67_CORE_SOURCES` as a source list — its own comment says
  that is what it was before the split — and build the same target from
  it: `add_library(aes67_core STATIC ${AES67_CORE_SOURCES})`, with core's
  `target_compile_options` and its `-Wno-unused-parameter
  -Wno-missing-field-initializers`.

Keeping the target named `aes67_core` is the cheap path and also the
right one: `target_link_libraries(aes67_net PUBLIC aes67_core)` (line
247), the plugin's link list (line 276) and `aes67ptpd PRIVATE aes67_core
aes67_net` (line 374) then need no edit at all, and the target boundary
goes on being what separates the two layers now that the directory
boundary is gone.

## Step 3. Tests: one doctest, two suite lists

Both repositories define an INTERFACE library called `doctest_headers` —
the driver in `Tests/CMakeLists.txt:4`, core inline in its root
`CMakeLists.txt`. Today they never meet because core's tests are forced
off. After the merge two definitions of one target is an error.

- Keep the driver's `Tests/CMakeLists.txt` definition; delete core's.
- Move core's `foreach(suite ...)` block — the 19 suites, each
  `add_executable`/`target_link_libraries`/`add_test` with `LABELS "unit"
  TIMEOUT 60` — into `Tests/CMakeLists.txt`.
- Re-point `AES67_FIXTURE_DIR` for `TestInteropSDP`: it is
  `${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures` relative to core today,
  and `Tests/fixtures` becomes one directory shared with the driver's.
- Decide about `Tests/support/NetworkUtilsStub.cpp`. It exists because
  the `StreamConfig` suite reaches the `NetworkUtils` seam that core
  declares and does not implement. After the merge the real
  `NetworkEngine/NetworkUtils.cpp` is in the same project and could be
  linked instead. **Keep the stub.** Linking the real one drags the
  socket layer into a unit suite for no gain, and the stub is what keeps
  that suite runnable without a network.

## Step 4. The platform-free gate: the one real design decision

This is where the merge costs something that is not typing.

`scripts/check-platform-free.sh` selects what to check with

    find Driver NetworkEngine Shared -name '*.cpp' -o -name '*.h' ...

After the merge those three directories hold both layers, and the driver
half legitimately includes `CoreAudio`, `sys/socket` and the rest. Run
unchanged, the gate fails on the first file it was never meant to see.
The repository boundary was answering the question "which files are core"
for free; nothing answers it now.

The fix is an explicit manifest. Two ways, and the second is better:

1. A checked-in `core-manifest.txt` the gate reads instead of `find`.
   Simple, and it silently goes stale the first time somebody adds a file
   to `AES67_CORE_SOURCES` and forgets.
2. Generate the list from CMake — the gate reads `AES67_CORE_SOURCES`
   plus the headers those translation units include, which is what it
   already does transitively. Then there is one list, it is the one the
   build uses, and it cannot drift.

Whichever is chosen, keep the transitive walk. Its own comment records
why: `SimpleRTP.h` opened sockets and `PTPClockSource.h` reached
CoreAudio without either showing up in the `.cpp` files that used them,
and a first pass at the list got five entries wrong for exactly that
reason.

While here, fix what `COMPARATIVA-CORE.md` already found:
`NetworkEngine/PTP/PTPMasterSettings.cpp` includes `<fstream>`, calls
`std::getenv("AES67_PTP_MASTER_CONFIG_PATH")` and `std::getenv("HOME")`,
calls `stat()`, and hardcodes `/Library/Application Support/AES67Driver/`.
It passes today because the gate greps a fixed list of headers and none
of those are on it. After the merge there is no reason left to let it
pass: the persistence belongs on the driver side of the manifest, and the
settings struct stays on the core side.

## Step 5. Submodules and hooks

- `.gitmodules`: drop the `external/aes67-core` entry. `external/doctest`
  exists in both and the driver's stays; that entry is a conflict to
  resolve, not a second submodule.
- `git rm` the `external/aes67-core` gitlink from the index.
- `scripts/check-tidy.sh` exists on both sides and is the eighth
  conflict. Read both before picking; they are not necessarily the same
  script.
- Core's `.githooks/pre-push` runs `scripts/gate.sh`, which runs the
  platform-free check. Fold that into whatever the driver's gate is, or
  it stops running the moment the repository is gone.

## Step 6. Optional: keep the off-Mac build

This is the mitigation for the cost the decision accepts, and it is the
only one. It is not required for the merge to work.

Add a second, small CMake project inside the merged repository that
declares `project(aes67_core LANGUAGES CXX)` — no OBJCXX — and builds
only `AES67_CORE_SOURCES` and the 19 core suites. Then

    cmake -S CoreOnly -B build-core && cmake --build build-core && ctest --test-dir build-core

still works on Linux, and the core layer keeps the sub-second gate it has
today.

Without this step, the record of what is lost is concrete: the PTP work
of 2026-09-03 had to be checked by compiling translation units by hand
(`g++ -fsyntax-only -I. -Iexternal/aes67-core`, with a local shim for
`net/if_dl.h` and a stub for `NetworkUtils::setQoSTrafficClass`, neither
committed). Merged and without step 6, that stops being the exception and
becomes the only way to check anything at all from a machine that is not
a Mac.

## Step 7. What happens to the aes67-core repository

Archive it. Do not delete it.

It is referenced by this repository's `COMPARATIVA-CORE.md` and by the
driver's `HANDOFF.md`, and its history is now inside the driver, so the
repository itself is only a name and a set of links. Leave it read-only
with a README saying where the code went.

## Step 8. Documentation that goes stale the moment this lands

- The driver's `HANDOFF.md`, which describes core as the
  `external/aes67-core` submodule and records the boundary as reviewed
  and deliberately kept (its lines 13 and 25). Both become false.
- The driver's `README.md` and the clone instructions, which say
  `--recurse-submodules` for a submodule that no longer exists.
- Core's own `README.md`, whose "Why it exists" section argues for the
  split. It survives inside the merged tree unless it is dealt with in
  step 1's conflict resolution.
- This repository's `COMPARATIVA-CORE.md`, per step 0.

## How to know it worked, all of it on a Mac

1. `cmake -S . -B build && cmake --build build && ctest --output-on-failure`.
2. **Count the tests before and after.** The number must not go down: 19
   core suites that were not being built from this side plus what the
   driver already ran. A suite silently dropped from a `foreach` that was
   moved by hand is the most likely way this merge goes wrong and the
   least likely to announce itself.
3. `scripts/check-platform-free.sh` passes over the new manifest, **and
   still bites**: add a `#include <CoreAudio/CoreAudio.h>` to a core file
   and confirm it fails. A gate that passes because its file list came
   out empty passes for the wrong reason.
4. The `AES67Driver` bundle and `aes67ptpd` both link.
5. `git log` and `git blame` on a file that came from core still show its
   history.

## Order, and where the risk is

Steps 1 to 3 are mechanical and reversible on a branch. Step 4 is the
only one needing judgement, and it is also the one that quietly stops
protecting anything if it is got wrong — a gate that scans nothing
reports success. Step 6 is optional and is the only thing that answers
the objection this decision overrode.

## Not verified

Nothing here has been run. The driver cannot be configured off a Mac, so
no step of this plan has been tested even partially, and the file and
suite counts are from reading the two trees rather than from building
them.
