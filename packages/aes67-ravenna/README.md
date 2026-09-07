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

## What is checked

`scripts/gate.sh` builds the package, runs three suites -- the RTSP messages,
the DNS records byte for byte, and the catalogue -- and then starts the
announcer on the loopback and asks it a real DESCRIBE. That last part is not a
unit test on purpose: everything here happens between two machines, and a
suite that checks bytes cannot tell whether a socket was ever bound.

It has never been run against RAVENNA hardware. This line stays until it has.
