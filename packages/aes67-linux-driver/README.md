# aes67-linux-driver

The Linux side of the same idea the macOS driver implements: an AES67 device a
Linux host offers to the network. None of it is written here. What this package
holds is [`bondagit/aes67-linux-daemon`](https://github.com/bondagit/aes67-linux-daemon)
as a submodule under `external/`, plus the build, the gate and the notes that
put it next to the other packages in this tree.

## What is vendored

| Piece | Where it comes from | What it does |
|-------|--------------------|--------------|
| `aes67-daemon` | `bondagit/aes67-linux-daemon` | The user-space half: SAP and mDNS discovery, the REST interface, the sources and sinks configuration, NMOS IS-04 |
| RAVENNA ALSA LKM | `bondagit/ravenna-alsa-lkm`, a submodule of the daemon | The kernel half, from Merging Technologies: a virtual ALSA device, the RTP streams and the PTP slave clock |
| `cpp-httplib` | `yhirose/cpp-httplib`, a submodule of the daemon | The HTTP server the REST interface is served from |

The daemon and the kernel module talk over netlink. The module is the PTP
slave and clocks every source and sink from that one clock; the daemon
configures it and reports its status.

Both are GPL. Their licence is theirs, not this repository's -- see
`external/aes67-linux-daemon/LICENSE`.

## What this package adds

`CMakeLists.txt` configures the vendored daemon with the options `build.sh`
uses and adds a `ravenna-alsa-lkm` target for the kernel module, which the
kernel's own build system builds and CMake does not. The module target is not
part of `all`: building a module wants the running kernel's headers.

Nothing here patches upstream. A change in behaviour belongs upstream, not in
a copy kept beside it.

## Writing the daemon's configuration from a profile

`aes67-profile-conf` takes a compatibility profile from `aes67-profiles` and
writes the daemon's `daemon.conf` for it:

```bash
./build/aes67-profile-conf --profile dolby --rate 96000 --interface eth0 -o daemon.conf
```

The base is the `daemon.conf` the vendored daemon ships, so every key the
profile does not determine keeps upstream's value rather than a copy of it
that ages. What a profile determines is five keys: `sample_rate`,
`tic_frame_size_at_1fs` (the packet time, as frames per packet at 1FS: 48 for
1 ms, 6 for 125 us), `ptp_domain`, `rtp_mcast_base` where the profile
documents an address, and `interface_name` when `--interface` is given.

It refuses rather than guessing: a rate or packet time the profile does not
allow, a base file missing a key it has to write, or a profile id that is not
one of ours -- `kindFromString` answers AES67 for anything it does not know,
which would turn a typo into a silently different configuration.

Two things it deliberately does not write:

- The profile's `recommendedDscp` is the marking of the **media**, and
  `daemon.conf`'s `ptp_dscp` is the marking of **PTP**. There is no media DSCP
  key in that file, so the profile's value goes nowhere and `ptp_dscp` keeps
  upstream's.
- Dante requires multicast inside 239.69.0.0/16 and documents no address to
  use. The tool says so and stops: upstream's base address is outside the
  range, and picking one inside it would be choosing a site's address for it.

The tool reads the profiles and a text file and nothing else, so it builds and
is tested wherever this repository is read, Linux or not.

## Building

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --build build --target ravenna-alsa-lkm   # wants the kernel headers
```

The daemon wants Boost (thread, filesystem, log, program_options), Avahi and
systemd headers. The web interface is a release download or an `npm` build --
see the upstream `README.md` and `build.sh`.

## Verification

```bash
packages/aes67-linux-driver/scripts/gate.sh
```

Everywhere it builds `aes67-profile-conf` and runs its suite. On Linux it also
configures, builds and runs the tests the daemon ships; anywhere else that half
is skipped and said to be skipped.

## What is not here

The PTP grandmaster: that is `packages/aes67-linux-ptpd`, which announces the
NIC's own hardware clock. The daemon is a slave and wants a master on the
segment; the two are the two ends of the same link, not alternatives.
