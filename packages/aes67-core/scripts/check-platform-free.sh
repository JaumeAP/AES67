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

# Headers this library reaches outside itself. There is one: the 1588 dataset
# comparison, which lives in the Teensy PTP library because that is where it
# was written first (NetworkEngine/PTP/PTPProtocolTypes.h says why). The rule
# has to cover it, or a change over there could put a board header into this
# library without anything here failing. Resolved from what is actually
# included rather than named literally, so a second such include is covered
# the day someone writes it.
t41_dir="../t41-ptp/src"
for inc in $(grep -rho '#include "ptp/[^"]*"' $files 2>/dev/null | sed 's/.*"\(.*\)"/\1/' | sort -u); do
  if [ -f "$t41_dir/$inc" ]; then
    files="$files
$t41_dir/$inc"
  else
    echo "FAIL: $inc is included from this library and was not found under $t41_dir" >&2
    exit 1
  fi
done
for f in $files; do
  hit="$(grep -Hno '#include <\(CoreAudio\|CoreFoundation\|AudioToolbox\|Accelerate\|mach\|sys/socket\|netinet\|arpa\|ifaddrs\|Arduino\|QNEthernet\|TimeLib\|imxrt\|core_pins\)[^>]*>' "$f" 2>/dev/null || true)"
  [ -n "$hit" ] && violations="$violations
$hit"
done

if [ -n "$violations" ]; then
  echo "FAIL: platform headers in a platform-free library:$violations" >&2
  exit 1
fi
echo "$(echo "$files" | wc -l | tr -d ' ') files, all platform-free"
