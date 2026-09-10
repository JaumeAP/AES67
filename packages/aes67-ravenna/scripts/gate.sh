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
    -R "RtspMessages|DnsSd|SessionCatalogue|Json|ConnectionApi|ReceiverRouting|ChannelMappingApi|NodeApi" || {
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
sleep 2

answer=$(printf 'DESCRIBE rtsp://127.0.0.1:%s/by-name/GateSession RTSP/1.0\r\nCSeq: 1\r\n\r\n' \
    "$port" | nc -w 2 127.0.0.1 "$port")

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

echo "==> PASS"
