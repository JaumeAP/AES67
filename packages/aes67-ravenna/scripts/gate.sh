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
ctest --test-dir build --output-on-failure -R "RtspMessages|DnsSd|SessionCatalogue" || {
    echo "FAIL: tests" >&2; exit 1; }

echo "==> A DESCRIBE over the loopback"
# The loopback and not a real interface: the point is to prove the server
# answers, not to advertise a session that does not exist onto somebody's
# network.
port=18999
./build/ravenna-announce --interface lo0 --address 127.0.0.1 --name GateSession \
    --rtsp-port "$port" --ptp-gmid 00-1D-C1-FF-FE-00-00-01 > /dev/null 2>&1 &
announcer=$!
sleep 2

answer=$(printf 'DESCRIBE rtsp://127.0.0.1:%s/by-name/GateSession RTSP/1.0\r\nCSeq: 1\r\n\r\n' \
    "$port" | nc -w 2 127.0.0.1 "$port")
kill "$announcer" 2> /dev/null
wait "$announcer" 2> /dev/null

case "$answer" in
  *"RTSP/1.0 200 OK"*"application/sdp"*"s=GateSession"*"ts-refclk"*)
    echo "answered with the session's SDP" ;;
  *)
    echo "FAIL: the announcer did not describe its own session" >&2
    echo "$answer" >&2
    exit 1 ;;
esac

echo "==> PASS"
