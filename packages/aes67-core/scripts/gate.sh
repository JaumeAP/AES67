#!/bin/bash
# Everything this repository checks, in one command.
#
#   scripts/gate.sh            build, test, contract, static analysis
#   scripts/gate.sh --clean    wipe build/ first
#
# Run by .githooks/pre-push. Until this existed, the three checks here only ran
# when somebody remembered to type them -- and the consumer that did run one of
# them (aes67-macos-driver calls check-platform-free.sh from its own gate) was
# the only automatic coverage this library had. A base other projects build on
# should not depend on a consumer for that.
set -uo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

build_dir="build"
analyse=0
for arg in "$@"; do
  case "$arg" in
    --clean)
      echo "==> Removing $build_dir"
      rm -rf "$build_dir"
      ;;
    --analyse|--analyze)
      analyse=1
      ;;
  esac
done

# doctest lives once, at the root of the monorepo, and is shared with the
# driver package. This path is what the CMakeLists falls back to when the root
# has not set AES67_DOCTEST_DIR, which is the case when this package is built
# on its own -- as it is here.
doctest_dir="../../external/doctest/doctest"
if [ ! -f "$doctest_dir/doctest.h" ]; then
  echo "FAIL: $doctest_dir is empty - run: git submodule update --init --recursive" >&2
  exit 1
fi

echo "==> Configure"
cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > /dev/null \
  || { echo "FAIL: cmake configure" >&2; exit 1; }

echo "==> Build"
cmake --build "$build_dir" -j"$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4)" \
  || { echo "FAIL: build" >&2; exit 1; }

echo "==> Tests"
( cd "$build_dir" && ctest --output-on-failure ) || { echo "FAIL: tests" >&2; exit 1; }

echo "==> Platform-free contract"
scripts/check-platform-free.sh || { echo "FAIL: platform contract" >&2; exit 1; }

# Static analysis, and only when asked for. It costs minutes where the tests
# cost a second, and it is not where regressions appear: clang-tidy finds
# latent defects, not something that broke between two commits. Paying for it
# on every push taxes the wrong moment.
#
# AES67_ANALYSE=1, or scripts/gate.sh --analyse. The monorepo's
# scripts/gate.sh passes the variable through, and .githooks/pre-push sets it
# when the push is going to the default branch, after asking.
if [ "${AES67_ANALYSE:-0}" = "1" ] || [ "$analyse" = "1" ]; then
  echo "==> Static analysis"
  ../../scripts/check-tidy.sh "$build_dir" || { echo "FAIL: clang-tidy" >&2; exit 1; }
else
  echo "==> Static analysis skipped (AES67_ANALYSE=1 or --analyse to run it)"
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
      -I . -I ../aes67-profiles -I ../t41-ptp/src Driver/ NetworkEngine/ Shared/ || { echo "FAIL: cppcheck" >&2; exit 1; }
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

  echo "==> Fuzzing the parsers"
  build-san/FuzzCoreParsers "${AES67_FUZZ_ITER:-200000}" || { echo "FAIL: fuzzing" >&2; exit 1; }


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
    ls *.gcov 2>/dev/null | grep '#packages#aes67-core#' | grep -vE '#Tests#|#external#|#doctest#|#build' \
    | LC_ALL=C xargs -r awk -F: '/^ *#####:/ { miss++ } /^ *[0-9]+[*]?:/ { hit++ } \
      END { t = hit + miss; if (t == 0) { print "coverage: nothing measured"; exit 0 } \
            printf "coverage: %d/%d lines of aes67-core, %.1f%%\n", hit, t, 100 * hit / t }' )
  rm -f build-cov/*.gcov
else
  echo "==> Analysis skipped: cppcheck, sanitizers, fuzzing and coverage (AES67_ANALYSE=1 to run them)"
fi

echo "==> PASS"
