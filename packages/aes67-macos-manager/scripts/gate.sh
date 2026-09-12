#!/bin/bash
# The gate for this package: the app's host tests, and the app itself when
# there is a Swift toolchain to build it with.
#
# The driver's artifacts are not a precondition. build.sh embeds
# AES67Driver.driver and aes67ptpd when the driver package's build/ has them
# and says what it could not find when it does not; a gate that demanded them
# would be a gate for two packages. The tree gate runs the driver's first, so
# there they are present.
#
#   scripts/gate.sh          host tests, then the app
set -uo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "$(uname -s)" != "Darwin" ]; then
  echo "==> Not macOS: nothing here builds"
  echo "==> PASS"
  exit 0
fi

if ! command -v swiftc > /dev/null 2>&1; then
  echo "FAIL: no swiftc on PATH" >&2
  exit 1
fi

echo "==> Host tests"
if ! AES67_BUILD_DIR=build ./run-tests.sh; then
  echo "FAIL: host tests" >&2
  exit 1
fi

echo "==> Uninstaller"
if ! ./build-uninstaller.sh --force > build/uninstaller.log 2>&1; then
  tail -20 build/uninstaller.log >&2
  echo "FAIL: build-uninstaller.sh" >&2
  exit 1
fi
test -x AES67Uninstall.app/Contents/MacOS/AES67Uninstall || {
  echo "FAIL: no executable in the uninstaller bundle" >&2
  exit 1
}

echo "==> App"
if ! ./build.sh --force > build/build.log 2>&1; then
  tail -20 build/build.log >&2
  echo "FAIL: build.sh" >&2
  exit 1
fi
grep "^WARNING" build/build.log || true

# The languages the app declares have to be the ones it carries, or macOS
# shows English to somebody whose system is not.
for lang in ca es en; do
  test -f "AES67Manager.app/Contents/Resources/$lang.lproj/Localizable.strings" || {
    echo "FAIL: the Manager carries no $lang.lproj" >&2
    exit 1
  }
done
test -x AES67Manager.app/Contents/MacOS/AES67Manager || {
  echo "FAIL: no app bundle produced" >&2; exit 1; }

# ---------------------------------------------------------------------------
# The analysis half, under AES67_ANALYSE=1. Swift has no clang-tidy, and the
# two checks that do apply to it -- the compiler's own warnings, and
# swift-format's lint -- cost seconds; what is expensive is the rest.
#
#   cppcheck     over DriverManager.cpp/.h, the C++ shim over the HAL
#   swift-format the toolchain's own linter, over every Swift file
#   sanitizers   the host tests under ASan and UBSan
#   fuzz         NmosResources, fed JSON a node this Mac does not own
#   coverage     line coverage of the tested Swift, printed only
# ---------------------------------------------------------------------------
echo "==> Installer"
if ! ./build-installer.sh --force > build/installer.log 2>&1; then
  tail -20 build/installer.log >&2
  echo "FAIL: build-installer.sh" >&2
  exit 1
fi
test -x AES67Install.app/Contents/MacOS/AES67Install || {
  echo "FAIL: no executable in the installer bundle" >&2
  exit 1
}
# What it carries decides what its ticks can do, and which languages it speaks.
for carried in AES67Manager.app AES67Controller.app ca.lproj es.lproj en.lproj; do
  test -e "AES67Install.app/Contents/Resources/$carried" || {
    echo "FAIL: the installer carries no $carried" >&2
    exit 1
  }
done


if [ "${AES67_ANALYSE:-0}" = "1" ]; then
  # Found the way the C++ packages find it: neither cppcheck nor clang-tidy
  # ships with the Command Line Tools. CPPCHECK=/path overrides.
  cppcheck_bin="${CPPCHECK:-}"
  if [ -z "$cppcheck_bin" ]; then
    for candidate in "$HOME/.local/venvs/cpptools/bin/cppcheck" "$(command -v cppcheck || true)"; do
      [ -x "$candidate" ] && { cppcheck_bin="$candidate"; break; }
    done
  fi

  echo "==> cppcheck (the C++ shim)"
  if [ -n "$cppcheck_bin" ]; then
    "$cppcheck_bin" --quiet --error-exitcode=1 --enable=warning,performance,portability \
      --inline-suppr --std=c++17 --language=c++ \
      --suppress=missingInclude --suppress=missingIncludeSystem \
      --suppress=normalCheckLevelMaxBranches \
      DriverManager.cpp DriverManager.h || { echo "FAIL: cppcheck" >&2; exit 1; }
  else
    echo "SKIP: no cppcheck found"
  fi

  echo "==> swift-format lint"
  if xcrun --find swift-format > /dev/null 2>&1; then
    xcrun swift-format lint --recursive Models Views Tests AES67ManagerApp.swift \
      || { echo "FAIL: swift-format" >&2; exit 1; }
  else
    echo "SKIP: no swift-format in this toolchain"
  fi

  echo "==> Sanitizers (address, undefined)"
  mkdir -p build
  xcrun swiftc -g -Onone -swift-version 5 -warnings-as-errors \
    -sanitize=address -sanitize=undefined \
    -o build/ManagerAppTests-san \
    Models/PrivilegedScript.swift Models/NmosResources.swift Models/SessionList.swift \
    Uninstaller/UninstallPlan.swift \
    Tests/PrivilegedScriptTests.swift Tests/NmosResourcesTests.swift \
    Tests/SessionListTests.swift Tests/main.swift \
    || { echo "FAIL: sanitizer build" >&2; exit 1; }
  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" build/ManagerAppTests-san \
    || { echo "FAIL: tests under sanitizers" >&2; exit 1; }

  echo "==> Fuzzing NmosResources"
  xcrun swiftc -O -swift-version 5 -sanitize=address -sanitize=undefined \
    -o build/FuzzNmosResources Models/NmosResources.swift Tests/fuzz/main.swift \
    || { echo "FAIL: fuzz build" >&2; exit 1; }
  build/FuzzNmosResources "${AES67_FUZZ_ITER:-200000}" || { echo "FAIL: fuzzing" >&2; exit 1; }

  echo "==> Coverage"
  xcrun swiftc -g -Onone -swift-version 5 \
    -profile-generate -profile-coverage-mapping \
    -o build/ManagerAppTests-cov \
    Models/PrivilegedScript.swift Models/NmosResources.swift Models/SessionList.swift \
    Uninstaller/UninstallPlan.swift \
    Tests/PrivilegedScriptTests.swift Tests/NmosResourcesTests.swift \
    Tests/SessionListTests.swift Tests/main.swift \
    || { echo "FAIL: coverage build" >&2; exit 1; }
  ( cd build && LLVM_PROFILE_FILE=coverage.profraw ./ManagerAppTests-cov > /dev/null ) \
    || { echo "FAIL: tests under coverage" >&2; exit 1; }
  xcrun llvm-profdata merge -sparse build/coverage.profraw -o build/coverage.profdata \
    || { echo "FAIL: llvm-profdata" >&2; exit 1; }
  xcrun llvm-cov report build/ManagerAppTests-cov -instr-profile=build/coverage.profdata \
    -ignore-filename-regex='Tests/' 2>/dev/null | grep -E "^TOTAL" \
    || echo "coverage: no report"
  rm -f build/coverage.profraw build/coverage.profdata
else
  echo "==> Analysis skipped: cppcheck, swift-format, sanitizers, fuzzing and coverage (AES67_ANALYSE=1 to run them)"
fi

echo "==> PASS"
