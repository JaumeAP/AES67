#!/bin/bash
# Core Audio validation of the *installed* AES67 plugin.
#
# Everything here goes through Apple's own tooling and APIs -- the plugin
# bundle as coreaudiod sees it, the unified log, and the HAL client API via
# HALValidate -- so it reports on what is installed in
# /Library/Audio/Plug-Ins/HAL, not on this build tree.
#
# HALValidate moved out of this repository on 2026-09-16: it links no driver
# code and checks whatever coreaudiod has loaded, so it belongs with the other
# device-neutral tools, in Eines/packages/macos-audio-development/tools. Point
# HAL_VALIDATE at the built binary, or let the lookup below find the usual
# checkout.
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

# HAL_VALIDATE wins; otherwise the sibling checkout, then the one in a home
# directory, then whatever is on PATH.
validator="${HAL_VALIDATE:-}"
if [ -z "$validator" ]; then
    for candidate in \
        "$repo_root/../../../Eines/packages/macos-audio-development/tools/build/HALValidate" \
        "$HOME/projects/Eines/packages/macos-audio-development/tools/build/HALValidate" \
        "$(command -v HALValidate 2>/dev/null || true)"
    do
        if [ -n "$candidate" ] && [ -x "$candidate" ]; then
            validator="$candidate"
            break
        fi
    done
fi

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
if [ -z "$validator" ] || [ ! -x "$validator" ]; then
    echo "  HALValidate not found."
    echo "  It lives in Eines/packages/macos-audio-development/tools; build it with"
    echo "    cmake -S <eines>/packages/macos-audio-development/tools -B <same>/build"
    echo "    cmake --build <same>/build --target HALValidate"
    echo "  then re-run this, or set HAL_VALIDATE to the binary."
    exit 1
fi

echo "  using $validator"

# A device with input streams goes through TCC: without microphone access for
# the terminal, opening it blocks. HALValidate detects that and skips the IO
# section rather than hanging.
"$validator" "$@"
