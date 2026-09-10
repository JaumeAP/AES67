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

# Butler/ stays off the disk.
#
# The checkout carries Merging's own user-space daemon as a 5.5 MB binary
# under a licence of its own, plus its web app: 7.9 MB of the 8.7 that this
# submodule weighs, and nothing here builds, installs or runs any of it --
# the user-space half this tree uses is bondagit/aes67-linux-daemon, from
# source. See README.md.
#
# It cannot be deleted: it is tracked in the submodule's own repository, which
# is not ours. sparse-checkout is how it never lands instead, and it is set
# here rather than left to a README line because a fresh clone would otherwise
# fetch it and nobody would notice.
if [[ -d "$module/Butler" ]]; then
    echo "==> Excluding Butler/ from the checkout (a binary nothing here uses)"
    git -C "$module" sparse-checkout init --no-cone > /dev/null 2>&1 || true
    printf '/*\n!/Butler/\n' | git -C "$module" sparse-checkout set --stdin > /dev/null 2>&1 \
        || echo "note: sparse-checkout not available; Butler/ stays on disk, unused"
fi

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
