# ravenna-alsa-lkm

The RAVENNA/AES67 ALSA kernel module: Merging Technologies' driver, through
[`bondagit/ravenna-alsa-lkm`](https://github.com/bondagit/ravenna-alsa-lkm),
vendored under `external/` as a submodule.

Nothing is written here. What this package holds is the checkout, the one
command that builds it and the gate that says whether it is there -- the same
posture `packages/aes67-linux-daemon` takes towards the daemon.

## Why it is its own package

It is its own thing. This is kernel C, built by the kernel's own build system
against the running kernel's headers; the daemon beside it is a user-space
process with Boost and Avahi. They are the two halves of one device and they
fail in different ways: a daemon that will not start and a module that will
not load are not the same morning's work.

It also has readers other than the daemon. The PTP slave in `driver/PTP.c` is
what `packages/aes67-linux-ptpd`'s interop suite mirrors -- that is where the
rules a grandmaster has to satisfy are written down -- and its RTP path is
what the core's own is measured against.

## What the module does

A virtual ALSA device whose playback and capture streams are RTP on the wire.
It carries its own PTP slave clock and clocks every source and sink from it,
and it talks to the daemon over netlink for configuration and status. See
`external/ravenna-alsa-lkm/README.md`.

## Building

```bash
git submodule update --init --recursive
cmake -S . -B build
cmake --build build --target ravenna-alsa-lkm
```

The target is deliberately not part of `all`: building a module wants
`/lib/modules/$(uname -r)/build`, which a container or a cross build may not
have.

## Verification

```bash
packages/ravenna-alsa-lkm/scripts/gate.sh
```

On Linux with kernel headers it builds the module and checks a
`MergingRavennaALSA.ko` came out. Without headers, or off Linux, it checks the
checkout is there and says what it skipped.

## Licence

GPL-2.0, which is what `MODULE_LICENSE("GPL v2")` in `driver/` declares and
what the upstream `README.md` links. See `NOTICE` at the root of this
repository.
