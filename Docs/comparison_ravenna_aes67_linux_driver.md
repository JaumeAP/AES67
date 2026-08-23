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

Still missing versus theirs: we listen but never *announce* our own
sources, there's no mDNS/RAVENNA discovery, and no `auto_sinks_update`
equivalent.

The daemon announces its own sources over SAP *and* browses for remote
ones, on `sap_mcast_addr` (default 239.255.255.255) every `sap_interval`
seconds (default 30, or 0 for the RFC-compliant automatic interval), and
**removes a remote source that hasn't been announced for
`announce_period × 10` seconds**. That expiry rule is the part worth
copying — it's what makes a discovery list trustworthy rather than an
ever-growing pile of stale entries.

It also does mDNS (Avahi) for RAVENNA-style discovery, which our RAVENNA
profile's caveats already admit we don't implement.

### 2. PTP

`StreamManager::ptpManager_` is declared and never assigned; the
`PTPArbitrator`/`PTPMaster`/`PTPSlave` work in this repo isn't started
from the real driver path. The daemon makes PTP mandatory: "audio
operations require the PTP slave to achieve locked status."

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
stream, and `StreamChannelMapper::kMaxChannelsPerFlow` caps a flow at 8,
so offering it would be a claim this driver can't honour. AX/BX/CX (the
96 kHz variants, channel counts halved) are supportable in principle but
weren't added speculatively.

### Playout delay as a user-facing setting

Per-sink `playout_delay` in samples (minimum: the source's
`max_samples_per_packet`), plus a global default. We have a jitter buffer
(`LockFreeCircularJitterBuffer`) but expose no equivalent knob.

Converging evidence this is expected by installers rather than a nicety:
Dolby's DMA manual has the same control under a different name — "Safety
Buffer", 0–50 samples, documented for exactly the same symptom (brief
audio dropouts from network trouble).

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

Still missing: `refclk_ptp_traceable`, which is about whether the clock
chain reaches a traceable source at all rather than whether two streams
agree.

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
DESCRIBE/ANNOUNCE. We read SDP from local files only. For a cinema
install where the processor publishes its own SDP, fetching by URL is the
difference between "type this in" and "point at it".

### ST-2022-7 redundancy and NMOS

Dual-interface seamless protection switching (with automatic master clock
election across the two), and optional NMOS IS-04/IS-05 registration.
Both are out of scope for now but worth knowing the reference
implementation treats 2022-7 as a driver-level concern, not an add-on.

### Automatic sink updates

`auto_sinks_update` — configured sinks follow discovered sources when
they change. Only meaningful once discovery exists at all, but it's the
natural second half of turning `SAPListener` on.

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

1. **Start the PTP subsystem** in the real driver path, and gate audio on
   lock the way they do. The code exists; nothing calls it.
2. ~~**Turn on `SAPListener`**, with their expiry rule~~ — **done**, see
   above.
3. ~~**Call `setQoSTrafficClass()`** with each profile's documented DSCP~~
   — **done**, see above.
4. ~~**Make transmit ptime configurable**~~ — **done**, see above. A
   Level B/C profile could now follow.
5. **Expose playout delay**, matching both this daemon and Dolby's own
   Safety Buffer. Worth being precise about the cost: this is *not* a
   wire-up job like the three above. `LockFreeCircularJitterBuffer` is
   packet-slot capacity with no notion of holding audio back, and
   `jitterBufferDepth` — which is persisted and honoured on restore, but
   left at its default for newly added streams — is capacity, not
   latency. A real playout delay means changing the RT read path, which
   puts it in the same "can't verify without hardware" bucket as PTP
   rather than alongside DSCP and SAP.
6. ~~**Compare grandmaster IDs** between sources and sinks~~ — **done**,
   see above.

The first three change nothing about the driver's design — they connect
things already built. That's the headline: this driver's gap to the
reference implementation is smaller than it looks, and most of it is
wiring, not missing subsystems.

PTP is deliberately last despite being the highest-impact: it's the only
one of these that changes clocking behaviour on the one path verified
against real hardware, and it can't be verified here. The others are
checkable with a build and the test suite.
