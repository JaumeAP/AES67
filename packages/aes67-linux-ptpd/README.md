# aes67-linux-ptpd

A PTP grandmaster for Linux, written for a Raspberry Pi 5.

The Pi 5's ethernet controller keeps a PTP hardware clock. That is the whole
reason this package exists: the kernel exposes it as `/dev/ptpN`, the NIC
stamps packets against it, and a master that announces that clock is
announcing the same time its timestamps are taken from. A daemon reading
`CLOCK_REALTIME` instead would be announcing one clock and stamping with
another.

It announces itself and nothing else. There is no BMCA here and no slave side:
this is the clock at the top, and a second master on the segment is reported
rather than negotiated with. What is not implemented is not half implemented.

## What it sends

- **Announce**, at the profile's rate, carrying the dataset a BMCA elsewhere
  compares: priority1, priority2, the clock class and accuracy, and this
  clock's identity, built from the interface's MAC.
- **Sync** and **Follow_Up**, two-step. The Sync goes out and the NIC reports
  when it left; that number is what the Follow_Up carries. When no transmit
  timestamp comes back, no Follow_Up is sent at all -- a Follow_Up carrying a
  time nobody measured is worse than a missing one.
- **Delay_Resp**, to each Delay_Req, carrying the hardware receive timestamp
  and the requester's port identity.

## Locking it to a reference

A NIC clock left alone free-runs on its crystal. Every device on the network
then agrees with it to the nanosecond and the whole network drifts together,
away from the studio. `--reference` is what fixes that:

    sudo ./build/aes67-ptpd --interface eth0 --profile aes67 --reference

It asks the PHC for external timestamps (`PTP_EXTTS_REQUEST2`) and steers the
clock to the edges it gets, through `clock_adjtime`. The servo is not a new
one: it is `packages/t41-ptp`'s `ptp-servo`, the same loop the Teensy box runs
against its word clock, with the same gains and the same 100 ppm bound.

The edge has to be stamped by the PHC, not by the kernel. That is a separate
capability from stamping packets -- a NIC can do one and not the other -- so
check before wiring anything:

    cat /sys/class/ptp/ptp0/n_ext_ts

Zero means this path does not exist on that board, and the daemon says so and
stops rather than falling back to `/dev/pps`, which stamps against
`CLOCK_REALTIME`: disciplining one clock while announcing another is the error
the whole design is trying to avoid.

What it wants on that pin is one pulse per second. A word clock is 48 kHz and
has to be divided down first, in hardware -- on a Pi 5 the RP1's PIO can do it
with no CPU and no interrupts.

While it is locked the daemon announces clockClass 13, "synchronised to an
application-specific source", and timeSource OTHER. Not clockClass 6: that
means a primary reference such as GPS, and a word clock gives frequency and a
boundary, never traceable absolute time. Unlocked it goes back to 248, which
is what the log line says when it changes.

## What it says while it runs

One line a second, on stdout, which is the journal under systemd. A daemon
that only speaks when something is wrong cannot be told apart from a stopped
one.

    [ptpd] external  clockClass 13  announce 42  sync 336  delay_resp 12  no-followup 0  hw  offset +37 ns  drift -1240.5 ns/s  edges 42 (0 dropped, last 0.3 s ago)

The first word is where the time is coming from, and there are three:

- `internal` -- the NIC's own crystal, free-running. No reference configured.
- `waiting` -- a reference is configured and the clock is not following it:
  no edge has arrived yet, the servo has not settled, or the edges stopped.
- `external` -- locked to the pulse arriving on the PHC's input.

`no-followup` counts the Syncs that went out with no transmit timestamp and so
were never followed up. It should stay at zero; a number that climbs means the
NIC is not returning stamps and the slaves are getting nothing to correct
against.

After three seconds with no edge a locked clock stops calling itself locked
and goes back to announcing 248, while holding the frequency the servo last
set. One missed edge is a glitch, three is a cable.

## The numbers come from the shared table

The domain, the sdoId and the three intervals are not in this package. They are
`packages/aes67-profiles`, which the Teensy firmware and the macOS driver read
too, so what this announces and what they expect cannot drift apart.

    --profile aes67         8 Sync/s, 1 Announce/s, 8 Delay_Req/s
    --profile aes67-tight   16 Sync/s, 1 Announce/s, 16 Delay_Req/s
    --profile default1588   1 Sync/s, 1 Announce every 2 s
    --profile gptp          802.1AS's numbers, majorSdoId 1

What this package decides for itself is what describes the clock rather than
the ecosystem: `--priority1`, `--priority2` and the announced quality.

## Getting it onto the Pi without building it

Every push builds it on a native arm64 runner -- the Pi 5's own architecture,
no cross toolchain and no emulation -- and leaves a `.deb` on the run. Open the
`linux-ptpd` workflow on GitHub, take the newest green run, download the
`aes67-ptpd-arm64` artifact, and on the Pi:

    unzip aes67-ptpd-arm64.zip
    sudo dpkg -i aes67-ptpd-*.deb
    sudo systemctl enable --now aes67-ptpd

The package puts the binary in `/usr/local/bin` and the unit in
`/usr/local/lib/systemd/system`. Building it yourself is below, and is what to
do if you are changing anything.

## Building on the Pi

    sudo apt install build-essential cmake git uuid-dev
    git clone --recursive https://github.com/JaumeAP/AES67.git
    cd AES67/packages/aes67-linux-ptpd
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j
    sudo cmake --install build

`cmake --install` puts the binary in `/usr/local/bin` and the unit in
`/lib/systemd/system`.

## Running it

    sudo systemctl enable --now aes67-ptpd

Or by hand, which is what to do the first time:

    sudo ./build/aes67-ptpd --interface eth0 --profile aes67 --verbose

Two privileges are needed and no others: `CAP_NET_ADMIN` to turn the NIC's
timestamping on through `SIOCSHWTSTAMP`, and `CAP_NET_BIND_SERVICE` for UDP 319
and 320. The unit in `systemd/` grants exactly those.

If the interface has no PTP hardware clock, or the kernel refuses to enable
stamping, the daemon stops and says so. `--allow-software-timestamps` runs it
anyway on kernel timestamps, which is useful for seeing traffic on a laptop and
is not a way to run a grandmaster.

Check what the interface offers before blaming the daemon:

    ethtool -T eth0

## What is checked, and where

`scripts/gate.sh` builds the package and runs `Tests/TestPtpWire.cpp`, which
checks the byte layout of all four messages offset by offset against IEEE
1588-2008 section 13. That suite builds anywhere, which is the reason
`src/PtpWire.cpp` knows nothing about sockets: a wrong offset is invisible on
the wire -- a slave does not complain about a Follow_Up whose timestamp sits
two octets late, it is simply wrong about the time.

The Linux half -- `PtpSockets.cpp`, `PhcClock.cpp` -- is compiled by CI on
Linux (`.github/workflows/linux-ptpd.yml`). It has never been run against real
PTP slaves on real hardware, and this README will say so until it has.
