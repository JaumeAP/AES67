#!/bin/bash
# The gate for the whole tree: it runs each package's own, in order, and stops
# at the first that fails.
#
# Each package verifies itself. This script owns no checks of its own -- it
# knows which packages there are and nothing about what any of them does, so a
# package can change how it verifies itself without this file changing.
#
#   packages/aes67-profiles/scripts/gate.sh      build, test, self-containment
#   packages/aes67-core/scripts/gate.sh          build, test, platform contract
#   packages/aes67-ravenna/scripts/gate.sh       build, tests, a live DESCRIBE
#   packages/aes67-macos-driver/scripts/gate.sh   build, test, CMake sanity
#   packages/aes67-macos-manager/scripts/gate.sh  host tests, the app
#   packages/aes67-linux-ptpd/scripts/gate.sh    build, wire tests
#   packages/t41-ptp/scripts/gate.sh             host tests
#
# aes67-core and aes67-ravenna run before the driver on purpose: the driver
# builds both in, and their failures are harder to read through it than on
# their own. aes67-ravenna used to run after it, which is the one place this
# order did not follow its own rule.
#
# .githooks/pre-push runs this. Opt in per clone with
# `git config core.hooksPath .githooks`, since hook configuration is local and
# does not travel with a repository.
#
#   scripts/gate.sh              the cheap half of every package
#   AES67_ANALYSE=1 scripts/gate.sh   plus each package's analysis half: clang-tidy,
#                                     cppcheck, the suites under ASan and UBSan, the
#                                     parsers fuzzed, and line coverage
#
# Arguments are passed through to each package gate, so `--clean` or
# `--analyse` reach the ones that understand them and are refused by the ones
# that do not -- which is why they are not forwarded by default.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

run() {
  local name="$1"; shift
  echo
  echo "######## $name"
  if ! "$@"; then
    echo
    echo "FAIL: $name" >&2
    exit 1
  fi
}

run "aes67-profiles"     packages/aes67-profiles/scripts/gate.sh
run "aes67-core"         packages/aes67-core/scripts/gate.sh
run "aes67-ravenna"      packages/aes67-ravenna/scripts/gate.sh
run "aes67-macos-driver" packages/aes67-macos-driver/scripts/gate.sh
run "aes67-macos-manager" packages/aes67-macos-manager/scripts/gate.sh
run "aes67-linux-ptpd"   packages/aes67-linux-ptpd/scripts/gate.sh

# The RAVENNA ALSA kernel module is a checkout, not a package: docs/ravenna-alsa-lkm.md
# says why. What its wrapper's gate did, this does.
#
# Butler/ stays off the disk: the checkout carries Merging's own user-space
# daemon as a 5.5 MB binary under a licence of its own, plus its web app --
# 7.9 MB of the 8.7 this submodule weighs, and nothing here builds, installs
# or runs any of it. It cannot be deleted, being tracked in a repository that
# is not ours, so it is not fetched instead.
lkm="external/ravenna-alsa-lkm"
if [ -d "$lkm/Butler" ]; then
  echo
  echo "######## ravenna-alsa-lkm"
  echo "==> Excluding Butler/ from the checkout (a binary nothing here uses)"
  git -C "$lkm" sparse-checkout init --no-cone > /dev/null 2>&1 || true
  printf '/*\n!/Butler/\n' | git -C "$lkm" sparse-checkout set --stdin > /dev/null 2>&1 \
    || echo "note: sparse-checkout not available; Butler/ stays on disk, unused"
fi
if [ -f "$lkm/driver/Makefile" ] && [ "$(uname -s)" = "Linux" ] \
   && [ -d "/lib/modules/$(uname -r)/build" ]; then
  run "ravenna-alsa-lkm" make -C "$lkm/driver"
fi
run "t41-ptp"            packages/t41-ptp/scripts/gate.sh

echo
echo "######## every package passed"
