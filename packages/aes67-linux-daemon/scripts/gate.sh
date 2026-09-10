#!/bin/bash
# The gate for this package.
#
# It builds aes67-profile-conf and runs its suites, everywhere: the tools read
# the profiles, the core and a text file, and nothing here needs Linux or the
# daemon's source.
set -uo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "==> Configure"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null || {
    echo "FAIL: configure" >&2; exit 1; }

echo "==> Build"
cmake --build build -j > /dev/null || { echo "FAIL: build" >&2; exit 1; }

echo "==> Tests"
ctest --test-dir build --output-on-failure -R "ProfileConf|ConfCheck|SourceGen|DaemonSdp" || {
    echo "FAIL: tests" >&2; exit 1; }

echo "==> PASS"
