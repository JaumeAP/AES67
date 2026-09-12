# aes67-macos-controller

The application that routes other people's devices.

Two programs, two audiences. `aes67-macos-manager` is about this machine: it
installs the driver and the PTP daemon, activates the audio device, picks the
compatibility profile, maps channels. This one is about the network: what
sessions are on it, and what is connected to what. An installer routing a room
should not have to install an audio driver to open a crosspoint matrix, which
is why this is a separate application and not a second window — Dante splits
Controller from Virtual Soundcard the same way, for the same reason.

## What it does

1. **Sessions.** Everything on the network, however it announces itself: SAP
   announcements, `_rtsp._tcp` services described over RTSP DESCRIBE, and
   NMOS IS-04 senders, merged into one list by the session's own identity. A
   session found three ways is one row that says so.
2. **Devices.** Every NMOS node, what it will let a controller use (IS-05,
   IS-08, or nothing) and the clock it follows with its lock state.
3. **Routing.** Every NMOS node as one matrix — senders across, receivers
   down, a click on a crosspoint connects or disconnects over IS-05.
4. **Channels.** The IS-08 grid for the devices that map channels: inputs
   across, outputs down, a click sets a crosspoint and a click on the one
   already set mutes it.
5. **Link.** Everything on the segment, whoever made it and however it makes
   itself known — registered services (`_rtsp._tcp`, `_nmos-node._tcp`,
   `_nmos-register._tcp`, Dante's `_netaudio-*._udp`, `_ravenna._tcp`) and PTP
   clocks. This is where gear appears that nothing here can control: a Dante
   device shows its registration, and Dolby Atmos Connect, which announces
   nothing at all because it is configured by hand end to end, shows its
   clock. Browsing a registration is not speaking a protocol.

## What it does not do

Nothing here talks Dante's protocol, or configures a Dolby processor, or
renames a device: listing is not control, and the protocols that would allow
it are either licensed or do not exist. No driver, no Core Audio, no audio
path. It installs nothing and needs nothing
installed. On a machine that also runs this project's driver, both listen on
the SAP port at once; that is what `SO_REUSEPORT` is there for.

## How it is built

Raw `swiftc`, like the Manager, from `build.sh`. It compiles three kinds of
source:

- **Its own** — `AES67ControllerApp.swift`, `Views/ControllerWindow.swift`,
  `Models/DiscoveryService.swift`.
- **The Manager's, by path** — `NmosResources.swift`, `NmosController.swift`,
  `SessionList.swift`, `RoutingMatrixView.swift`. One implementation, two
  applications; copying them is how two implementations start.
- **The driver's libraries** — `libaes67_net.a` and what it rests on, reached
  through `Bridging/Controller-Bridging-Header.h`, which exposes exactly one
  header: the discovery bridge. The driver package has to be built first, and
  `build.sh` says so by name when it is not.

```bash
cd ../aes67-macos-driver && cmake -S . -B build && cmake --build build   # once
cd ../aes67-macos-controller && ./build.sh --force
open AES67Controller.app
```

`scripts/gate.sh` builds it and checks the bundle carries the local-network
usage description and the Bonjour service types, without which macOS gives it
no multicast and no browsing at all. There are no host tests here on purpose:
what could be tested is tested where it lives — the session merge in the
Manager's `run-tests.sh`, the discovery behind the bridge in the driver's
`SessionDirectory`, `RTSPSessionDiscovery` and `DiscoveryBridge` suites.
