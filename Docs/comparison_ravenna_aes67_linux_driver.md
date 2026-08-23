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

### 1. SAP discovery

`NetworkEngine/Discovery/SAPListener.{h,cpp}` is built (it's in
`CMakeLists.txt`) and referenced by nothing outside its own files and a
comment in `Tools/AES67TestSender.cpp`. Streams can only be added by hand
or by importing an SDP file.

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

### Configurable transmit packet time

Our receiver honours `sdp_.ptime`; our transmitter hardcodes 1 ms
(`RTPTransmitter.cpp`: `const uint64_t intervalUs = 1000;`). The RAVENNA
profile's caveats already state this. Theirs is configurable per source
as `max_samples_per_packet` ∈ {12, 16, 48, 96, 192} — i.e. 0.25 / 0.33 /
1 / 2 / 4 ms at 48 kHz — with a driver-side tick
(`frame_size_at_1fs`, 32–192 samples).

This is the single change that would most widen what gear we interoperate
with: ST 2110-30 Levels B and C need 125 µs packets, which we currently
can't emit at all.

### Playout delay as a user-facing setting

Per-sink `playout_delay` in samples (minimum: the source's
`max_samples_per_packet`), plus a global default. We have a jitter buffer
(`LockFreeCircularJitterBuffer`) but expose no equivalent knob.

Converging evidence this is expected by installers rather than a nicety:
Dolby's DMA manual has the same control under a different name — "Safety
Buffer", 0–50 samples, documented for exactly the same symptom (brief
audio dropouts from network trouble).

### Reference-clock checking

- Source: `refclk_ptp_traceable` — whether the PTP reference clock is traceable
- Sink: `ignore_refclk_gmid` — whether the grandmaster ID must match

We parse SDP but do nothing with reference-clock identity. This is a real
interop safety check: two streams locked to *different* grandmasters will
drift, and comparing gmid catches it before the audio does.

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
2. **Turn on `SAPListener`**, with their expiry rule (drop a source after
   10 missed announcements). Again — the code exists.
3. ~~**Call `setQoSTrafficClass()`** with each profile's documented DSCP~~
   — **done**, see above.
4. **Make transmit ptime configurable**, unlocking ST 2110-30 Levels B/C.
5. **Expose playout delay**, matching both this daemon and Dolby's own
   Safety Buffer.
6. **Compare grandmaster IDs** between sources and sinks.

The first three change nothing about the driver's design — they connect
things already built. That's the headline: this driver's gap to the
reference implementation is smaller than it looks, and most of it is
wiring, not missing subsystems.

PTP is deliberately last despite being the highest-impact: it's the only
one of these that changes clocking behaviour on the one path verified
against real hardware, and it can't be verified here. The others are
checkable with a build and the test suite.
