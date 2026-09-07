# What Dante Controller does, and what answers it here

Dante Controller is the thing people mean when they say "routing": a list of
devices, a grid to subscribe channels in, and a clock readout. None of that is
one protocol. This is what each part of it is, which standard covers it, and
what this repository has.

The short version: everything that is routing has an answer, because NMOS
defines one. Everything that is Audinate's own -- the discovery protocol, the
presets, the diagnostics -- does not, and pretending otherwise would mean
reverse-engineering a licensed protocol.

| Dante Controller does | The standard for it | Here |
|---|---|---|
| Lists devices, with names and models | IS-04 Node API | **Done.** `Ravenna/NodeApi.h`, served at `/x-nmos/node/v1.3/` |
| Drag one device onto another | IS-05 Connection API | **Done.** `Ravenna/ConnectionApi.h`, at `/x-nmos/connection/v1.1/` |
| The grid, channel by channel | IS-08 Channel Mapping API | **Done.** `Ravenna/ChannelMappingApi.h`, at `/x-nmos/channelmapping/v1.0/` |
| Shows which device is clock master, and lock state | IS-04, a node's `clocks` | **Done**, in `self/`. The daemon that measures it is `packages/aes67-linux-ptpd` |
| Channel labels | IS-04 source channels, IS-08 input channels | **Done** |
| Device name, sample rate, encoding | IS-04 resources and the SDP | **Done**, read-only: nothing here renames a device over the network |
| Finds devices without configuration | IS-04 peer-to-peer, `_nmos-node._tcp` over mDNS | **Done** |
| A central registry every device reports to | IS-04 Registration API | **Not done.** A registry client is an HTTP client and a heartbeat |
| Routing presets, saved and recalled | Nothing standard: Audinate's own | **Not done** |
| Latency, bandwidth, error counters | Nothing standard equivalent | **Not done.** The PTP daemon reports its own state once a second |
| Device lock, identify, firmware | Nothing standard equivalent | **Not done** |
| Talking to Dante devices as Dante | Proprietary and licensed | **Never.** See below |

## Why there is no Dante mode

Dante's discovery, its control protocol and its transport are Audinate's, and
they are licensed rather than published. There is no clean implementation to
write and reverse-engineering one is a legal problem before it is a technical
one.

What is interoperable is **AES67 mode**, which Dante devices have and which is
standard on the wire: SAP/SDP, RTP L24 multicast in Dante's own group range,
PTPv2 on a domain. `packages/aes67-profiles` holds what each ecosystem
restricts, and `packages/aes67-macos-driver/Tools/DanteInteropSim.cpp` checks
this driver's SAP and SDP against what Dante Controller itself needs, read out
of the Controller bundle rather than guessed.

So a Dante device with AES67 mode on can subscribe to what this publishes, and
this can subscribe to what it publishes. What it cannot do is appear in Dante
Controller's own device list, because that list is Dante's protocol and not
AES67's.

## What is still missing to route a plant from one screen

Two things, and the second needs the first only to be pleasant:

1. **A registry client.** Peer-to-peer discovery finds what is on the link. A
   plant with several subnets wants a registry, which means POSTing the node's
   resources to `/x-nmos/registration/v1.3/resource` and heartbeating.
2. **A controller.** Something that browses, draws the grid and writes to
   IS-05 and IS-08. It is an application, not a package: everything it needs to
   read and write is served already.
