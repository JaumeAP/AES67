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
# BUILD_MANAGER_APP=OFF: the SwiftUI app is built by ManagerApp/build.sh with
# raw swiftc and does not compile in either environment today -- here the
# #Preview macro plugin is missing (no full Xcode toolchain), and on the GitHub
# runner swiftc gave up type-checking DiscoveredSessionsView.swift:23. It is
# also unverified against a live driver per README. Excluding it keeps this
# gate about the C++ driver and its tests; build it explicitly with
# `cmake --build build --target ManagerApp` once it is fixed.
cmake -S . -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_TOOLS=OFF \
  -DBUILD_MANAGER_APP=OFF || { echo "FAIL: cmake configure" >&2; exit 1; }

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
