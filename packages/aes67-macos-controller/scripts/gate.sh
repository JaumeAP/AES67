#!/bin/bash
# The gate for this package: the application builds, and what it builds is an
# application.
#
# There are no host tests here. Everything this package could test on its own
# is tested where it lives: the session merge and the NMOS decoding in
# aes67-macos-manager's run-tests.sh, the discovery behind the bridge in
# aes67-macos-driver's suites (SessionDirectory, RTSPSessionDiscovery,
# DiscoveryBridge). What is left in this package is an app entry point, a
# window and a Swift wrapper over a C API, and a test of those would be a test
# of SwiftUI.
#
#   scripts/gate.sh          build the app
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

mkdir -p build

echo "==> App"
if ! ./build.sh --force > build/build.log 2>&1; then
  tail -20 build/build.log >&2
  echo "FAIL: build.sh" >&2
  exit 1
fi

test -x AES67Controller.app/Contents/MacOS/AES67Controller || {
  echo "FAIL: no executable in the bundle" >&2
  exit 1
}

# The bundle has to carry the local-network usage description and the Bonjour
# service types, or macOS silently gives it no multicast and no browsing --
# the two things this application is for.
for key in NSLocalNetworkUsageDescription NSBonjourServices; do
  grep -q "$key" AES67Controller.app/Contents/Info.plist || {
    echo "FAIL: Info.plist has no $key" >&2
    exit 1
  }
done

echo "==> PASS"
