#!/bin/bash
# Static analysis over the library, with .clang-tidy deciding what counts.
#
# clang-tidy is not part of the Command Line Tools, so this script does not
# assume it: it looks for one and says how to get it rather than failing with
# "command not found". The pip package is the light way in -- match its version
# to the system compiler, or its idea of the standard library will not match
# what actually compiles here.
#
#   python3 -m venv ~/.local/venvs/cpptools
#   ~/.local/venvs/cpptools/bin/pip install clang-tidy==21.1.6
#
# The -isysroot is not optional. Without it a non-Apple clang-tidy cannot find
# <cstdint> and reports every file as a compilation error, which looks alarming
# and means nothing.
set -uo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

TIDY="${CLANG_TIDY:-}"
if [ -z "$TIDY" ]; then
  for candidate in "$HOME/.local/venvs/cpptools/bin/clang-tidy" "$(command -v clang-tidy || true)"; do
    [ -x "$candidate" ] && { TIDY="$candidate"; break; }
  done
fi
if [ -z "$TIDY" ]; then
  echo "SKIP: no clang-tidy found. See the comment at the top of this script." >&2
  exit 0
fi

build_dir="${1:-build}"
if [ ! -f "$build_dir/compile_commands.json" ]; then
  cmake -S . -B "$build_dir" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > /dev/null || {
    echo "FAIL: cannot configure $build_dir" >&2; exit 1; }
fi

sdk="$(xcrun --show-sdk-path 2>/dev/null || true)"
extra=()
[ -n "$sdk" ] && extra=(--extra-arg=-isysroot --extra-arg="$sdk")

sources="$(python3 -c "
import json, sys
db = json.load(open('$build_dir/compile_commands.json'))
print('\n'.join(e['file'] for e in db
                if '/Tests/' not in e['file'] and '/external/' not in e['file']))")"

echo "==> clang-tidy ($("$TIDY" --version | sed -n 's/.*LLVM version \(.*\)/\1/p'))"
# shellcheck disable=SC2086
echo "$sources" | xargs "$TIDY" -p "$build_dir" --quiet "${extra[@]}"
status=$?

if [ $status -ne 0 ]; then
  echo "FAIL: clang-tidy reported an error-level check (see WarningsAsErrors in .clang-tidy)" >&2
  exit 1
fi
echo "$(echo "$sources" | wc -l | tr -d ' ') files analysed, no error-level findings"
