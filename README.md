# AES67

AES67 audio-over-IP: a macOS driver, the platform-free core it is built on,
and the Teensy PTP library a grandmaster box uses. Four packages, three of which came
from three repositories and are kept together because they are read together,
not because they build together.

## What is here

| Package | What it is | Built with |
|---------|------------|------------|
| [`packages/aes67-profiles`](packages/aes67-profiles) | The tables every implementation here has to agree on: the AoIP compatibility profiles, the Dolby per-model data and the PTP profiles. Freestanding headers -- no platform, no operating system, no other package -- so the macOS core and the Teensy firmware read the same numbers instead of keeping two copies | CMake, C++20 |
| [`packages/aes67-core`](packages/aes67-core) | The platform-free core: SDP parsing, the RTP wire header, the jitter buffer and packet pool, the media-clock PLL, the resampling chain, channel mapping, configuration. No Apple framework, no socket header, no operating system — checked, not just intended | CMake, C++20 |
| [`packages/aes67-macos-driver`](packages/aes67-macos-driver) | The macOS AudioServerPlugIn driver, the PTP daemon, the SwiftUI manager app and the tools | CMake, C++20 and Objective-C++ |
| [`packages/aes67-linux-ptpd`](packages/aes67-linux-ptpd) | A PTP grandmaster for Linux, written for a Raspberry Pi 5: it announces the NIC's own hardware clock, stamps its Sync messages with it and reads its rates from `aes67-profiles`. Announce, Sync/Follow_Up and Delay_Resp; no BMCA and no slave side | CMake, C++20 |
| [`packages/aes67-linux-daemon`](packages/aes67-linux-daemon) | The tools that configure the AES67 Linux daemon from the profiles: `aes67-profile-conf` writes its `daemon.conf` and its RTP sources, reads one back and says what is wrong with it, and holds our SDP against the daemon's own. The daemon is built from its own checkout, not vendored here | CMake, C++20 |
| [`packages/ravenna-alsa-lkm`](packages/ravenna-alsa-lkm) | The Linux device's kernel half: Merging Technologies' RAVENNA/AES67 ALSA module through [`bondagit/ravenna-alsa-lkm`](https://github.com/bondagit/ravenna-alsa-lkm), as a submodule. Its own package because a kernel module and a user-space process fail in different ways | Kernel Makefile |
| [`packages/aes67-ravenna`](packages/aes67-ravenna) | RAVENNA's session layer and the NMOS APIs a controller routes with: DNS-SD over mDNS and the RTSP DESCRIBE that hands over the SDP, IS-04 so the device is on the list, IS-05 to give one device another's stream, IS-08 for the grid channel by channel. The SDP is the core's and the grid is the core's matrix | CMake, C++20 |
| [`packages/t41-ptp`](packages/t41-ptp) | IEEE 1588 for the Teensy 4.1, a fork of `IMS-AS-LUH/t41-ptp`, carrying QNEthernet and TimeLib under `libraries/` | Arduino / PlatformIO |

The grandmaster firmware that consumes `t41-ptp` is not here: it is
[`JaumeAP/AES67-master-box-`](https://github.com/JaumeAP/AES67-master-box-),
standalone, and takes `t41-ptp` as a submodule.

## Verification

**Every package verifies itself.** Each has a gate of its own that builds it,
runs its tests and applies whatever checks that package needs — the core has a
platform-freedom contract, the driver has a CMake sanity check, `t41-ptp`
compiles for the board. None of them depends on another package's gate to be
covered.

`scripts/gate.sh` runs all three in order and stops at the first failure. It
owns no checks of its own; it knows which packages there are and nothing about
what any of them does.

```bash
scripts/gate.sh                    # the cheap half of every package: seconds
AES67_ANALYSE=1 scripts/gate.sh    # with clang-tidy where a package has it: minutes
```

Each package's gate can be run on its own, from anywhere:

```bash
packages/aes67-core/scripts/gate.sh
packages/aes67-macos-driver/scripts/gate.sh
packages/aes67-linux-ptpd/scripts/gate.sh
packages/aes67-linux-daemon/scripts/gate.sh
packages/ravenna-alsa-lkm/scripts/gate.sh
packages/aes67-ravenna/scripts/gate.sh
packages/t41-ptp/scripts/gate.sh
```

There is no CI. GitHub Actions was disabled and deleted, and `.githooks/pre-push`
is what replaced it: it runs `scripts/gate.sh` before every push, and asks
about the static analysis when the push is to the default branch. A fresh
clone has to opt in once, because hook configuration is local and does not
travel with a repository:

```bash
git config core.hooksPath .githooks
```

## Building

macOS only for the two CMake packages: `project()` declares OBJCXX, so
configuring fails on Linux before anything builds.

```bash
git clone --recurse-submodules https://github.com/JaumeAP/AES67.git
cd AES67
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

That builds the driver, which pulls the core in with it: 42 test suites, plus
the core's 19. `external/doctest` is the submodule those two share; the other is the RAVENNA
kernel module, which nothing on macOS reads.

`t41-ptp` is cross-compiled for an ARM Cortex-M7 and is not wired into the
CMake tree. Its host tests run on the Mac; its board build needs PlatformIO.

## Licences

The root `LICENSE` is GPL-3.0 and covers the code written for this project.
Everything vendored keeps the licence it arrived with. [`NOTICE`](NOTICE) says
which is which, subtree by subtree.
