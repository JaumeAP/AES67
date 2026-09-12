#!/bin/bash
# Build script for AES67 Uninstall.
#
# The third application in the image. It removes what the Manager installs,
# and it exists so that removing does not require opening the thing being
# removed -- or having it still there at all.
#
# Raw swiftc, like the other two. It carries no resources: everything it
# knows is three paths and a launchd label.

set -e

cd "$(dirname "$0")"

FORCE=0
if [ "$1" = "--force" ]; then
    FORCE=1
fi

BINARY="AES67Uninstall.app/Contents/MacOS/AES67Uninstall"
if [ "$FORCE" -eq 0 ] && [ -f "$BINARY" ]; then
    CHANGED=$(find Uninstaller Models/PrivilegedScript.swift -name '*.swift' -newer "$BINARY" 2>/dev/null)
    if [ -z "$CHANGED" ]; then
        echo "AES67 Uninstall: Up to date"
        exit 0
    fi
fi

echo "Building AES67 Uninstall..."

swiftc -o AES67Uninstall \
  -target arm64-apple-macos13.0 \
  -warnings-as-errors \
  -sdk "$(xcrun --show-sdk-path --sdk macosx)" \
  -framework SwiftUI \
  -framework Foundation \
  -framework AppKit \
  Models/PrivilegedScript.swift \
  Uninstaller/UninstallPlan.swift \
  Uninstaller/UninstallView.swift \
  Uninstaller/AES67UninstallApp.swift

echo "Creating app bundle..."
mkdir -p AES67Uninstall.app/Contents/MacOS
mkdir -p AES67Uninstall.app/Contents/Resources
mv AES67Uninstall AES67Uninstall.app/Contents/MacOS/
cp Resources/UninstallInfo.plist AES67Uninstall.app/Contents/Info.plist

echo "Signing app..."
codesign --force --sign - AES67Uninstall.app

echo "Build complete: AES67Uninstall.app"
