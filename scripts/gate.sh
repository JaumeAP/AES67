#!/bin/bash
# The gate for the whole tree: it runs each package's own, in order, and stops
# at the first that fails.
#
# Each package verifies itself. This script owns no checks of its own -- it
# knows which packages there are and nothing about what any of them does, so a
# package can change how it verifies itself without this file changing.
#
#   packages/aes67-core/scripts/gate.sh          build, test, platform contract
#   packages/aes67-macos-driver/scripts/gate.sh   build, test, CMake sanity
#   packages/t41-ptp/scripts/gate.sh             host tests
#
# aes67-core runs before the driver on purpose: the driver builds the core in
# and its failures are harder to read than the core's own.
#
# .githooks/pre-push runs this. Opt in per clone with
# `git config core.hooksPath .githooks`, since hook configuration is local and
# does not travel with a repository.
#
#   scripts/gate.sh              the cheap half of every package
#   AES67_ANALYSE=1 scripts/gate.sh   with static analysis where a package has it
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

run "aes67-core"         packages/aes67-core/scripts/gate.sh
run "aes67-macos-driver" packages/aes67-macos-driver/scripts/gate.sh
run "t41-ptp"            packages/t41-ptp/scripts/gate.sh

echo
echo "######## every package passed"
