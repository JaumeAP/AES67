#!/bin/bash
# Build script for AES67 Install.
#
# The program that puts the other two on the machine: one window, a tick per
# application, and a button that makes the machine match the ticks. It carries
# both applications inside its own bundle, so the image holds one thing to
# open rather than two things to drag.
#
# Raw swiftc, like everything else here.

set -e

cd "$(dirname "$0")"

FORCE=0
if [ "$1" = "--force" ]; then
    FORCE=1
fi

CONTROLLER_APP=../aes67-macos-controller/AES67Controller.app

BINARY="AES67Install.app/Contents/MacOS/AES67Install"
if [ "$FORCE" -eq 0 ] && [ -f "$BINARY" ]; then
    CHANGED=$(find Installer Uninstaller Models/PrivilegedScript.swift -name '*.swift' -newer "$BINARY" 2>/dev/null)
    if [ -z "$CHANGED" ]; then
        echo "AES67 Install: Up to date"
        exit 0
    fi
fi

echo "Building AES67 Install..."

swiftc -o AES67Install \
  -target arm64-apple-macos13.0 \
  -warnings-as-errors \
  -sdk "$(xcrun --show-sdk-path --sdk macosx)" \
  -framework SwiftUI \
  -framework Foundation \
  -framework AppKit \
  Models/PrivilegedScript.swift \
  Uninstaller/UninstallPlan.swift \
  Installer/InstallPlan.swift \
  Installer/InstallerView.swift \
  Installer/AES67InstallApp.swift

echo "Creating app bundle..."
mkdir -p AES67Install.app/Contents/MacOS
mkdir -p AES67Install.app/Contents/Resources
mv AES67Install AES67Install.app/Contents/MacOS/
cp Resources/InstallInfo.plist AES67Install.app/Contents/Info.plist

# What it installs, carried inside it. Missing is a warning and not a failure:
# the installer still builds, and says which tick will not be able to do
# anything.
for app in AES67Manager.app "$CONTROLLER_APP"; do
    name="$(basename "$app")"
    if [ -d "$app" ]; then
        rm -rf "AES67Install.app/Contents/Resources/$name"
        ditto "$app" "AES67Install.app/Contents/Resources/$name"
    else
        echo "WARNING: $app not found — the installer will carry no $name."
    fi
done


# The languages, the way macOS reads them: one .lproj per language holding
# Localizable.strings, which is what SwiftUI resolves every Text("...") key
# against. Shared with the uninstaller: one window's worth of words each.
for lproj in Localization/Installer/*.lproj; do
    [ -d "$lproj" ] || continue
    ditto "$lproj" "AES67Install.app/Contents/Resources/$(basename "$lproj")"
done

echo "Signing app..."
codesign --force --sign - AES67Install.app

echo "Build complete: AES67Install.app"
