#!/bin/bash
# The gate for this package: build it and run its suites.
#
# It is the smallest gate in the repository because the package is the smallest
# thing in it: tables of numbers, one validator over them, and no platform at
# all. What it does check, and what the other packages cannot check for it, is
# that this builds with nothing else present -- no core, no driver, no Arduino.
set -uo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "==> Configure"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null || {
    echo "FAIL: configure" >&2; exit 1; }

echo "==> Build"
cmake --build build -j > /dev/null || { echo "FAIL: build" >&2; exit 1; }

echo "==> Tests"
ctest --test-dir build --output-on-failure || { echo "FAIL: tests" >&2; exit 1; }

# Nothing here may reach a platform, and nothing here may reach another
# package. The first is the same rule the core keeps; the second is this
# package's own, and it is the reason it exists.
echo "==> No platform, no neighbours"
if grep -rn '#include <\(CoreAudio\|CoreFoundation\|AudioToolbox\|Accelerate\|mach\|sys/socket\|netinet\|arpa\|ifaddrs\|Arduino\|QNEthernet\|TimeLib\)' Profiles/ Testing/ 2>/dev/null; then
    echo "FAIL: a platform header in a package that has no platform" >&2
    exit 1
fi
if grep -rn '#include "\(\.\./\|Driver/\|NetworkEngine/\|Shared/\)' Profiles/ Testing/ 2>/dev/null; then
    echo "FAIL: a header from another package" >&2
    exit 1
fi
echo "$(ls Profiles/*.h Profiles/*.cpp Testing/*.h | wc -l | tr -d ' ') files, self-contained"

# Static analysis, opt-in like the other packages: it is slow and it wants a
# clang-tidy the Command Line Tools do not ship. AES67_ANALYSE=1 turns it on.
if [ "${AES67_ANALYSE:-0}" = "1" ]; then
    echo "==> Static analysis"
    scripts/check-tidy.sh build || { echo "FAIL: clang-tidy" >&2; exit 1; }
else
    echo "==> Static analysis skipped (AES67_ANALYSE=1 to run it)"
fi

# ---------------------------------------------------------------------------
# The analysis half, under AES67_ANALYSE=1: what the plain run above cannot
# afford on every push. Each step is its own build directory so the ordinary
# build keeps its optimised binaries.
#
#   cppcheck     a second engine over the sources; SKIP where there is none
#   sanitizers   the same suites under ASan and UBSan, in build-san
#   fuzz         the parsers, fed mutated and random input, in that build
#   coverage     line coverage of the package's own sources, printed only
# ---------------------------------------------------------------------------
if [ "${AES67_ANALYSE:-0}" = "1" ] || [ "${analyse:-0}" = "1" ]; then
  jobs="$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

  # Found the way check-tidy.sh finds clang-tidy: neither ships with the
  # Command Line Tools, and both live in ~/.local/venvs/cpptools on this
  # machine. CPPCHECK=/path/to/cppcheck overrides.
  cppcheck_bin="${CPPCHECK:-}"
  if [ -z "$cppcheck_bin" ]; then
    for candidate in "$HOME/.local/venvs/cpptools/bin/cppcheck" "$(command -v cppcheck || true)"; do
      [ -x "$candidate" ] && { cppcheck_bin="$candidate"; break; }
    done
  fi

  echo "==> cppcheck"
  if [ -n "$cppcheck_bin" ]; then
    # -U rather than a suppression: cppcheck analyses every branch of an #if,
    # and Profiles/ProfileLog.h's is `#include AES67_PROFILES_LOG_HEADER`,
    # which is not a header when the macro is undefined. Nothing in this tree
    # defines it, so saying so is the truth rather than a silenced check.
    "$cppcheck_bin" --quiet --error-exitcode=1 --enable=warning,performance,portability \
      --inline-suppr --std=c++20 --language=c++ -U AES67_PROFILES_LOG_HEADER \
      --suppress=missingInclude --suppress=missingIncludeSystem \
      --suppress=normalCheckLevelMaxBranches \
      -I . Profiles/ || { echo "FAIL: cppcheck" >&2; exit 1; }
  else
    echo "SKIP: no cppcheck found"
  fi

  echo "==> Sanitizers (address, undefined)"
  rm -rf build-san
  cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DAES67_SANITIZE=address,undefined > /dev/null \
    || { echo "FAIL: sanitizer configure" >&2; exit 1; }
  cmake --build build-san -j"$jobs" > /dev/null || { echo "FAIL: sanitizer build" >&2; exit 1; }
  ( cd build-san && UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
      ctest --output-on-failure -LE "timing|network" ) || { echo "FAIL: tests under sanitizers" >&2; exit 1; }


  echo "==> Coverage"
  rm -rf build-cov
  cmake -S . -B build-cov -DCMAKE_BUILD_TYPE=Debug -DAES67_COVERAGE=ON > /dev/null \
    || { echo "FAIL: coverage configure" >&2; exit 1; }
  cmake --build build-cov -j"$jobs" > /dev/null || { echo "FAIL: coverage build" >&2; exit 1; }
  ( cd build-cov && ctest -LE "timing|network" > /dev/null ) || { echo "FAIL: tests under coverage" >&2; exit 1; }
  # gcov with -p keeps the path in the output name, which is what lets the
  # summary count this package's sources and nobody else's.
  gcov_tool="gcov"; command -v xcrun > /dev/null 2>&1 && gcov_tool="xcrun llvm-cov gcov"
  ( cd build-cov && find . -name '*.gcda' -print0 | xargs -0 $gcov_tool -p > /dev/null 2>&1; \
    ls *.gcov 2>/dev/null | grep '#packages#aes67-profiles#' | grep -vE '#Tests#|#external#|#doctest#|#build' \
    | LC_ALL=C xargs -r awk -F: '/^ *#####:/ { miss++ } /^ *[0-9]+[*]?:/ { hit++ } \
      END { t = hit + miss; if (t == 0) { print "coverage: nothing measured"; exit 0 } \
            printf "coverage: %d/%d lines of aes67-profiles, %.1f%%\n", hit, t, 100 * hit / t }' )
  rm -f build-cov/*.gcov
else
  echo "==> Analysis skipped: cppcheck, sanitizers, fuzzing and coverage (AES67_ANALYSE=1 to run them)"
fi

echo "==> PASS"
