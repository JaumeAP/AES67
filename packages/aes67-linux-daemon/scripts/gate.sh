#!/bin/bash
# The gate for this package.
#
# Everywhere it builds aes67-profile-conf and runs its suites: that tool reads
# the profiles and a base daemon.conf and nothing else, so it is the half of
# this package a Mac can verify.
#
# On Linux it also configures and builds the vendored daemon and runs the tests
# it ships. On anything else that half is skipped and said to be skipped -- the
# daemon talks to a Linux kernel module over netlink, and a build that succeeds
# without compiling it has proved nothing about it.
set -uo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

daemon="external/aes67-linux-daemon"
# The kernel module is the package next door, and the daemon is built against
# that checkout: see CMakeLists.txt.
module="../ravenna-alsa-lkm/external/ravenna-alsa-lkm"

echo "==> Submodule"
for path in "$daemon/daemon/CMakeLists.txt" \
            "external/cpp-httplib/httplib.h" \
            "$module/driver/Makefile"; do
    if [[ ! -f "$path" ]]; then
        echo "FAIL: $path missing - run: git submodule update --init --recursive" >&2
        exit 1
    fi
done
echo "==> daemon $(git -C "$daemon" rev-parse --short HEAD), module $(git -C "$module" rev-parse --short HEAD) checked out"

# The module is pinned once, by packages/ravenna-alsa-lkm, and cpp-httplib once,
# here. The daemon used to pin both again under its own 3rdparty/, which is what
# the fork this package tracks removed: two checkouts of one repository, free to
# drift, with only one ever compiled. If a rebase onto upstream ever brings
# either submodule back, this says so.
for vendored in 3rdparty/ravenna-alsa-lkm 3rdparty/cpp-httplib; do
    if git -C "$daemon" rev-parse "HEAD:$vendored" > /dev/null 2>&1; then
        echo "FAIL: the daemon pins $vendored again; this tree pins it" >&2
        exit 1
    fi
done

echo "==> Configure"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null || {
    echo "FAIL: configure" >&2; exit 1; }

echo "==> Build"
cmake --build build -j > /dev/null || { echo "FAIL: build" >&2; exit 1; }

echo "==> Tests"
ctest --test-dir build --output-on-failure -R "ProfileConf|ConfCheck|SourceGen|DaemonSdp" || {
    echo "FAIL: tests" >&2; exit 1; }

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "==> Not Linux: the daemon was not built"
    echo "==> PASS"
    exit 0
fi

if [[ ! -x build/aes67-daemon/aes67-daemon ]]; then
    echo "FAIL: no daemon binary on Linux" >&2; exit 1
fi

echo "==> The daemon's own tests"
ctest --test-dir build/aes67-daemon --output-on-failure || {
    echo "FAIL: tests" >&2; exit 1; }

echo "==> PASS"
