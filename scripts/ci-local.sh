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
if [ "${1:-}" = "--clean" ]; then
  echo "==> Removing $build_dir (clean run)"
  rm -rf "$build_dir"
fi

# The workflow installed cmake but never libaspl, which is why it failed on
# every run: the driver target is gated on APPLE, so it builds here and needs
# the framework. Fail early with the fix rather than deep inside cmake.
if ! command -v cmake >/dev/null 2>&1; then
  echo "FAIL: cmake not found. brew install cmake" >&2
  exit 1
fi
if [ "$(uname -s)" = "Darwin" ] && ! brew list libaspl >/dev/null 2>&1; then
  echo "FAIL: libaspl not installed. brew tap gavv/gavv && brew install libaspl" >&2
  exit 1
fi

jobs="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"

echo "==> Configure (Release, tests only)"
mkdir -p "$build_dir"
cmake -S . -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_TOOLS=OFF || { echo "FAIL: cmake configure" >&2; exit 1; }

echo "==> Build (-j$jobs)"
cmake --build "$build_dir" -j"$jobs" || { echo "FAIL: build" >&2; exit 1; }

echo "==> Tests (network-dependent suites excluded, as in CI)"
( cd "$build_dir" && ctest -E "RingBuffer|PTPClock|IntegrationAudioPath" --output-on-failure ) \
  || { echo "FAIL: tests" >&2; exit 1; }

# Both checks below are informational: the workflow's lint job never failed on
# them either, it only printed what it found.
echo "==> Active TODOs"
grep -rn "TODO" --include="*.cpp" --include="*.h" --include="*.hpp" --include="*.swift" . \
  | grep -v vendor | grep -v "\.git" || echo "No TODOs found"

echo "==> Dead-code references in build files"
grep -rn "CircularJitterBuffer\|JitterBuffer\|TemporalJitterBuffer\|SimplifiedLockFreePacketPool" \
  CMakeLists.txt Tests/CMakeLists.txt || echo "No dead code references found"

echo "==> PASS"
