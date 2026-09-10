#!/bin/bash
# The gate for this package.
#
# The module is kernel C: there is nothing to compile anywhere but Linux, and
# on Linux there is nothing to compile without the running kernel's headers.
# So the gate checks the checkout is there and pinned, and on Linux builds the
# module when the headers exist and says why it did not when they do not --
# rather than passing on nothing, or failing on a machine that was never going
# to be able to build it.
set -uo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

module="external/ravenna-alsa-lkm"

echo "==> Submodule"
if [[ ! -f "$module/driver/Makefile" ]]; then
    echo "FAIL: $module/driver/Makefile missing - run: git submodule update --init --recursive" >&2
    exit 1
fi
echo "==> $(git -C "$module" rev-parse --short HEAD) checked out"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "==> Not Linux: the module was not built"
    echo "==> PASS"
    exit 0
fi

headers="/lib/modules/$(uname -r)/build"
if [[ ! -d "$headers" ]]; then
    echo "==> No kernel headers at $headers: the module was not built"
    echo "==> PASS"
    exit 0
fi

echo "==> Build"
if ! make -C "$module/driver" > /dev/null; then
    echo "FAIL: the module did not build" >&2
    exit 1
fi

if [[ ! -f "$module/driver/MergingRavennaALSA.ko" ]]; then
    echo "FAIL: no module built" >&2
    exit 1
fi

echo "==> PASS"
