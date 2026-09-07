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

## Building on the Pi

    sudo apt install build-essential cmake git
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
