# aes67-ravenna

RAVENNA's session layer: how a device finds what this machine offers, and how
it asks what a session is.

RAVENNA does not stream over RTSP. What it uses RTSP for is one question --
"describe the session you advertised" -- answered with the SDP of a multicast
stream that is already on the wire whether anybody asked or not. Discovery is
DNS-SD over mDNS. Those two things are this package, and nothing else is.

## What it is not

Not a Dante emulation. Dante's discovery, control and transport are
proprietary and licensed, and there is no clean implementation of them to
write. What is interoperable with Dante gear is AES67 mode, which is what
`packages/aes67-profiles` and the driver's `Tools/DanteInteropSim.cpp` are
about; this package is the RAVENNA side of the same idea, and RAVENNA is built
on standards a reader can look up: RFC 6762, RFC 6763, RFC 2326, RFC 4566,
RFC 7273.

Not a second SDP writer either, and not a second routing matrix. The SDP is
`aes67-core`'s `SDPParser::generate`, which already writes what RAVENNA wants,
`a=framecount` included. The channels are `aes67-core`'s
`StreamChannelMapper`: the 128-channel matrix, with its overlap prevention and
its per-channel routing. A session here is those two joined, and the catalogue
refuses to hold one where they disagree -- a session advertising eight
channels while the device puts two on the wire is a fault neither side sees.

## What it does

- **Advertises** each session as DNS-SD records: a PTR under `_rtsp._tcp`, a
  second PTR under RAVENNA's own `_ravenna_session` subtype -- which is what
  separates a RAVENNA session from a camera on the same service -- an SRV
  naming the host and RTSP port, a TXT carrying the path and the channel
  count, and an A.
- **Answers** queries for either name, unprompted announcements when a session
  appears, and a goodbye with a zero TTL when it stops, so a browser drops the
  session now rather than in 75 minutes.
- **Serves** `OPTIONS` and `DESCRIBE`. Everything else gets 501, a path that
  names no session gets 404, and what is not RTSP gets no answer at all.

There is no SETUP, no PLAY and no session state, because a multicast stream
does not need any: the device DESCRIBEs, reads the group out of the SDP, and
joins it.

## Assigning one device's stream to another's channels

Advertising a session says what exists; it does not connect anything. What
tells a receiver to take a particular stream is NMOS **IS-05**, served here at
`/x-nmos/connection/v1.1/`, and it is the standard version of what a
controller does when you drag one device onto another.

The exchange is three requests:

    base=http://192.168.1.50:8080/x-nmos/connection/v1.1/single

    # 1. what the sender offers
    curl $base/senders/sender-Mix%20A/transportfile/

    # 2. stage it on the receiver, and activate
    curl -X PATCH -H 'Content-Type: application/json' -d '{
      "master_enable": true,
      "transport_file": {"data": "<the SDP>", "type": "application/sdp"},
      "activation": {"mode": "activate_immediate"}
    }' $base/receivers/receiver-1/staged/

    # 3. what actually happened
    curl $base/receivers/receiver-1/active/

Activating is where the channels are assigned. `Ravenna/ReceiverRouting.h` is
what joins the two -- a receiver, named by the connection API, holding a
mapping the matrix keys by a `StreamID` -- and `aes67-core`'s
`StreamChannelMapper` is what assigns them: the SDP's channel count goes to the
matrix, the matrix finds a contiguous block on the 128-channel device, and a
receiver that does not fit is refused with the reason rather than left half
connected. Disabling frees the block, and pointing a receiver at another
stream replaces its connection instead of holding two.

What is implemented is single senders and receivers with immediate
activation. Scheduled activation answers 501 and says so: accepting one and
never acting on it is a connection a controller believes it has made and
nobody is carrying. Bulk answers 501 too, which sends a controller to the
single endpoints.

## Being on the list at all

