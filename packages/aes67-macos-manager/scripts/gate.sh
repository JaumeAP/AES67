#!/bin/bash
# The gate for this package: the app's host tests, and the app itself when
# there is a Swift toolchain to build it with.
#
# The driver's artifacts are not a precondition. build.sh embeds
# AES67Driver.driver and aes67ptpd when the driver package's build/ has them
# and says what it could not find when it does not; a gate that demanded them
# would be a gate for two packages. The tree gate runs the driver's first, so
# there they are present.
#
#   scripts/gate.sh          host tests, then the app
set -uo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "$(uname -s)" != "Darwin" ]; then
  echo "==> Not macOS: nothing here builds"
  echo "==> PASS"
  exit 0
fi

if ! command -v swiftc > /dev/null 2>&1; then
  echo "FAIL: no swiftc on PATH" >&2
  exit 1
fi

echo "==> Host tests"
if ! AES67_BUILD_DIR=build ./run-tests.sh; then
  echo "FAIL: host tests" >&2
  exit 1
fi

echo "==> App"
if ! ./build.sh --force > build/build.log 2>&1; then
  tail -20 build/build.log >&2
  echo "FAIL: build.sh" >&2
  exit 1
fi
grep "^WARNING" build/build.log || true
test -x AES67Manager.app/Contents/MacOS/AES67Manager || {
  echo "FAIL: no app bundle produced" >&2; exit 1; }

echo "==> PASS"
