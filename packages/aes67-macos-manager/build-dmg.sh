#!/bin/bash
#
# build-dmg.sh
# AES67 macOS Driver
# Wraps the installer in a disk image.
#
# One thing to open, not two to drag: AES67Install.app carries the Manager and
# the Controller inside it, shows a tick per application, and makes the machine
# match the ticks -- installing what is ticked, removing what is not, and, from
# its own button, taking everything off including the driver and the PTP
# daemon. AES67Uninstall.app rides along for whoever wants the removal alone.
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

APP="$SCRIPT_DIR/AES67Install.app"
UNINSTALL_APP="$SCRIPT_DIR/AES67Uninstall.app"
VERSION="$(sed -n 's/^\([0-9][0-9.]*\)-build.*/\1/p' "$DRIVER_PACKAGE/VERSION.txt")"
OUTPUT_DIR="$BUILD_DIR/dmg"
STAGING_DIR="$OUTPUT_DIR/staging"
DMG="$OUTPUT_DIR/AES67Manager-${VERSION}.dmg"

if [ ! -d "$APP" ]; then
    echo "ERROR: $APP not found. Build it first (build-installer.sh, which needs" >&2
    echo "       build.sh and the controller's build.sh to have run)." >&2
    exit 1
fi

# What the app carries decides what the Install button can install, so say so
# here rather than letting the user find out from a disabled button.
for app in AES67Manager.app AES67Controller.app; do
    if [ ! -d "$APP/Contents/Resources/$app" ]; then
        echo "WARNING: the installer carries no $app — that tick will do nothing."
    fi
done
for resource in AES67Driver.driver aes67ptpd com.aes67driver.ptpd.plist; do
    if [ ! -e "$APP/Contents/Resources/AES67Manager.app/Contents/Resources/$resource" ]; then
        echo "WARNING: the Manager inside carries no $resource — its Install button will not install it."
    fi
done

rm -rf "$OUTPUT_DIR"
mkdir -p "$STAGING_DIR"

ditto "$APP" "$STAGING_DIR/AES67Install.app"

# The Controller is carried when it has been built, and its absence is a
# warning rather than a failure: it is a separate package with its own gate,
# and an image with the Manager alone is still the delivery it always was.

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
echo "Open AES67Install.app: it has a tick per application and one button."
echo ""
echo "AES67Manager.app is this machine's: once installed, its own Install button"
echo "asks for an administrator password once and puts down:"
echo "  /Library/Audio/Plug-Ins/HAL/AES67Driver.driver"
echo "  /usr/local/libexec/aes67ptpd"
echo "  /Library/LaunchDaemons/com.aes67driver.ptpd.plist"
echo ""
echo ""
echo "AES67Controller.app is the network's: sessions and NMOS crosspoints, for"
echo "routing a plant. It installs nothing and needs nothing installed."
echo ""
echo "Both are inside the installer, and so is removal: its own button takes"
echo "everything off, driver and PTP daemon included. AES67Uninstall.app rides"
echo "along for whoever wants the removal on its own."
echo ""
echo "This image is UNSIGNED. To sign the apps before packaging them:"
echo "  codesign --force --deep --sign \"Developer ID Application: Your Name\" \"$APP\""
echo "  codesign --force --deep --sign \"Developer ID Application: Your Name\" \"$UNINSTALL_APP\""
