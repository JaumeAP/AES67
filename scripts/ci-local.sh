#!/bin/bash
# Local replacement for the GitHub Actions "Build and Test" workflow, which
# was disabled on 2026-08-25 and deleted from the repo: every build now runs
# on this machine. The steps mirror what the workflow ran, so a green run
# here means what a green run there used to mean.
#
# Usage:
#   scripts/ci-local.sh            incremental build in build/
#   scripts/ci-local.sh --clean    wipe build/ first, like a fresh runner
#
# Invoked automatically by .githooks/pre-push. Set SKIP_LOCAL_CI=1, or push
# with --no-verify, to skip it for one push.
set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

build_dir="build"
sanitize=""
for arg in "$@"; do
  case "$arg" in
    --clean)
      echo "==> Removing $build_dir (clean run)"
      rm -rf "$build_dir"
      ;;
    --sanitize)
      # Its own build dir and its own flags: sanitizer builds are slower and
      # link differently, so sharing build/ would force a full rebuild every
      # time you switch back. Not part of the pre-push gate -- a push should
      # not wait for an instrumented rebuild -- run it deliberately before
      # anything that touches the ring buffer or the IO path.
      build_dir="build-san"
      sanitize="address,undefined"
      ;;
    --tsan)
      # ThreadSanitizer is the one that has a chance at the SPSC ring buffer
      # and the IO/network thread boundary. It cannot prove the lock-free code
      # correct, only catch a race it actually observes, so pair it with
      # `ctest -L timing`, which is where the concurrent cases live.
      build_dir="build-tsan"
      sanitize="thread"
      ;;
  esac
done

if ! command -v cmake >/dev/null 2>&1; then
  echo "FAIL: cmake not found" >&2
  exit 1
fi

# libASPL: the submodule under external/ is the primary source and CMake
# prefers it over any system copy, so this is informational only. When neither
# is present CMake warns and skips the driver and examples rather than failing,
# which is a legitimate tests-only run.
if [ -f "external/libASPL/include/aspl/Device.hpp" ]; then
  echo "==> libASPL: submodule"
elif [ -f /usr/local/include/aspl/Plugin.hpp ] || [ -f /opt/homebrew/include/aspl/Plugin.hpp ]; then
  echo "==> libASPL: system"
else
  echo "==> libASPL: absent - driver and examples will be skipped"
fi

jobs="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"

echo "==> Configure (Release, tests only)"
mkdir -p "$build_dir"
# ManagerApp is back in the gate: it built with the Command Line Tools once
# the #Preview blocks moved to Views/Previews/ (the macro plugin they need
# ships with full Xcode only, and build.sh lists its sources explicitly, so
# that directory stays out of a command-line build) and once
# DiscoveredSessionsView's session-already-added check was rewritten. Skip it
# with -DBUILD_MANAGER_APP=OFF if a machine lacks a Swift toolchain.
cmake -S . -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DAES67_SANITIZE="$sanitize" \
  -DBUILD_TESTS=ON \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_TOOLS=OFF \
  -DBUILD_MANAGER_APP=ON || { echo "FAIL: cmake configure" >&2; exit 1; }

echo "==> Build (-j$jobs)"
cmake --build "$build_dir" -j"$jobs" || { echo "FAIL: build" >&2; exit 1; }

# -LE timing, not a name regex: the suites that depend on wall-clock behaviour
# or on real multicast sockets carry the `timing` label (Tests/CMakeLists.txt),
# so labelling a new one excludes it here automatically. Run them explicitly
# with `ctest -L timing` when you want them.
# Under a sanitizer the timing suites are the point, not noise to skip: the
# concurrent ring-buffer case is what TSan needs to see. Their wall-clock
# assertions do get slower under instrumentation, so a speed-ratio failure
# there means "instrumented", not "regressed".
# Seeded with --output-on-failure so the array is never empty: bash 3.2, which
# is what /bin/bash is on macOS, treats "${arr[@]}" on an empty array as an
# unbound variable under `set -u` and aborts.
ctest_args=(--output-on-failure)
if [ -n "$sanitize" ]; then
  # Timing and concurrency suites are the point under a sanitizer, but the
  # `network` ones need real multicast sockets and fail on a developer machine
  # for reasons that have nothing to do with the instrumentation.
  ctest_args+=(-LE network)
  echo "==> Tests (network excluded, sanitizer: $sanitize)"
else
  ctest_args+=(-LE timing)
  echo "==> Tests (label 'timing' excluded)"
fi
( cd "$build_dir" && ctest "${ctest_args[@]}" ) \
  || { echo "FAIL: tests" >&2; exit 1; }

# The core is its own repository now, and it carries its own check. Running it
# from here rather than reimplementing it: this build is a consumer of that
# library, and a consumer that lets a platform header into it would find out at
# somebody else's build.
echo "==> Core stays platform-free"
if [ -x external/aes67-core/scripts/check-platform-free.sh ]; then
  external/aes67-core/scripts/check-platform-free.sh || { echo "FAIL: core contract" >&2; exit 1; }
else
  echo "FAIL: external/aes67-core missing - run: git submodule update --init --recursive" >&2
  exit 1
fi

# Both checks below are informational: the workflow's lint job never failed on
# them either, it only printed what it found.
echo "==> Active TODOs"
grep -rn "TODO" --include="*.cpp" --include="*.h" --include="*.hpp" --include="*.swift" . \
  | grep -v vendor | grep -v "\.git" | grep -v "^\./external/" || echo "No TODOs found"

echo "==> Dead-code references in build files"
grep -rn "CircularJitterBuffer\|JitterBuffer\|TemporalJitterBuffer\|SimplifiedLockFreePacketPool" \
  CMakeLists.txt Tests/CMakeLists.txt || echo "No dead code references found"

echo "==> PASS"