A controller has to find the device and know what it is made of before it can
route anything. That is **IS-04**, served at `/x-nmos/node/v1.3/`: the node,
its device, and for each session a source, a flow and a sender, with a receiver
for what it can take.

    node=http://192.168.1.50:8080/x-nmos/node/v1.3

    curl $node/self/       # the node, its clock, where its APIs are
    curl $node/senders/    # one per session, with the SDP's address
    curl $node/receivers/  # what it can be given

Served, not registered. IS-04 has two modes and this is the peer-to-peer one:
the node advertises itself over mDNS as `_nmos-node._tcp` and a controller
browsing the link reads it here. Registering with a registry is an HTTP client
and a heartbeat, and it is not pretended at.

The ids are derived from the names rather than drawn at random, so a restart
does not renumber a plant: a controller keys everything on them, and ids that
moved would make every route point at something that no longer exists.

The resources are built from what the device holds -- the session catalogue,
the connection API's receivers -- rather than kept beside it. Two descriptions
of one device disagree the moment one of them changes.

## The grid, channel by channel

IS-05 says which stream a receiver takes. **IS-08**, served at
`/x-nmos/channelmapping/v1.0/`, says where each of that stream's channels
lands -- one cell at a time, which is the part of a routing controller that
looks like a grid.

    grid=http://192.168.1.50:8080/x-nmos/channelmapping/v1.0

    # what there is to route: inputs are the streams receivers took,
    # the output is this device's channels
    curl $grid/io/

    # the grid as it stands
    curl $grid/map/active/

    # move one cell: channel 2 of receiver-1 onto device channel 64
    curl -X POST -H 'Content-Type: application/json' -d '{
      "activation": {"mode": "activate_immediate"},
      "action": {"device": {"64": {"input": "receiver-1", "channel_index": 1}}}
    }' $grid/map/activate

The grid is not a second copy of anything: it is read from and written to
`StreamChannelMapper`'s per-channel routing, which has been there all along
with no way to reach it from outside the machine. Whatever fed a device
channel stops feeding it when a cell is rewritten -- two inputs on one output
channel is not a mix, it is a fault -- and a grid the matrix refuses leaves the
device carrying exactly what it was carrying.

One output block, called `device`, because this device's channels are one
array. Saying two would be describing hardware that is not there.

## Trying it

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j
    ./build/ravenna-announce --interface en0 --address 192.168.1.50 \
        --name "Mix A" --group 239.69.1.10 --channels 2 \
        --ptp-gmid 00-1D-C1-FF-FE-00-00-01

It sends no audio. The stream an SDP describes is somebody else's job -- the
Teensy box, the macOS driver -- and this is the half that makes it findable.

Pass `--ptp-gmid`: without it the SDP carries no `a=ts-refclk`, which says
which clock the timestamps are against, and a receiver that requires one --
RAVENNA gear does -- will not lock to the stream. The tool says so on start-up
rather than leaving it to be discovered on site.

RTSP's own port is 554 and needs privilege; anything above 1024 does not, and
discovery still works because the SRV record carries whichever port was bound.

The controller for this is the macOS Manager app's Network Routing sheet
(`packages/aes67-macos-driver`, ManagerApp): it browses `_nmos-node._tcp`, reads
IS-04 from every node it finds — this announcer among them — and draws senders
against receivers as a matrix, so a crosspoint click is the IS-05 PATCH that
this tool's `[ravenna] receiver ...` lines report. Nothing has to be typed at
the API by hand.

## Against Dante Controller

`Docs/dante-controller-parity.md` sets out, part by part, what Dante
Controller does, which standard covers it and what is implemented here. The
short version is that everything which is routing has an answer, because NMOS
defines one, and everything which is Audinate's own does not.

## What is checked

`scripts/gate.sh` builds the package, runs three suites -- the RTSP messages,
the DNS records byte for byte, and the catalogue -- and then starts the
announcer on the loopback and asks it a real DESCRIBE. That last part is not a
unit test on purpose: everything here happens between two machines, and a
suite that checks bytes cannot tell whether a socket was ever bound.

It has never been run against RAVENNA hardware. This line stays until it has.
