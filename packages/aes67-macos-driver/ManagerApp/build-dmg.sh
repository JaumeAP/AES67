#!/bin/bash
#
# build-dmg.sh
# AES67 macOS Driver
# Wraps AES67Manager.app in a disk image.
#
# That is the whole of the delivery. There is no package that writes to
# /Library or /usr/local: the app carries the driver, the PTP daemon and the
# daemon's LaunchDaemon as resources and puts them down itself, asking for
# privileges once, when the Install button is pressed. What the user does with
# this image is drag one app into Applications.
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Same convention as build.sh: CMake passes its binary directory, and a
# standalone run falls back to this package's own build/.
BUILD_DIR="${AES67_BUILD_DIR:-$PROJECT_ROOT/build}"

APP="$SCRIPT_DIR/AES67Manager.app"
VERSION="$(sed -n 's/^\([0-9][0-9.]*\)-build.*/\1/p' "$PROJECT_ROOT/VERSION.txt")"
OUTPUT_DIR="$BUILD_DIR/dmg"
STAGING_DIR="$OUTPUT_DIR/staging"
DMG="$OUTPUT_DIR/AES67Manager-${VERSION}.dmg"

if [ ! -d "$APP" ]; then
    echo "ERROR: $APP not found. Build the app first (ManagerApp/build.sh)." >&2
    exit 1
fi

# What the app carries decides what the Install button can install, so say so
# here rather than letting the user find out from a disabled button.
for resource in AES67Driver.driver aes67ptpd com.aes67driver.ptpd.plist; do
    if [ ! -e "$APP/Contents/Resources/$resource" ]; then
        echo "WARNING: the app carries no $resource — the Install button will not install it."
    fi
done

rm -rf "$OUTPUT_DIR"
mkdir -p "$STAGING_DIR"

ditto "$APP" "$STAGING_DIR/AES67Manager.app"
ln -s /Applications "$STAGING_DIR/Applications"

hdiutil create \
    -volname "AES67 Manager" \
    -srcfolder "$STAGING_DIR" \
    -ov -format UDZO \
    "$DMG"

rm -rf "$STAGING_DIR"

echo ""
echo "Disk image: $DMG"
echo ""
echo "Drag AES67Manager.app to Applications, open it, and press Install."
echo "The app asks for an administrator password once, then puts down:"
echo "  /Library/Audio/Plug-Ins/HAL/AES67Driver.driver"
echo "  /usr/local/libexec/aes67ptpd"
echo "  /Library/LaunchDaemons/com.aes67driver.ptpd.plist"
echo ""
echo "This image is UNSIGNED. To sign the app before packaging it:"
echo "  codesign --force --deep --sign \"Developer ID Application: Your Name\" \"$APP\""
