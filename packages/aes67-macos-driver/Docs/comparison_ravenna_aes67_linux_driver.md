# What we can learn from the RAVENNA/AES67 Linux driver

The closest thing to a reference implementation of what this project is:
an open-source virtual sound card that speaks AES67, on a platform with a
comparable audio stack. Worth comparing against precisely because it is
mature, in production use, and its design decisions are visible.

## The projects

| | This driver | RAVENNA/AES67 Linux |
|---|---|---|
| Audio stack | CoreAudio AudioServerPlugIn (userspace, libASPL) | ALSA kernel module (`MergingRavennaALSA.ko`) |
| Control plane | ManagerApp (SwiftUI), settings JSON files | `aes67-daemon` — REST API + web UI, netlink to the module |
| Origin | this project | Merging Technologies (GPL kernel part), forked/extended by [bondagit](https://github.com/bondagit/aes67-linux-daemon) |

Sources read: [aes67-linux-daemon](https://github.com/bondagit/aes67-linux-daemon)
README and `daemon/README.md` (full config reference),
[ravenna-alsa-lkm](https://github.com/bondagit/ravenna-alsa-lkm),
[Merging's product page](https://www.merging.com/products/alsa_ravenna_aes67_driver).
Retrieved 2026-08-23.

The split is the same shape as ours — a real-time component doing RTP and
clock, and a separate control process doing configuration and discovery —
which is why the comparison is worth anything at all.

## Things we already have code for but never run

The most useful finding, and the least flattering: three subsystems exist
in this repo, compile, and have **zero callers**. The Linux driver treats
all three as core functionality.

### 1. SAP discovery — **now done**

`NetworkEngine/Discovery/SAPListener.{h,cpp}` was built (it's in
`CMakeLists.txt`) and referenced by nothing outside its own files.
Streams could only be added by hand or by importing an SDP file. Wired up
since this comparison was written:

- `AES67Device` owns and starts the listener; failing to start is
  non-fatal (discovery is a convenience, carrying audio is not).
- Sessions reach ManagerApp through a second custom property on the
  existing gateway (`kDiscoveredSessionsPropertySelector`, 'a67s'),
  alongside the PTP diagnostics one.
- New "Discover" toolbar button and `DiscoveredSessionsView` — pick a
  session, add it as a stream, no retyping.

The listener itself gained the two things it lacked: sessions now carry a
`lastSeen` stamp and expire (`kSessionTimeout`, 300 s — their
ten-missed-announcements rule at the usual 30 s interval), and SAP
**deletion** packets are honoured instead of silently discarded, so an
announcer's explicit goodbye takes effect immediately rather than after
the timeout. Repeats refresh the entry rather than being treated as new
discoveries.

We now both listen *and* announce: `NetworkEngine/Discovery/SAPAnnouncer.{h,cpp}`
sends version-1 SAP announcements for every TX stream on both SAP groups
every 30 s (`kAnnounceInterval`), with a deletion packet on withdrawal
carrying the same Message ID Hash it announced with, and `IP_MULTICAST_LOOP`
off so we never discover our own sources. AES67Device wires it to
`StreamManager::getTransmitSessions()` + `SDPParser::generate`. Still missing
versus theirs: no mDNS/RAVENNA discovery (SAP is our only discovery
transport). `auto_sinks_update` — receive streams following a moved source —
is now done; see below.

**SAP address, found by inspecting Dante Controller.** The listener
originally joined only 224.2.127.254 (RFC 2974 SAPv2 global scope). Dante's
own `libDanteController` announces AES67 sessions on **239.255.255.255**
(the address AES67 uses, and the AES67 Linux daemon's own default too), so
a Dante device in AES67 mode was never discovered. The listener now joins
both groups. That was the RECEIVE side; the SEND side (SAP announce, above)
is now done too, so a Dante device in AES67 mode can auto-discover streams
FROM us. (Dante *Controller* itself still won't list us in its device view:
it browses for Dante `_netaudio-*` mDNS devices, and we are an AES67 device,
not a Dante one — the interop is at the stream/SAP/PTP layer, which is what
both halves cover.)

The daemon announces its own sources over SAP *and* browses for remote
ones, on `sap_mcast_addr` (default 239.255.255.255) every `sap_interval`
seconds (default 30, or 0 for the RFC-compliant automatic interval), and
**removes a remote source that hasn't been announced for
`announce_period × 10` seconds**. That expiry rule is the part worth
copying — it's what makes a discovery list trustworthy rather than an
ever-growing pile of stale entries.

It also does mDNS (Avahi) for RAVENNA-style discovery, which our RAVENNA
profile's caveats already admit we don't implement.

### 2. PTP — **now startable, off by default**

`StreamManager::ptpManager_` was declared and never assigned; nothing in
the driver path ever called `PTPClockManager::getClockForDomain()`, so the
whole subsystem — slave, master, BMCA, arbitrator — was compiled and never
run, and `getPTPDiagnostics()` could only ever return the disconnected
defaults.

Adding a stream now starts (or joins) a clock for that stream's PTP
domain, so several streams on one domain share a clock rather than each
starting its own, and diagnostics report something real.

Two switches, both **off by default**, in the PTP Diagnostics window:

- *Run a PTP clock* — the master switch. Off because every build before
  this carried audio with no PTP at all, and starting it opens multicast
  sockets and threads on the one path that has been verified against real
  hardware. This is the single change in this whole comparison that a
  build and a test run cannot check.
- *Refuse audio until the clock locks* — the daemon does this
  unconditionally ("audio operations require the PTP slave to achieve
  locked status"). Here it is opt-in and depends on the first switch,
  because on a system carrying audio today it can only ever take audio
  away.

Their exposed status is a good model for what ours should report, and is
close to what our diagnostics gateway already carries:

- `status`: `unlocked` / `locking` / `locked`
- `gmid`: grandmaster clock ID currently synced to
- `jitter`: measured PTP packet delay jitter

Plus `ptp_status_script`, a hook run whenever slave lock status changes —
a cheap idea for making lock loss visible to an installer.

### 3. DSCP marking — **now done**

`NetworkUtils::setQoSTrafficClass()` had no callers, so this driver marked
nothing whatever the profiles documented. Wired up since this comparison
was written: every transmitter now marks its outgoing packets with the
active profile's `recommendedDscp` (`StreamManager::createTransmitter()`
→ `RTPSocket::openTransmitter()` → `setQoSTrafficClass()`), best-effort —
a socket that refuses the codepoint logs and carries on unmarked rather
than failing the stream. Receivers stay unmarked: they send no audio to
prioritise.

Still missing versus theirs: a *per-source* override (ours is per
profile), and any marking of our own PTP traffic. The daemon makes DSCP a
first-class setting in two places:

- `ptp_dscp`, default **46**, valid 48 or 46
- per-source `dscp`, valid 46 / 34 / 26 / 0

Note their PTP default of 46 is exactly the value Dante does *not* use
(Dante marks PTP CS7/56) — the conflict our Dante profile now documents,
confirmed from a second direction.

## Things they have that we don't

### Configurable transmit packet time — **now done**

Our transmitter used to hardcode 1 ms while taking its sample count from
`sdp_.framecount`, so the two could disagree — a 96 kHz stream with the
default framecount of 48 transmitted at the wrong rate. Both are now
derived together from `framecount` (authoritative when present) or
`ptimeUs`, so they always agree.

Getting there uncovered a bug that made sub-millisecond packet times
impossible in the first place: `ptime` was held as **integer
milliseconds** and parsed with `stoul`, so the perfectly legal
`a=ptime:0.125` parsed to **zero**, silently. Packet time is now
microseconds end to end (`SDPSession::ptimeUs`,
`CompatibilityProfile::allowedPtimesUs`, `StreamInfo::ptime` — which had
been documented as microseconds while being assigned milliseconds).
`Tests/TestSDPParser.cpp` had carried `a=ptime:0.5` and `a=ptime:0.25`
fixtures since before any of this was noticed, but only ever asserted the
sample rate; it now asserts the packet times, 125 µs included, and that
fractional values survive a round trip through `generate()`.

Theirs is configurable per source as `max_samples_per_packet` ∈ {12, 16,
48, 96, 192} — 0.25 / 0.33 / 1 / 2 / 4 ms at 48 kHz — with a driver-side
tick (`frame_size_at_1fs`, 32–192 samples). Ours takes whatever the SDP
says, which covers the same ground from the other direction.

A **Level B** profile now exists to ask for it (48 kHz, 125 µs, ≤8
channels — Level A's constraints at the shorter packet time). It's a
separate profile rather than a widening of Level A, because the levels are
claims about what the *receiving* gear supports: a Level A device must not
be sent 125 µs packets just because this driver can emit them, so each
profile rejects the other's packet time.

Level C is deliberately absent — it allows 64 channels in a single
stream. The RTP path carries that width (the RAVENNA profile uses it, at
the 125 µs packet time that makes 64 channels fit a frame), so a Level C
profile is possible; it wasn't added speculatively. Neither were AX/BX/CX
(the 96 kHz variants, channel counts halved).

### Playout delay as a user-facing setting — **now done**

Per-sink `playout_delay` in samples (minimum: the source's
`max_samples_per_packet`), plus a global default.

Converging evidence this is expected by installers rather than a nicety:
Dolby's DMA manual has the same control under a different name — "Safety
Buffer", 0–50 samples, documented for exactly the same symptom (brief
audio dropouts from network trouble).

This looked like it would need an RT-path restructure, and the earlier
draft of this document said so. It didn't: `RTPReceiver` already had a
pre-fill phase that waits for the jitter buffer to reach
`kPrefillPacketCount` before paced consumption starts, and *that cushion
is the playout delay* — it was simply a hardcoded 6. It's now derived from
a configured delay in samples, expressed in samples (as both the daemon
and Dolby express it) and converted to packets where the pre-fill loop
counts them, never below one packet. Driver-wide rather than per sink.

### Reference-clock checking — **now done**

- Source: `refclk_ptp_traceable` — whether the PTP reference clock is traceable
- Sink: `ignore_refclk_gmid` — whether the grandmaster ID must match

We parsed SDP's `a=ts-refclk` and did nothing with the identity in it.
`StreamManager::canAddStream()` now compares a new stream's grandmaster
against the one every current stream is using and refuses a mismatch,
naming both. Comparison is separator- and case-insensitive, so
`00-1B-21-…` and `00:1b:21:…` are recognised as the same clock; a stream
that declares no grandmaster is never refused on this ground.
`setIgnoreRefClockMismatch()` is the escape hatch, driver-wide where
theirs is per sink.

`refclk_ptp_traceable` — the RFC 7273 traceable form,
`a=ts-refclk:ptp=IEEE1588-2008:traceable`, where the grandmaster is locked
to a traceable primary reference (e.g. GPS) and pins no gmid/domain — is
now handled too: `SDPParser` parses it into `SDPSession::ptpTraceable`
(clearing the gmid so a receiver won't lock to an identity allowed to
change) and regenerates it in preference to a named grandmaster.
Round-tripped in TestSDPParser.

### Wider format support

| | This driver | Daemon |
|---|---|---|
| Sample rates | 44.1/48/96 per profile (device declares to 384 k) | 44.1 → 384 kHz throughout |
| Codecs | L16, L24 | L16, L24, **AM824 (L32)** |
| Channels | 128 per direction | 2–64 (mono supported) |

Our channel count is the one axis where we're ahead. AM824 we reject
everywhere — correct for ST 2110-30 (that's -31), but it is legitimate
RAVENNA/AES67 territory.

### SDP over the network

They fetch a sink's SDP from an **HTTP or RTSP URL** (`use_sdp`,
`source`), and run an RTSP server exposing their own sources via
DESCRIBE/ANNOUNCE. **Now done on the reading side.** `SDPFetcher::fetch`
takes a local path, `file://`, `http://` or `rtsp://` (DESCRIBE, through
the RTSPClient that was already here and that nothing called), and
`StreamManager::importSDPURL` builds a receive stream from whatever comes
back. For a cinema install where the processor publishes its own SDP, that
is the difference between "type this in" and "point at it".

`https://` is refused with a message that says why: this layer speaks BSD
sockets and has no TLS, and a scheme that fails late is worse than one
that fails at the point of typing. Everything it reads comes from an
unauthenticated server into coreaudiod, so the body is bounded at 1 MiB,
every socket carries a timeout, and no parse throws — the cases are pinned
in TestSDPFetcher.

Announcing our own sources by URL is the half still missing: the RTSP
server here serves DESCRIBE (`AES67Device`), but there is no ANNOUNCE.

### Marking our own PTP

**Now done.** `PTPSlaveConfig::dscp` and `PTPMasterConfig::dscp` mark this
port's PTP, and `aes67ptpd --dscp <n>` exposes it. Unmarked stays the
default, which is what this driver has always sent. The Merging RAVENNA
driver installed on this machine carries the same knob
(`$.network.PTP.DSCP`), and the reason is the same: on a segment that
sorts by DSCP, PTP left unmarked queues behind the audio it is timing.
Which value belongs there is the network's business — EF is 46, Dante
marks PTP CS7 — so neither of us chooses one.

From the same comparison, the PTP dataset is exposed now
(`PTPMasterSettings` carries priority1/2, the clock class and accuracy,
the intervals, the delay mechanism and the DSCP; `applyPTPSettings` puts
them into the engines). Clock class and accuracy are stored but not
carried into the Announce on purpose: the master announces what its clock
source really is, and letting a settings file claim better is a lie BMCA
acts on.

NMOS IS-04 has its first half: `NMOSRegistrationClient` finds a registry
over `_nmos-register._tcp`, registers this driver as a Node and keeps the
registration alive, re-registering when the registry answers 404. `AES67Device` starts it when `NMOSSettings::enabled` says so (off by
default), registering with a discovered registry or with the one the
settings name, and unregistering on the way out. Devices, sources, flows,
senders and receivers — where the streams themselves would go — are still
not written.

### ST-2022-7 redundancy and NMOS

Dual-interface seamless protection switching (with automatic master clock
election across the two), and optional NMOS IS-04/IS-05 registration.
Both are out of scope for now but worth knowing the reference
implementation treats 2022-7 as a driver-level concern, not an add-on.

### Automatic sink updates

`auto_sinks_update` — configured sinks follow discovered sources when
they change. **Now done.** `StreamManager::updateReceiveStreamsFromAnnouncement`,
fed from the `SAPListener` announcement callback in AES67Device (parsing the
SDP off the audio path), re-points any receive stream onto a source that
re-announced with changed transport — multicast address, port, sample rate,
encoding, ptime, payload type — preserving the sink's device-channel mapping.
Streams are matched by session name plus, when both sides know it, the unicast
source address; a channel-count change is deliberately NOT followed (it would
force a device re-map that could collide with neighbouring streams) and is
logged instead. The pure decision is `StreamManager::evaluateSinkFollow`
(inline, unit-tested in TestStreamManager: move, no-op, not-bound,
channel-count-change). On by default; `setAutoSinkFollow(false)` disables it.

## Their documented operational gotchas

Worth recording because they're the same class of problem this driver
has, on a different OS:

- Virtual machines unsupported.
- PulseAudio must be disabled (no macOS equivalent, but the lesson —
  a second audio daemon fighting for the device — generalises).
- **CPU frequency scaling causes audio distortion**; must be disabled.
  The macOS analogue is our `THREAD_TIME_CONSTRAINT_POLICY` work in
  `AudioThreadPriority.cpp`.
- Kernel 5.10+ needs RT scheduler throttling adjusted.
- Minimum tested latency ≈ 6 ms, platform-dependent — a useful sanity
  check against any latency claim we might be tempted to make.

## Suggested order of work

Ranked by value-for-effort, given how much is already written here:

1. ~~**Start the PTP subsystem** in the real driver path, and gate audio
   on lock the way they do~~ — **done**, both behind opt-in switches; see
   above. Still unverified against real hardware, which is exactly why
   the defaults are off.
2. ~~**Turn on `SAPListener`**, with their expiry rule~~ — **done**, see
   above. And its natural second half, ~~**announce our own sources over
   SAP**~~ — **done**, `SAPAnnouncer`, see above.
3. ~~**Call `setQoSTrafficClass()`** with each profile's documented DSCP~~
   — **done**, see above.
4. ~~**Make transmit ptime configurable**~~ — **done**, see above. A
   Level B/C profile could now follow.
5. ~~**Expose playout delay**~~ — **done**, see above. The earlier
   pessimism about it was wrong: `jitterBufferDepth` is indeed capacity
   rather than latency, but the pre-fill cushion beside it was exactly
   the right knob and only needed to stop being a constant.
6. ~~**Compare grandmaster IDs** between sources and sinks~~ — **done**,
   see above.

The first three change nothing about the driver's design — they connect
things already built. That's the headline: this driver's gap to the
reference implementation is smaller than it looks, and most of it is
wiring, not missing subsystems.

Every item above is now done. The two that touch the path verified
against real hardware — PTP and playout delay — are the two a build and a
test run cannot check. PTP is therefore shipped off by default rather
than assumed good; playout delay defaults to the cushion this receiver
has always used, so leaving it alone changes nothing.
