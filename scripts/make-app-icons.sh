#!/bin/bash
# The app icons, from the drawing to the .icns each bundle carries.
#
# scripts/make-app-icons.swift draws every size; iconutil packs each .iconset.
# The results land in the package that ships the bundle:
#
#   packages/aes67-macos-manager/Resources/AES67Manager.icns
#   packages/aes67-macos-manager/Resources/AES67Install.icns
#   packages/aes67-macos-manager/Resources/AES67Uninstall.icns
#   packages/aes67-macos-controller/Resources/AES67Controller.icns
#
# Committed, because a build should not need a compiler pass to have an icon,
# and re-runnable, because the drawing is the source and this is the build of
# it. Run it after editing the .swift.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "==> Drawing"
swift "$root/scripts/make-app-icons.swift" "$work"

echo "==> Packing"
manager_resources="$root/packages/aes67-macos-manager/Resources"
controller_resources="$root/packages/aes67-macos-controller/Resources"
mkdir -p "$manager_resources" "$controller_resources"

pack() {
    local name="$1" destination="$2"
    iconutil --convert icns --output "$destination/$name.icns" "$work/$name.iconset"
    echo "    $destination/$name.icns"
}

pack AES67Manager "$manager_resources"
pack AES67Install "$manager_resources"
pack AES67Uninstall "$manager_resources"
pack AES67Controller "$controller_resources"

echo "==> Done"
