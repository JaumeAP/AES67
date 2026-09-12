#!/bin/bash
#
# build-dmg.sh
# AES67 macOS Driver
# Wraps the three applications in a disk image: AES67Manager.app,
# AES67Controller.app and AES67Uninstall.app.
#
# That is the whole of the delivery. There is no package that writes to
# /Library or /usr/local: the Manager carries the driver, the PTP daemon and
# the daemon's LaunchDaemon as resources and puts them down itself, asking for
# privileges once, when the Install button is pressed. What the user does with
# this image is drag the apps into Applications.
#
# Three applications, one image, because they are one delivery and three jobs:
# the Manager is about this machine, the Controller about the network, and the
# Uninstaller about getting the first one off again. The Controller needs
# nothing installed to be useful, and whoever only routes a plant drags that
# one and never presses Install. The Uninstaller is separate on purpose:
# removing should not require opening the thing being removed, or having it
# still there at all.
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DRIVER_PACKAGE="$(cd "$SCRIPT_DIR/../aes67-macos-driver" && pwd)"

# Same convention as build.sh: CMake passes the driver's binary directory, and
# a standalone run falls back to that package's own build/. The version is the
# driver's too -- this image ships the driver, so it carries its number.
BUILD_DIR="${AES67_BUILD_DIR:-$DRIVER_PACKAGE/build}"

APP="$SCRIPT_DIR/AES67Manager.app"
CONTROLLER_APP="$SCRIPT_DIR/../aes67-macos-controller/AES67Controller.app"
UNINSTALL_APP="$SCRIPT_DIR/AES67Uninstall.app"
VERSION="$(sed -n 's/^\([0-9][0-9.]*\)-build.*/\1/p' "$DRIVER_PACKAGE/VERSION.txt")"
OUTPUT_DIR="$BUILD_DIR/dmg"
STAGING_DIR="$OUTPUT_DIR/staging"
DMG="$OUTPUT_DIR/AES67Manager-${VERSION}.dmg"

if [ ! -d "$APP" ]; then
    echo "ERROR: $APP not found. Build the app first (build.sh)." >&2
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

# The Controller is carried when it has been built, and its absence is a
# warning rather than a failure: it is a separate package with its own gate,
# and an image with the Manager alone is still the delivery it always was.
if [ -d "$CONTROLLER_APP" ]; then
    ditto "$CONTROLLER_APP" "$STAGING_DIR/AES67Controller.app"
else
    echo "WARNING: $CONTROLLER_APP not found — the image will carry the Manager alone."
    echo "         Build it with ../aes67-macos-controller/build.sh"
fi

if [ -d "$UNINSTALL_APP" ]; then
    ditto "$UNINSTALL_APP" "$STAGING_DIR/AES67Uninstall.app"
else
    echo "WARNING: $UNINSTALL_APP not found — the image will carry no uninstaller."
    echo "         Build it with ./build-uninstaller.sh"
fi

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
echo "Drag the apps to Applications."
echo ""
echo "AES67Manager.app is this machine's: open it and press Install."
echo "The app asks for an administrator password once, then puts down:"
echo "  /Library/Audio/Plug-Ins/HAL/AES67Driver.driver"
echo "  /usr/local/libexec/aes67ptpd"
echo "  /Library/LaunchDaemons/com.aes67driver.ptpd.plist"
echo ""
echo ""
echo "AES67Controller.app is the network's: sessions and NMOS crosspoints, for"
echo "routing a plant. It installs nothing and needs nothing installed."
echo ""
echo "AES67Uninstall.app takes the driver, the PTP daemon and the settings back"
echo "off the machine, and can be run without the Manager being there."
echo ""
echo "This image is UNSIGNED. To sign the apps before packaging them:"
echo "  codesign --force --deep --sign \"Developer ID Application: Your Name\" \"$APP\""
echo "  codesign --force --deep --sign \"Developer ID Application: Your Name\" \"$CONTROLLER_APP\""
echo "  codesign --force --deep --sign \"Developer ID Application: Your Name\" \"$UNINSTALL_APP\""
