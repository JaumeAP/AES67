#!/bin/bash
# The gate for this package.
#
# On Linux it builds everything, the daemon included, and runs the wire suite.
# On anything else it builds the wire library and its tests and says what it
# skipped: the daemon is Linux by construction, and pretending otherwise would
# mean a gate that passes without having compiled the half that talks to the
# kernel.
set -uo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "==> Configure"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null || {
    echo "FAIL: configure" >&2; exit 1; }

echo "==> Build"
cmake --build build -j > /dev/null || { echo "FAIL: build" >&2; exit 1; }

echo "==> Tests"
ctest --test-dir build --output-on-failure -R PtpWire || {
    echo "FAIL: tests" >&2; exit 1; }

if [[ "$(uname -s)" == "Linux" ]]; then
    if [[ -x build/aes67-ptpd ]]; then
        echo "==> Daemon built"
    else
        echo "FAIL: no daemon binary on Linux" >&2; exit 1
    fi
else
    echo "==> Not Linux: the daemon was not built"
fi

echo "==> PASS"
