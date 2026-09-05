#!/bin/bash
# The one rule this library exists to keep: nothing here may reach an Apple
# framework, a socket header, or anything else that assumes an operating
# system. Checked transitively, through the headers each file includes, because
# that is how it breaks in practice -- SimpleRTP.h opened sockets and
# PTPClockSource.h reached CoreAudio without either showing up in the .cpp files
# that used them, and a first pass at this list got five entries wrong for
# exactly that reason.
set -uo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

violations=""
files="$(find Driver NetworkEngine Shared -name '*.cpp' -o -name '*.h' -o -name '*.hpp' 2>/dev/null)"
for f in $files; do
  hit="$(grep -Hno '#include <\(CoreAudio\|CoreFoundation\|AudioToolbox\|Accelerate\|mach\|sys/socket\|netinet\|arpa\|ifaddrs\)[^>]*>' "$f" 2>/dev/null || true)"
  [ -n "$hit" ] && violations="$violations
$hit"
done

if [ -n "$violations" ]; then
  echo "FAIL: platform headers in a platform-free library:$violations" >&2
  exit 1
fi
echo "$(echo "$files" | wc -l | tr -d ' ') files, all platform-free"
