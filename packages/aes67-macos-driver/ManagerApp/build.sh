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
  Models/PrivilegedScript.swift \
  Models/DolbyModelCatalog.swift \
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

# Embed what the app installs: the driver bundle, the PTP daemon and the
# daemon's LaunchDaemon plist. This app is how they reach the system — see
# DriverManager.installDriver/uninstallDriver — so anything missing here is
# something the Install button cannot put down.
#
# The build directory comes from CMake (AES67_BUILD_DIR); the fallback is what
# a standalone build of this package uses. Missing artifacts are a warning,
# not an error: the app still builds, and says what it could not embed.
BUILD_DIR="${AES67_BUILD_DIR:-../build}"

DRIVER_BUNDLE="$BUILD_DIR/AES67Driver.driver"
if [ -d "$DRIVER_BUNDLE" ]; then
    echo "Embedding AES67Driver.driver..."
    rm -rf "AES67Manager.app/Contents/Resources/AES67Driver.driver"
    ditto "$DRIVER_BUNDLE" "AES67Manager.app/Contents/Resources/AES67Driver.driver"
else
    echo "WARNING: $DRIVER_BUNDLE not found — building without an embedded driver."
    echo "         Build AES67Driver first (needs libASPL), then rebuild this app, so the Install button has something to install."
fi

# The daemon binds UDP 319/320, which coreaudiod cannot; the driver runs on
# the local clock without it.
#
# It goes where SMAppService expects a daemon the app registers: the launchd
# plist in Contents/Library/LaunchDaemons and the executable inside the bundle,
# named by BundleProgram. Nothing is copied to /Library or /usr/local -- launchd
# runs it from in here, and unregistering is what removes it.
PTPD_BINARY="$BUILD_DIR/aes67ptpd"
if [ -f "$PTPD_BINARY" ]; then
    echo "Embedding aes67ptpd..."
    mkdir -p "AES67Manager.app/Contents/Library/LaunchDaemons"
    rm -f "AES67Manager.app/Contents/MacOS/aes67ptpd"
    ditto "$PTPD_BINARY" "AES67Manager.app/Contents/MacOS/aes67ptpd"
    ditto "Resources/com.aes67driver.ptpd.plist" \
          "AES67Manager.app/Contents/Library/LaunchDaemons/com.aes67driver.ptpd.plist"
else
    echo "WARNING: $PTPD_BINARY not found — building without the PTP daemon."
    echo "         The driver will fall back to the local clock."
fi

# Sign inside out, one component at a time.
#
# Not `codesign --deep`: Apple documents it as unsuitable for anything but
# emergency repair, it signs nested code with the outer target's options rather
# than each component's own, and it is on its way out. The order matters --
# every nested piece has to carry its own signature before the enclosing bundle
# is sealed over it, or the outer seal covers an unsigned payload.
echo "Signing app..."
for nested in \
    "AES67Manager.app/Contents/MacOS/aes67ptpd" \
    "AES67Manager.app/Contents/Resources/AES67Driver.driver"
do
    [ -e "$nested" ] && codesign --force --sign - --options runtime --timestamp=none "$nested"
done
codesign --force --sign - --options runtime --timestamp=none AES67Manager.app

echo "Build complete: AES67Manager.app"
