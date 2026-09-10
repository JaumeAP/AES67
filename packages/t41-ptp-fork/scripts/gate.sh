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
#   scripts/gate.sh           host tests
#   scripts/gate.sh --board   host tests and the Teensy build
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
else
  echo "==> Static analysis skipped (AES67_ANALYSE=1 to run it)"
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
