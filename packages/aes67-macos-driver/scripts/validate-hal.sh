#!/bin/bash
# Core Audio validation of the *installed* AES67 plugin.
#
# Everything here goes through Apple's own tooling and APIs -- the plugin
# bundle as coreaudiod sees it, the unified log, and the HAL client API via
# Tools/HALValidate -- so it reports on what is installed in
# /Library/Audio/Plug-Ins/HAL, not on this build tree.
#
# Usage:
#   scripts/validate-hal.sh                 full check
#   scripts/validate-hal.sh --skip-io       properties and rates only
#   scripts/validate-hal.sh -- --name Foo   pass the rest to HALValidate
#
# Exit status is HALValidate's: non-zero if any check failed.
set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

install_dir="/Library/Audio/Plug-Ins/HAL"
bundle="$install_dir/AES67Driver.driver"
build_dir="${BUILD_DIR:-build}"
validator="$build_dir/Tools/HALValidate"

echo "== Installed plugin =="
if [ -d "$bundle" ]; then
    version=$(/usr/bin/defaults read "$bundle/Contents/Info" CFBundleShortVersionString 2>/dev/null || echo "?")
    build_number=$(/usr/bin/defaults read "$bundle/Contents/Info" CFBundleVersion 2>/dev/null || echo "?")
    identifier=$(/usr/bin/defaults read "$bundle/Contents/Info" CFBundleIdentifier 2>/dev/null || echo "?")
    echo "  $bundle"
    echo "  identifier $identifier, version $version ($build_number)"
    /usr/bin/codesign --verify --verbose=1 "$bundle" 2>&1 | sed 's/^/  codesign: /'
else
    echo "  NOT INSTALLED: $bundle is missing."
    echo "  Install with: sudo cmake --install $build_dir   (then restart coreaudiod)"
fi

echo
echo "== coreaudiod =="
if /usr/bin/pgrep -q coreaudiod; then
    echo "  running (pid $(/usr/bin/pgrep coreaudiod | head -1))"
else
    echo "  NOT RUNNING"
fi

echo
echo "== Core Audio's view of the plugin (system_profiler) =="
/usr/sbin/system_profiler SPAudioDataType 2>/dev/null \
    | /usr/bin/grep -A6 "AES67" \
    | sed 's/^/  /' \
    || echo "  no AES67 device reported"

echo
echo "== coreaudiod log, last 10 minutes, plugin messages =="
/usr/bin/log show --last 10m --style compact \
    --predicate 'process == "coreaudiod" AND (eventMessage CONTAINS "AES67" OR eventMessage CONTAINS "aes67")' \
    2>/dev/null | tail -20 | sed 's/^/  /'

echo
echo "== HAL client API checks =="
if [ ! -x "$validator" ]; then
    echo "  $validator not built."
    echo "  Build with: cmake -S . -B $build_dir -DBUILD_TOOLS=ON && cmake --build $build_dir --target HALValidate"
    exit 1
fi

# A device with input streams goes through TCC: without microphone access for
# the terminal, opening it blocks. HALValidate detects that and skips the IO
# section rather than hanging.
"$validator" "$@"
