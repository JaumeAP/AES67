# ravenna-alsa-lkm

Not a package: a checkout, under `external/` beside doctest, and this file.
It was `packages/ravenna-alsa-lkm` -- three files of wrapper around a
submodule -- until the wrapper stopped earning its place.

The RAVENNA/AES67 ALSA kernel module: Merging Technologies' driver, through
[`bondagit/ravenna-alsa-lkm`](https://github.com/bondagit/ravenna-alsa-lkm),
vendored under `external/` as a submodule, pinned to the **`aes67-daemon`**
branch.

Nothing is written here. What this package holds is the checkout, the one
command that builds it and the gate that says whether it is there.

## The user-space half, and which one

The module is half a device. Merging's own README says so: the kernel part
registers the ALSA device, generates and receives the RTP packets and runs the
PTP-driven interrupt loop; everything above that -- mDNS and SAP discovery,
NMOS IS-04/05, sample-rate arbitration between devices, the REST API -- lives
in a user-space process, and the module does nothing without one.

There are two of those, and this tree uses the second:

- **The Butler** (`Merging_RAVENNA_Daemon`), which the checkout carries under
  `external/ravenna-alsa-lkm/Butler/`. It is a 5.5 MB ELF binary, not source,
  under a licence of its own (`Butler/LICENSE.md`) rather than the module's
  GPL -- and the public build is **limited to 8 inputs and outputs unless a
  Merging device is present**.

  It is not on this disk. the tree's `scripts/gate.sh` sets a `sparse-checkout` on the
  submodule that leaves `Butler/` out, which takes the checkout from 8.7 MB to
  820 KB and means everything present here builds from source. Deleting it is
  not an option -- it is tracked in `bondagit`'s repository, not ours -- so
  not fetching it is. To see it anyway:

  ```bash
  git -C external/ravenna-alsa-lkm sparse-checkout disable
  ```
- **[`bondagit/aes67-linux-daemon`](https://github.com/bondagit/aes67-linux-daemon)**,
  GPL-3.0, which drives the same module over the same netlink interface
  (`daemon/netlink.hpp`, `daemon/driver_handler.cpp`) and implements the same
  session layer in the open. No channel limit, and the source is readable --
  which is what let `packages/aes67-macos-driver` mirror its SAP receive path
  and its SDP writer in its interop suites rather than guess at them.

The branch pin is what makes that choice real rather than nominal. `master` is
Merging's module as it stands; `aes67-daemon` is the same module with the
patches that daemon needs, and it is what the pinned commit is on. Without the
pin an update would quietly move to a module the free daemon does not drive.

Building and running the daemon is its business, from its own checkout:

```bash
git clone https://github.com/bondagit/aes67-linux-daemon.git
# then follow its README: it wants Boost, Avahi and this module inserted first
sudo insmod external/ravenna-alsa-lkm/driver/MergingRavennaALSA.ko
```

## Why it is its own package

It is its own thing. This is kernel C, built by the kernel's own build system
against the running kernel's headers; the daemon above it is a user-space
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
scripts/gate.sh   # the tree gate carries what this package's own used to do
```

On Linux with kernel headers it builds the module and checks a
`MergingRavennaALSA.ko` came out. Without headers, or off Linux, it checks the
checkout is there and says what it skipped.

## Licence

GPL-2.0, which is what `MODULE_LICENSE("GPL v2")` in `driver/` declares and
what the upstream `README.md` links. See `NOTICE` at the root of this
repository.
