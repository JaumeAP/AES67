# aes67-linux-daemon

The user-space half of the Linux device: an AES67 device a Linux host offers
to the network. The kernel half is its own package,
`packages/ravenna-alsa-lkm`.

None of the daemon is written here. What this package holds is
[`bondagit/aes67-linux-daemon`](https://github.com/bondagit/aes67-linux-daemon)
as a submodule under `external/`, the build and the gate, and the tools that
turn this repository's compatibility profiles into the daemon's own
configuration.

## What is vendored

| Piece | Where it comes from | What it does |
|-------|--------------------|--------------|
| `aes67-daemon` | `bondagit/aes67-linux-daemon` | The user-space half: SAP and mDNS discovery, the REST interface, the sources and sinks configuration, NMOS IS-04 |
| `cpp-httplib` | [`bondagit/cpp-httplib`](https://github.com/bondagit/cpp-httplib), a submodule of this package | The HTTP server the REST interface is served from |

The kernel module is not here: it is `packages/ravenna-alsa-lkm`, its own
package with its own checkout and its own gate, because a kernel module and a
user-space process fail in different ways. This package builds the daemon
against that checkout -- `RAVENNA_ALSA_LKM_DIR` points at it.

Upstream pins the module a second time inside the daemon, under its
`3rdparty/`, and cpp-httplib the same way. Two checkouts of one thing, free to
drift, with only one ever compiled -- so the daemon this package tracks is a
fork with both submodules removed and the two pins kept here instead, [`JaumeAP/aes67-linux-daemon`](https://github.com/JaumeAP/aes67-linux-daemon)
branch `no-vendored-lkm`, one commit ahead of upstream and nothing else. The
gate fails if a rebase onto upstream ever brings it back.

Upstream's own `build.sh` still expects both under `3rdparty/`; building that
way means putting them there. This
package's CMake does not: it passes the module's location the way `build.sh`
always did.

The daemon and the kernel module talk over netlink. The module is the PTP
slave and clocks every source and sink from that one clock; the daemon
configures it and reports its status.

Both are GPL. Their licence is theirs, not this repository's -- see
`external/aes67-linux-daemon/LICENSE`.

## What this package adds

`CMakeLists.txt` configures the vendored daemon with the options `build.sh`
uses, against the module package's checkout.

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

### Writing the daemon's sources

```bash
./build/aes67-profile-conf --sources 4 --channels 2 --profile dante
```

Writes `{ "sources": [ ... ] }`, the body the daemon takes at
`PUT /api/source/:id`, one entry per source, with the channel map running
consecutively across them. Four fields come from the profile: `codec` (the
first encoding it allows, or `--codec` checked against them),
`max_samples_per_packet` (the packet time in samples), `dscp` -- the profile's
`recommendedDscp` belongs here and not in `daemon.conf`, because this is the
marking of the audio and `ptp_dscp` is the marking of PTP -- and `address`,
where the profile documents one. The rest are upstream's own example values:
`ttl` 15, `payload_type` 98, `io` "Audio Device", enabled and PTP-traceable.

The daemon documents five sizes for `max_samples_per_packet` -- 12, 16, 48, 96
and 192. A packet time that falls between two of them is rounded **up** to the
next, and the tool says so on standard error: ST 2110-30 Level B's 125 us at
48 kHz is six samples, so it writes 12 and reports a packet of 250 us. It
refuses outright when the profile forbids the rate, the packet time, the codec
or the channel count, and when the ids would run past 63.

### Comparing our SDP with the daemon's

`Tools/DaemonSdp.{h,cpp}` writes the SDP the vendored daemon announces, line
for line with `SessionManager::get_source_sdp_`
(`daemon/session_manager.cpp:734`), which cannot be called from here: it is a
private method of a class holding a netlink handle, a driver and a PTP state.
`Tools/SdpCompare.{h,cpp}` puts that beside `SDPParser::generate` from
`aes67-core` for the same stream and reports every field the two disagree on,
saying which of them a receiver would act on.

It is a mirror of somebody else's function, so it drifts when the submodule
moves. `TestDaemonSdp` pins it to the SDP the daemon's own README prints,
which is what catches the drift.

It has already earned itself twice:

- `a=clock-domain:PTPv2 <domain>`, RAVENNA's attribute for the PTP domain, was
  written by the daemon and not by us. `SDPParser::generate` writes it now.
  With `ts-refclk` in its traceable form no domain is pinned anywhere else in
  the session, so a receiver had nothing to read.
- `a=ptime` differs in precision -- the daemon prints twelve decimals of a
  millisecond, we print three from an integer count of microseconds. Not a
  difference of stream: `a=ptime` is a recommendation (RFC 4566 §6) and
  `a=framecount` says the same packet exactly, in samples. The comparison
  reports it as wording when the framecounts agree, and as breaking when they
  do not.

There is no binary for this: it is a library and its suite.

### Reading a configuration back

```bash
./build/aes67-profile-conf --check /etc/daemon.conf --profile dante
```

`--check` reports what is wrong with a configuration instead of writing one.
With a profile it also reports what that profile forbids. Errors exit 1,
warnings alone exit 0.

The daemon itself validates almost none of this: `daemon/json.cpp` hands each
key to a setter, a value too big for the setter's type is truncated where
nobody sees it, and a key it does not know is ignored in silence. So each
check answers to something written down -- a field of the profile, a line of
upstream's source, or a standard:

| Check | On whose authority |
|-------|--------------------|
| Rate, packet time, PTP domain and multicast against the profile | `aes67-profiles` |
| A value too big for the type the daemon reads it as | `daemon/config.hpp` |
| A key the daemon never reads | `daemon/json.cpp` |
| `rtp_mcast_base`, `rtp_mcast_base_sec`, `sap_mcast_addr` inside 224.0.0.0/4 | RFC 5771 |
| A SAP address that is neither of SAP's own | RFC 2974 |
| `rtp_port` even, so RTCP has the odd port above it | RFC 3550 §11 |
| `ptp_domain` inside 0-127 | IEEE 1588-2008 |
| `ptp_dscp` inside 0-63 | the six bits of the DS field |
| `tic_frame_size_at_1fs` above zero and no more than `max_tic_frame_size` | the daemon's own pair |
| `http_port`, `rtsp_port` and the two NMOS ports all different | four servers, one host |
| A rate the project's own tests never drive | upstream `README.md` |
| `syslog_proto` other than `none` or `udp` falling through to TCP | `daemon/log.cpp:50,54` |
| `log_severity` above fatal | Boost.Log's levels |
| `http_base_dir`, `status_file`, `ptp_status_script` that are not there | the disk, relative to the current directory |

Anything merely unusual is a warning; only what the daemon, a profile or a
standard rules out is an error.

## Building

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The daemon wants Boost (thread, filesystem, log, program_options), Avahi and
systemd headers. The web interface is a release download or an `npm` build --
see the upstream `README.md` and `build.sh`.

## Verification

```bash
packages/aes67-linux-daemon/scripts/gate.sh
```

Everywhere it builds `aes67-profile-conf` and runs its suite. On Linux it also
configures, builds and runs the tests the daemon ships; anywhere else that half
is skipped and said to be skipped.

## What is not here

The PTP grandmaster: that is `packages/aes67-linux-ptpd`, which announces the
NIC's own hardware clock. The daemon is a slave and wants a master on the
segment; the two are the two ends of the same link, not alternatives.
