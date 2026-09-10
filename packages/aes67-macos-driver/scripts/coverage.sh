#!/bin/bash
# Source-based coverage report for the test suite, using the llvm-cov that
# ships with the Command Line Tools -- no extra install.
#
#   scripts/coverage.sh            build, run every suite, report
#   scripts/coverage.sh --quick    skip the `timing` label
#
# Builds in build-cov/ so the normal build tree keeps its optimised binaries
# and stays free of .profraw files.
set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

build_dir="build-cov"
prof_dir="$build_dir/profraw"

# Every suite, timing included. They are excluded from the ordinary gate
# because they are slow and wall-clock dependent, not because they are
# unreliable here -- and excluding them from COVERAGE means the PTP code they
# are the only exercise of reads as untested. It did: PTPClock.cpp came out at
# 10% and PTPService.cpp at 0% while both had suites.
#
# --quick drops them again, for a run that only has to be fast.
#
# Seeded with --output-on-failure so the array is never empty: bash 3.2, which
# is what /bin/bash is on macOS, treats "${arr[@]}" on an empty array as an
# unbound variable under `set -u` and aborts the subshell -- which here meant
# ctest never ran and the profiles that were never written were reported as an
# uninstrumented build. scripts/gate.sh carries the same note for the same
# reason.
ctest_filter=(--output-on-failure)
if [ "${1:-}" = "--quick" ]; then
  ctest_filter+=(-LE timing)
fi

echo "==> Configure (coverage, no Manager app)"
cmake -S . -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DAES67_COVERAGE=ON \
  -DBUILD_TESTS=ON \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_TOOLS=OFF \
  || { echo "FAIL: cmake configure" >&2; exit 1; }

echo "==> Build"
cmake --build "$build_dir" -j"$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)" \
  || { echo "FAIL: build" >&2; exit 1; }

rm -rf "$prof_dir"; mkdir -p "$prof_dir"

echo "==> Run tests"
# Absolute path, and %p for one profile per process. Absolute because ctest
# runs each test with its own build directory as the working directory, so a
# relative LLVM_PROFILE_FILE scatters the profiles next to each binary instead
# of collecting them; %p because every suite is its own process and a fixed
# name would leave only whichever ran last.
( cd "$build_dir" && LLVM_PROFILE_FILE="$repo_root/$prof_dir/%p.profraw" ctest "${ctest_filter[@]}" )
test_status=$?

echo "==> Merge profiles"
profraw_files=$(find "$prof_dir" -name '*.profraw')
if [ -z "$profraw_files" ]; then
  echo "FAIL: no .profraw produced - was the build instrumented?" >&2
  exit 1
fi
# shellcheck disable=SC2086
xcrun llvm-profdata merge -sparse $profraw_files -o "$build_dir/coverage.profdata" \
  || { echo "FAIL: llvm-profdata merge" >&2; exit 1; }

echo "==> Report (project sources only)"
binaries=$(find "$build_dir/Tests" -type f -perm +111 ! -name '*.dSYM' -maxdepth 1)
objects=()
for b in $binaries; do objects+=(-object "$b"); done
# -arch is not optional here: the test binaries are universal (arm64 +
# x86_64) and llvm-cov refuses to load one without being told which slice to
# read. Use whatever this machine runs natively, which is the slice the tests
# actually executed.
arch="$(uname -m)"

xcrun llvm-cov report "${objects[@]}" \
  -arch "$arch" \
  -instr-profile="$build_dir/coverage.profdata" \
  `# Other packages' sources are measured by their own gates -- aes67-core,` \
  `# aes67-profiles and aes67-ravenna each report their own number. Counting` \
  `# them here charged this package for code its suites are not the exercise` \
  `# of: aes67-ravenna reads 85.8% in its own gate and 0% in this report.` \
  -ignore-filename-regex='(external|vendor|Tests)/|packages/aes67-(core|profiles|ravenna)/' \
  || { echo "FAIL: llvm-cov report" >&2; exit 1; }

echo
echo "Per-file HTML: xcrun llvm-cov show ${objects[*]} -arch $arch -instr-profile=$build_dir/coverage.profdata -format=html -o $build_dir/coverage-html"
[ $test_status -eq 0 ] || echo "NOTE: some tests failed; coverage above reflects that run."
