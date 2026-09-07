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
if grep -rn '#include <\(CoreAudio\|CoreFoundation\|AudioToolbox\|Accelerate\|mach\|sys/socket\|netinet\|arpa\|ifaddrs\|Arduino\|QNEthernet\|TimeLib\)' Profiles/ 2>/dev/null; then
    echo "FAIL: a platform header in a package that has no platform" >&2
    exit 1
fi
if grep -rn '#include "\(\.\./\|Driver/\|NetworkEngine/\|Shared/\)' Profiles/ 2>/dev/null; then
    echo "FAIL: a header from another package" >&2
    exit 1
fi
echo "$(ls Profiles/*.h Profiles/*.cpp | wc -l | tr -d ' ') files, self-contained"

# Static analysis, opt-in like the other packages: it is slow and it wants a
# clang-tidy the Command Line Tools do not ship. AES67_ANALYSE=1 turns it on.
if [ "${AES67_ANALYSE:-0}" = "1" ]; then
    echo "==> Static analysis"
    scripts/check-tidy.sh build || { echo "FAIL: clang-tidy" >&2; exit 1; }
else
    echo "==> Static analysis skipped (AES67_ANALYSE=1 to run it)"
fi

echo "==> PASS"
