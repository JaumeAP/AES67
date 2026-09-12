#!/bin/bash
# Build script for AES67 Controller.
#
# A second application, not a second copy: the parts that are about the
# network rather than about this machine are compiled from the Manager package
# next door (the NMOS client, the session merge, the crosspoint matrix), and
# the parts that are about a driver are absent. What it adds is its own
# discovery, through the C bridge over aes67_net.
#
# Raw swiftc, like the Manager's build.sh and for the same reasons: the app is
# not a SwiftPM target, and a second build system for two windows would be the
# larger decision.

set -e

cd "$(dirname "$0")"

FORCE=0
if [ "$1" = "--force" ]; then
    FORCE=1
fi

MANAGER=../aes67-macos-manager
DRIVER=../aes67-macos-driver
# Where the driver package's libraries are. CMake passes its own binary
# directory; the fallback is that package's build/, which is what its own gate
# fills.
DRIVER_BUILD="${AES67_DRIVER_BUILD_DIR:-$DRIVER/build}"

BINARY="AES67Controller.app/Contents/MacOS/AES67Controller"
if [ "$FORCE" -eq 0 ] && [ -f "$BINARY" ]; then
    CHANGED=$(find . "$MANAGER/Models" "$MANAGER/Views" -name '*.swift' -newer "$BINARY" 2>/dev/null)
    if [ -z "$CHANGED" ]; then
        echo "AES67 Controller: Up to date"
        exit 0
    fi
fi

LIBS=(
  "$DRIVER_BUILD/libaes67_net.a"
  "$DRIVER_BUILD/aes67-ravenna/libaes67_ravenna.a"
  "$DRIVER_BUILD/aes67-core/libaes67_core.a"
  "$DRIVER_BUILD/aes67-core/aes67-profiles/libaes67_profiles.a"
)
for lib in "${LIBS[@]}"; do
  if [ ! -f "$lib" ]; then
    echo "FAIL: $lib is missing — build the driver package first (its libraries carry the" >&2
    echo "      discovery this application runs: cd $DRIVER && cmake -S . -B build && cmake --build build)" >&2
    exit 1
  fi
done

echo "Building AES67 Controller..."

swiftc -o AES67Controller \
  -target arm64-apple-macos13.0 \
  -warnings-as-errors \
  -sdk "$(xcrun --show-sdk-path --sdk macosx)" \
  -import-objc-header Bridging/Controller-Bridging-Header.h \
  -Xcc -I"$DRIVER" -Xcc -I"$DRIVER/../aes67-core" -Xcc -I"$DRIVER/../aes67-profiles" \
  -framework SwiftUI \
  -framework Foundation \
  -framework AppKit \
  -framework Network \
  -framework CoreFoundation \
  -lc++ \
  "${LIBS[@]}" \
  "$MANAGER/Models/NmosResources.swift" \
  "$MANAGER/Models/NmosController.swift" \
  "$MANAGER/Models/SessionList.swift" \
  "$MANAGER/Views/RoutingMatrixView.swift" \
  Models/DiscoveryService.swift \
  Views/ControllerWindow.swift \
  AES67ControllerApp.swift

echo "Creating app bundle..."
mkdir -p AES67Controller.app/Contents/MacOS
mkdir -p AES67Controller.app/Contents/Resources
mv AES67Controller AES67Controller.app/Contents/MacOS/
cp Resources/Info.plist AES67Controller.app/Contents/

echo "Signing app..."
codesign --force --sign - AES67Controller.app

echo "Build complete: AES67Controller.app"
