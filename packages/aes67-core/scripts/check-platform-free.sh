#!/bin/bash
# The one rule this library exists to keep: nothing here may reach an Apple
# framework, a socket header, or anything else that assumes an operating
# system. Checked transitively, through the headers each file includes, because
# that is how it breaks in practice -- SimpleRTP.h opened sockets and
# PTPClockSource.h reached CoreAudio without either showing up in the .cpp files
# that used them, and a first pass at this list got five entries wrong for
# exactly that reason.
#
# An ALLOWLIST, not a list of headers known to be bad. The blacklist it
# replaced named fourteen prefixes and could only ever catch a header somebody
# had already thought of: <uuid/uuid.h> sat in Shared/Types.h unused for as
# long as the check existed, passing every run and costing four CI jobs an
# apt-get install of uuid-dev to build a library that never called a uuid_
# function. <unistd.h>, <sys/stat.h> and <sys/time.h> passed the same way.
# Anything not named below fails, so the next one fails the day it is written
# rather than the day someone thinks to blacklist it.
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
# The C++ standard library, which is the whole of what a platform-free library
# is meant to need.
allowed="algorithm array atomic bitset cassert cctype cerrno cfloat charconv chrono
climits cmath compare complex concepts condition_variable cstdarg cstddef cstdint
cstdio cstdlib cstring ctime deque exception execution filesystem forward_list
fstream functional initializer_list iomanip ios iosfwd iostream istream iterator
limits list locale map memory memory_resource mutex new numbers numeric optional
ostream queue random ranges ratio regex scoped_allocator set shared_mutex span
sstream stack stdexcept streambuf string string_view system_error thread tuple
type_traits typeindex typeinfo unordered_map unordered_set utility valarray variant
vector version"

# The POSIX headers this library has accepted, each one deliberate. They assume
# a POSIX host, which every target here is (macOS, Linux; the Teensy build does
# not compile these files), but they are not free -- so they are named, and a
# new one is a decision somebody makes rather than a line that slips through.
#
#   unistd.h, sys/stat.h   the config-file paths in Profiles/ConfigPaths.h
#   sys/time.h, time.h     wall-clock time for SDP origin lines and timestamps
#   stdio.h                DebugLog.h, which writes a file
allowed="$allowed unistd.h sys/stat.h sys/time.h time.h stdio.h"
# One space between entries: the list above is wrapped over several lines, and
# the membership test below matches on " $inc ".
allowed="$(echo $allowed)"

for f in $files; do
  for inc in $(grep -ho '#include <[^>]*>' "$f" 2>/dev/null | sed 's/#include <\(.*\)>/\1/'); do
    case " $allowed " in
      *" $inc "*) ;;
      *)
        hit="$(grep -Hn "#include <$inc>" "$f" 2>/dev/null || true)"
        violations="$violations
$hit"
        ;;
    esac
  done
done

if [ -n "$violations" ]; then
  echo "FAIL: platform headers in a platform-free library:$violations" >&2
  exit 1
fi
echo "$(echo "$files" | wc -l | tr -d ' ') files, all platform-free"
