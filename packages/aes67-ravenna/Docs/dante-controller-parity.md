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
| A central registry every device reports to | IS-04 Registration API | **Done.** `packages/aes67-macos-driver/NetworkEngine/Discovery/NMOSRegistrationClient.h`, off unless the installation asks |
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

## The controller

`packages/aes67-macos-controller` is the screen: a separate application,
because routing a plant and managing this machine's audio device are different
jobs with different audiences, and an installer should not have to install a
driver to open a matrix. It shows

1. **Devices** — every node, its senders and receivers, which of IS-05 and
   IS-08 it will let a controller use, and the clock it follows with its lock
   state. That last column is not decoration: a device passing audio while
   locked to nothing drifts, which reads as a routing fault and is not one.
2. **Sessions** — everything on the network however it announces itself, SAP
   and RTSP found by the application itself through the driver's discovery
   bridge, NMOS senders merged in by the destination their transport file
   names.
3. **Routing** — the IS-05 crosspoints, device by device.
4. **Channels** — the IS-08 grid, channel by channel, for the devices that
   declare a channel mapping control.

Writing that client is what found three places where this repository's own
IS-08 server did not match the specification a controller reads: `map/active`
answered under the key `action`, which is what a POST to `map/activate` sends
rather than what a GET returns, and `io` put a port's `name` and
`description`, and its `block_size` and `reordering`, at the top level instead
of under `properties` and `caps`. A grid drawn from the spec came out empty
and unnamed. Fixed, with the suite holding the shapes.

## What is still out of reach

Presets, latency and error counters, device lock, identify and firmware are
Audinate's own and have no standard equivalent to implement. Renaming a device
over the network is not something IS-04 defines either -- what a controller
shows there is what the device calls itself.
