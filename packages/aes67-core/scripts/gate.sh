#!/bin/bash
# Everything this repository checks, in one command.
#
#   scripts/gate.sh            build, test, contract, static analysis
#   scripts/gate.sh --clean    wipe build/ first
#
# Run by .githooks/pre-push. Until this existed, the three checks here only ran
# when somebody remembered to type them -- and the consumer that did run one of
# them (aes67-macos-driver calls check-platform-free.sh from its own gate) was
# the only automatic coverage this library had. A base other projects build on
# should not depend on a consumer for that.
set -uo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

build_dir="build"
analyse=0
for arg in "$@"; do
  case "$arg" in
    --clean)
      echo "==> Removing $build_dir"
      rm -rf "$build_dir"
      ;;
    --analyse|--analyze)
      analyse=1
      ;;
  esac
done

# doctest lives once, at the root of the monorepo, and is shared with the
# driver package. This path is what the CMakeLists falls back to when the root
# has not set AES67_DOCTEST_DIR, which is the case when this package is built
# on its own -- as it is here.
doctest_dir="../../external/doctest/doctest"
if [ ! -f "$doctest_dir/doctest.h" ]; then
  echo "FAIL: $doctest_dir is empty - run: git submodule update --init --recursive" >&2
  exit 1
fi

echo "==> Configure"
cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > /dev/null \
  || { echo "FAIL: cmake configure" >&2; exit 1; }

echo "==> Build"
cmake --build "$build_dir" -j"$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4)" \
  || { echo "FAIL: build" >&2; exit 1; }

echo "==> Tests"
( cd "$build_dir" && ctest --output-on-failure ) || { echo "FAIL: tests" >&2; exit 1; }

echo "==> Platform-free contract"
scripts/check-platform-free.sh || { echo "FAIL: platform contract" >&2; exit 1; }

# Static analysis, and only when asked for. It costs minutes where the tests
# cost a second, and it is not where regressions appear: clang-tidy finds
# latent defects, not something that broke between two commits. Paying for it
# on every push taxes the wrong moment.
#
# AES67_ANALYSE=1, or scripts/gate.sh --analyse. The monorepo's
# scripts/gate.sh passes the variable through, and .githooks/pre-push sets it
# when the push is going to the default branch, after asking.
if [ "${AES67_ANALYSE:-0}" = "1" ] || [ "$analyse" = "1" ]; then
  echo "==> Static analysis"
  scripts/check-tidy.sh "$build_dir" || { echo "FAIL: clang-tidy" >&2; exit 1; }
else
  echo "==> Static analysis skipped (AES67_ANALYSE=1 or --analyse to run it)"
fi

echo "==> PASS"
