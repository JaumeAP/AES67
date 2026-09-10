#!/bin/bash
# The gate for this package: build it, run its suites, and check that the
# announcer really answers.
#
# The last part is not a unit test and is here on purpose. Everything this
# package does that matters happens between two machines, and the three suites
# only prove that the bytes are right: they cannot tell whether a socket was
# ever bound. So the gate starts the announcer on the loopback and asks it a
# real DESCRIBE.
set -uo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "==> Configure"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null || {
    echo "FAIL: configure" >&2; exit 1; }

echo "==> Build"
cmake --build build -j > /dev/null || { echo "FAIL: build" >&2; exit 1; }

echo "==> Tests"
ctest --test-dir build --output-on-failure \
    -R "RtspMessages|DnsSd|SessionCatalogue|Json|ConnectionApi|ReceiverRouting|ChannelMappingApi|NodeApi|RavennaKitParity" || {
    echo "FAIL: tests" >&2; exit 1; }

echo "==> A DESCRIBE over the loopback"
# The loopback and not a real interface: the point is to prove the server
# answers, not to advertise a session that does not exist onto somebody's
# network.
port=18999
nmos_port=18998
# The loopback is lo0 on macOS and lo on Linux, and mdns.start() takes the name
# and fails on the one that is not there -- which is how this gate passed here
# for months and failed the first time a runner ran it: the announcer never
# started, and every check after it read an empty answer.
loopback=lo0
[ "$(uname -s)" = "Linux" ] && loopback=lo
./build/ravenna-announce --interface "$loopback" --address 127.0.0.1 --name GateSession \
    --rtsp-port "$port" --nmos-port "$nmos_port" \
    --ptp-gmid 00-1D-C1-FF-FE-00-00-01 > /dev/null 2>&1 &
announcer=$!

# The DESCRIBE itself is the wait. Two seconds of sleep was enough on an idle
# machine and not enough on one building something else at the same time: this
# gate failed exactly once today, during a run with a sanitizer build in
# parallel, and passed three times in a row on its own.
#
# Retried rather than probed. `nc -z` opens a TCP connection, and this server
# takes one request per connection -- the probe IS the request, and the real
# DESCRIBE that followed it got nothing. Asking the question again costs the
# same and answers it.
answer=""
for _ in $(seq 1 40); do
    answer=$(printf 'DESCRIBE rtsp://127.0.0.1:%s/by-name/GateSession RTSP/1.0\r\nCSeq: 1\r\n\r\n' \
        "$port" | nc -w 2 127.0.0.1 "$port")
    case "$answer" in *"RTSP/1.0 200 OK"*) break ;; esac
    sleep 0.25
done

echo "==> An IS-05 connection over the loopback"
# The whole exchange a controller performs to assign one device's stream to
# another's channels: read the sender's transport file, stage it on the
# receiver, activate it. Passing it means the SDP went through the API, the
# receiver accepted it and the routing matrix found room.
base="http://127.0.0.1:$nmos_port/x-nmos/connection/v1.1/single"
node_base="http://127.0.0.1:$nmos_port/x-nmos/node/v1.3"

# The ids come from IS-04, which is where a controller reads them and which
# publishes exactly what IS-05 answers to. A gate that knew how they were made
# would still pass on the day the two APIs stopped agreeing, which is the one
# thing this part is here to catch.
sender_id=$(curl -s "$node_base/senders/" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)[0]["id"])')
receiver_id=$(curl -s "$node_base/receivers/" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)[0]["id"])')

sdp=$(curl -s "$base/senders/$sender_id/transportfile/" \
    | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))')
activated=$(curl -s -X PATCH -H 'Content-Type: application/json' \
    -d "{\"master_enable\":true,\"transport_file\":{\"data\":$sdp,\"type\":\"application/sdp\"},\"activation\":{\"mode\":\"activate_immediate\"}}" \
    "$base/receivers/$receiver_id/staged/")

echo "==> An IS-08 cell moved over the loopback"
# The grid, after the connection: move one channel of the stream the receiver
# just took onto device channel 64. Passing it means the API reached the same
# matrix the connection wrote to.
grid_base="http://127.0.0.1:$nmos_port/x-nmos/channelmapping/v1.0"
moved=$(curl -s -X POST -H 'Content-Type: application/json' \
    -d "{\"activation\":{\"mode\":\"activate_immediate\"},\"action\":{\"device\":{\"64\":{\"input\":\"$receiver_id\",\"channel_index\":1}}}}" \
    "$grid_base/map/activate")

echo "==> The node describing itself over the loopback"
# What a controller reads first. If this is wrong the device is not on the
# list at all, and nothing else here matters.
node=$(curl -s "http://127.0.0.1:$nmos_port/x-nmos/node/v1.3/senders/")

kill "$announcer" 2> /dev/null
wait "$announcer" 2> /dev/null

case "$answer" in
  *"RTSP/1.0 200 OK"*"application/sdp"*"s=GateSession"*"ts-refclk"*)
    echo "the DESCRIBE answered with the session's SDP" ;;
  *)
    echo "FAIL: the announcer did not describe its own session" >&2
    echo "$answer" >&2
    exit 1 ;;
esac

case "$activated" in
  *'"mode":"activate_immediate"'*'"master_enable":true'*)
    echo "the receiver took the stream and the channels were assigned" ;;
  *)
    echo "FAIL: the IS-05 activation did not go through" >&2
    echo "$activated" >&2
    exit 1 ;;
