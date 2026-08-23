#!/bin/bash
# Build script for AES67 Manager App

set -e

cd "$(dirname "$0")"

FORCE=0
if [ "$1" = "--force" ]; then
    FORCE=1
fi

# Incremental build: skip if binary is newer than all Swift sources
BINARY="AES67Manager.app/Contents/MacOS/AES67Manager"
if [ "$FORCE" -eq 0 ] && [ -f "$BINARY" ]; then
    CHANGED=$(find . -name '*.swift' -newer "$BINARY" 2>/dev/null)
    if [ -z "$CHANGED" ]; then
        echo "AES67 Manager: Up to date"
        exit 0
    fi
fi

echo "Building AES67 Manager..."

# Compile Swift files - include all Views and Models
# Note: SDPProcessor.swift disabled - needs compatibility fixes
swiftc -o AES67Manager \
  -target arm64-apple-macos13.0 \
  -sdk "$(xcrun --show-sdk-path --sdk macosx)" \
  -framework SwiftUI \
  -framework Foundation \
  -framework AppKit \
  -framework CoreAudio \
  -framework UniformTypeIdentifiers \
  Models/StreamInfo.swift \
  Models/DriverManager.swift \
  Models/MenuBarManager.swift \
  Views/ContentView.swift \
  Views/StreamListView.swift \
  Views/StreamDetailView.swift \
  Views/AddStreamView.swift \
  Views/SettingsView.swift \
  Views/ChannelMappingView.swift \
  Views/ChannelMapDiagnosticView.swift \
  Views/QuickStartView.swift \
  Views/PTPDiagnosticView.swift \
  Views/ProfileParametersView.swift \
  Views/DiscoveredSessionsView.swift \
  Views/AudioStatusView.swift \
  AES67ManagerApp.swift

# Create app bundle structure
echo "Creating app bundle..."
mkdir -p AES67Manager.app/Contents/MacOS
mkdir -p AES67Manager.app/Contents/Resources

# Move executable
mv AES67Manager AES67Manager.app/Contents/MacOS/

# Copy Info.plist
cp Resources/Info.plist AES67Manager.app/Contents/

# Embed the driver bundle so this app can install/remove itself via the main
# window's switch (see DriverManager.installDriver/uninstallDriver) without
# depending on the .pkg installer having run first. Sourced from the sibling
# CMake build/ dir — not fatal if it's missing (e.g. AES67Driver wasn't
# built, or libASPL isn't available): the app still builds, the switch just
# can't turn on until rebuilt with the driver present.
DRIVER_BUNDLE="../build/AES67Driver.driver"
if [ -d "$DRIVER_BUNDLE" ]; then
    echo "Embedding AES67Driver.driver..."
    rm -rf "AES67Manager.app/Contents/Resources/AES67Driver.driver"
    ditto "$DRIVER_BUNDLE" "AES67Manager.app/Contents/Resources/AES67Driver.driver"
else
    echo "WARNING: $DRIVER_BUNDLE not found — building without an embedded driver."
    echo "         Build AES67Driver first (needs libASPL), then rebuild this app, so the install switch has something to install."
fi

# Sign the app
echo "Signing app..."
codesign --force --deep --sign - --options runtime AES67Manager.app

echo "Build complete: AES67Manager.app"
