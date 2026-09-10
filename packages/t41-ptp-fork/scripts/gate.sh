#!/bin/bash
# The gate for this package: host tests, and the board build when there is a
# PlatformIO to do it with.
#
# Until 2026-09-05 this package had no gate at all inside the monorepo. Its
# 887 host checks passed and nothing ran them, and the workflow that used to
# (.github/workflows/tests.yml) never fired here: GitHub Actions only reads
# workflows from the root of a repository, and this is a package inside one.
# A change here reached the board unverified.
#
#   scripts/gate.sh              host tests
#   scripts/gate.sh --board      host tests and the Teensy build
#   AES67_ANALYSE=1 scripts/gate.sh   with clang-tidy, cppcheck, the
#                                     sanitizers and the parser fuzzer
#
# The board build needs PlatformIO and downloads a toolchain on first use, so
# it is opt-in. Without --board the gate is seconds; the host tests stub out
# Arduino, QNEthernet and TimeLib, which is what lets them run on a Mac at all.
set -uo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

board=0
for arg in "$@"; do
  case "$arg" in
    --board) board=1 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

echo "==> Host tests"
# -Werror here and not in the Makefile's default: the Makefile is what a
# person runs while working, and a warning should not stop them mid-change.
# The gate is the other side of that.
if ! EXTRA_CXXFLAGS="-Werror" make -s -C test; then
  make -s -C test clean > /dev/null 2>&1
  echo "FAIL: host tests" >&2
  exit 1
fi
make -s -C test clean > /dev/null 2>&1

# Static analysis, opt-in like the other packages' (AES67_ANALYSE=1).
if [ "${AES67_ANALYSE:-0}" = "1" ]; then
  echo "==> Static analysis"
  if ! make -s -C test tidy; then
    echo "FAIL: clang-tidy" >&2
    exit 1
  fi
  # A second engine over the same code. It says SKIP where there is no
  # cppcheck rather than failing: this is the analysis half of the gate, and
  # the machine that has neither is not a machine where the code is wrong.
  if ! make -s -C test cppcheck; then
    echo "FAIL: cppcheck" >&2
    exit 1
  fi

  # The same 887 checks again, under AddressSanitizer and
  # UndefinedBehaviorSanitizer. They are here rather than in the plain run
  # because they are minutes rather than seconds, and they catch what neither
  # the compiler nor clang-tidy can: what the code does with the values a test
  # actually feeds it.
  echo "==> Sanitizers"
  make -s -C test clean > /dev/null 2>&1
  if ! EXTRA_CXXFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" make -s -C test; then
    make -s -C test clean > /dev/null 2>&1
    echo "FAIL: sanitizers" >&2
    exit 1
  fi
  make -s -C test clean > /dev/null 2>&1

  # The parser, against inputs nobody wrote. Deterministic and bounded here --
  # the coverage-guided build is CI's, because Apple's clang ships no fuzzer
  # runtime -- so this run takes seconds and finds the same crash twice rather
  # than once every few weeks.
  echo "==> Fuzzing the parser"
  if ! make -s -C test fuzz ITER="${AES67_FUZZ_ITER:-200000}"; then
    make -s -C test clean > /dev/null 2>&1
    echo "FAIL: fuzzing" >&2
    exit 1
  fi
  make -s -C test clean > /dev/null 2>&1
else
  echo "==> Static analysis, sanitizers and fuzzing skipped (AES67_ANALYSE=1 to run them)"
fi

if [ "$board" = "1" ]; then
  if command -v pio > /dev/null 2>&1; then
    echo "==> Board build (Teensy 4.1)"
    if ! make -s -C test board; then
      echo "FAIL: board build" >&2
      exit 1
    fi
  else
    echo "==> Board build skipped: no pio on PATH"
  fi
else
  echo "==> Board build skipped (--board to run it)"
fi

echo "==> PASS"