esac

case "$moved" in
  *"\"64\":{\"channel_index\":1,\"input\":\"$receiver_id\"}"*)
    echo "the channel moved to device channel 64" ;;
  *)
    echo "FAIL: the IS-08 activation did not move the channel" >&2
    echo "$moved" >&2
    exit 1 ;;
esac

case "$node" in
  *'"label":"GateSession"'*'"transport":"urn:x-nmos:transport:rtp.mcast"'*)
    echo "the node lists its sender" ;;
  *)
    echo "FAIL: the node API did not list the session as a sender" >&2
    echo "$node" >&2
    exit 1 ;;
esac

# ---------------------------------------------------------------------------
# The analysis half, under AES67_ANALYSE=1: what the plain run above cannot
# afford on every push. Each step is its own build directory so the ordinary
# build keeps its optimised binaries.
#
#   cppcheck     a second engine over the sources; SKIP where there is none
#   sanitizers   the same suites under ASan and UBSan, in build-san
#   fuzz         the parsers, fed mutated and random input, in that build
#   coverage     line coverage of the package's own sources, printed only
# ---------------------------------------------------------------------------
if [ "${AES67_ANALYSE:-0}" = "1" ] || [ "${analyse:-0}" = "1" ]; then
  jobs="$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

  echo "==> Static analysis"
  scripts/check-tidy.sh build || { echo "FAIL: clang-tidy" >&2; exit 1; }

  # Found the way check-tidy.sh finds clang-tidy: neither ships with the
  # Command Line Tools, and both live in ~/.local/venvs/cpptools on this
  # machine. CPPCHECK=/path/to/cppcheck overrides.
  cppcheck_bin="${CPPCHECK:-}"
  if [ -z "$cppcheck_bin" ]; then
    for candidate in "$HOME/.local/venvs/cpptools/bin/cppcheck" "$(command -v cppcheck || true)"; do
      [ -x "$candidate" ] && { cppcheck_bin="$candidate"; break; }
    done
  fi

  echo "==> cppcheck"
  if [ -n "$cppcheck_bin" ]; then
    # -U rather than a suppression: cppcheck analyses every branch of an #if,
    # and Profiles/ProfileLog.h's is `#include AES67_PROFILES_LOG_HEADER`,
    # which is not a header when the macro is undefined. Nothing in this tree
    # defines it, so saying so is the truth rather than a silenced check.
    "$cppcheck_bin" --quiet --error-exitcode=1 --enable=warning,performance,portability \
      --inline-suppr --std=c++20 --language=c++ -U AES67_PROFILES_LOG_HEADER \
      --suppress=missingInclude --suppress=missingIncludeSystem \
      --suppress=normalCheckLevelMaxBranches \
      -I . -I ../aes67-core -I ../aes67-profiles Ravenna/ || { echo "FAIL: cppcheck" >&2; exit 1; }
  else
    echo "SKIP: no cppcheck found"
  fi

  echo "==> Sanitizers (address, undefined)"
  rm -rf build-san
  cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DAES67_SANITIZE=address,undefined > /dev/null \
    || { echo "FAIL: sanitizer configure" >&2; exit 1; }
  cmake --build build-san -j"$jobs" > /dev/null || { echo "FAIL: sanitizer build" >&2; exit 1; }
  ( cd build-san && UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
      ctest --output-on-failure -LE "timing|network" ) || { echo "FAIL: tests under sanitizers" >&2; exit 1; }

  echo "==> Fuzzing the parsers"
  build-san/FuzzRavennaParsers "${AES67_FUZZ_ITER:-200000}" || { echo "FAIL: fuzzing" >&2; exit 1; }


  echo "==> Coverage"
  rm -rf build-cov
  cmake -S . -B build-cov -DCMAKE_BUILD_TYPE=Debug -DAES67_COVERAGE=ON > /dev/null \
    || { echo "FAIL: coverage configure" >&2; exit 1; }
  cmake --build build-cov -j"$jobs" > /dev/null || { echo "FAIL: coverage build" >&2; exit 1; }
  ( cd build-cov && ctest -LE "timing|network" > /dev/null ) || { echo "FAIL: tests under coverage" >&2; exit 1; }
  # gcov with -p keeps the path in the output name, which is what lets the
  # summary count this package's sources and nobody else's.
  gcov_tool="gcov"; command -v xcrun > /dev/null 2>&1 && gcov_tool="xcrun llvm-cov gcov"
  ( cd build-cov && find . -name '*.gcda' -print0 | xargs -0 $gcov_tool -p > /dev/null 2>&1; \
    ls *.gcov 2>/dev/null | grep '#packages#aes67-ravenna#' | grep -vE '#Tests#|#external#|#doctest#|#build' \
    | LC_ALL=C xargs -r awk -F: '/^ *#####:/ { miss++ } /^ *[0-9]+[*]?:/ { hit++ } \
      END { t = hit + miss; if (t == 0) { print "coverage: nothing measured"; exit 0 } \
            printf "coverage: %d/%d lines of aes67-ravenna, %.1f%%\n", hit, t, 100 * hit / t }' )
  rm -f build-cov/*.gcov
else
  echo "==> Analysis skipped: cppcheck, sanitizers, fuzzing and coverage (AES67_ANALYSE=1 to run them)"
fi

echo "==> PASS"
