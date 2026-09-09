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

On Linux it configures, builds and runs the tests the daemon ships. On
anything else there is nothing to compile, so it checks the submodule is there
and pinned and says what it skipped.

## What is not here

The PTP grandmaster: that is `packages/aes67-linux-ptpd`, which announces the
NIC's own hardware clock. The daemon is a slave and wants a master on the
segment; the two are the two ends of the same link, not alternatives.
