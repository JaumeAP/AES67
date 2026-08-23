# Taking the clock from DigiLink

Wanted: the AES67 network — and this driver with it — running on the HDX
rig's clock, the one arriving over DigiLink.

That is achievable. It just can't be done in software on the Mac, and this
document is about why, and about the three arrangements that do work.

## Why not in software

Three separate walls, any one of which is enough:

1. **DigiLink is a hardware link on the card.** The clock rides differential
   pairs into the HDX PCIe card. There is no software interface on the Mac
   that exposes it — not CoreAudio, not IOKit.
2. **Pro Tools owns the card.** While Pro Tools runs it takes the hardware
   exclusively and doesn't go through CoreAudio at all, so the one software
   window that exists (the AvidAudioServer CoreAudio device) is shut
   precisely when the session is running. See
   `references/avid_hdx_clock_source_notes.md`.
3. **The protocol is closed.** No public specification, and the handshake
   is gated on an ID chip in genuine Avid interfaces.

The only genuine *software* route is to stop being a CoreAudio driver and
become something that runs inside Pro Tools — an AAX plugin, or a DirectIO
driver — under Avid's developer program. That is a different product, not
a feature of this one.

So: hardware tap. Which is what studios already do, and none of the
following is exotic.

## Path A — MTRX / MTRX II / MTRX Studio (one box)

The cleanest, and probably what a room with both worlds already has.

    HDX card ──DigiLink──▶ MTRX ──AES67/Dante──▶ network ──▶ this driver

MTRX takes DigiLink from the HDX card *and* carries Dante (AES67-capable)
with word clock support, so a single box is clocked by the HDX system and
is the clock leader for the audio network. Every AES67 device downstream —
this driver included — then follows the HDX clock, which is exactly the
goal.

MTRX Studio's own documentation describes both directions: it can be the
preferred master of the Dante clock with the network synchronised to it,
or a slave to the network's clock. The first is the one wanted here.

**Watch the PTP version.** Dante is PTPv1, AES67 is PTPv2. An
AES67-enabled Dante device bridges the two — receiving PTPv2 from the
clock master and generating PTPv1 for the Dante side, or the reverse. Get
the direction right or you will have two masters quietly disagreeing. Our
Dante profile documents the same split.

## Path B — word clock out of any HD interface

If there's no MTRX, every HD I/O and HD OMNI has a word clock output.

    HDX ──DigiLink──▶ HD I/O ──word clock──▶ PTP grandmaster ──▶ network

The grandmaster has to accept an external word clock reference — broadcast
GMs generally do, and so do the dedicated Dante/AES67 leader clocks. The
network then follows a PTPv2 clock that is itself derived from the HDX
rig.

## Path C — SYNC X / SYNC HD as the house clock

In a post room this is the usual shape, and the tidiest of the three.

    SYNC X ──loop sync──▶ HDX interfaces
       └────word clock──▶ PTP grandmaster ──▶ network

Sync X supports word clock, Loop Sync, AES3 and video reference, so it can
be the single origin both worlds hang off rather than one chasing the
other. Note Loop Sync always runs at 1x (44.1/48 kHz) — fine as a
reference, it is not a sample-rate carrier.

## Old rooms: no grandmaster, no network clock gear

All three paths above assume a PTP grandmaster already exists. Plenty of
HDX rooms have nothing of the sort — word clock and Loop Sync between the
Avid boxes, and that's the whole clock system.

If the AES67 audio and the HDX audio **sum or chain anywhere**, the two
sides must share a clock. This is not a quality argument, it is an
arithmetic one:

> Two free-running crystals 10 ppm apart drift 0.48 samples per second at
> 48 kHz — one sample every two seconds, a whole 1 ms packet's worth
> inside two minutes. Audio-grade crystals are typically ±10–25 ppm, so
> that is the optimistic case. It surfaces as periodic clicks or dropouts
> that get blamed on the network for weeks.

The receive path here has adaptive rate matching (`RTPReceiver`'s
P-controller) which absorbs some of this, but it is a shock absorber, not
a clock. It cannot make two clock domains one.

### The minimal fix: one box

    HD interface ──word clock──▶ PTP grandmaster ──PTPv2──▶ AES67 network
                                        │                      │
                                        └── this driver ───────┘

A grandmaster that accepts a **word clock input** — the Dante/AES67 leader
clocks sold for exactly this, or a broadcast GM with external reference.
One BNC out of any HD I/O or HD OMNI, and the AES67 side now runs on the
HDX clock. Everything downstream, this driver included, is a plain PTP
slave.

Take the clock **from** the HDX rig rather than feeding word clock into
it, when there's a choice: HDX is the harder side to discipline, and this
way it keeps doing what it already does.

### Why "let this driver be the master" isn't the answer here

This driver can be PTP grandmaster, and in a room with Dolby amplifiers it
may have to be — the DMA and DAC3202 are PTP slaves with clock priority
fixed at 255 and will not originate timing for anyone.

But its clock would then be the Mac's own crystal, with no relationship to
the HDX rig. The AES67 side would be internally coherent and still drifting
against Pro Tools, which is the same failure with more steps. The Mac has
no word clock output to feed the HDX rig from, so there is no way to close
the loop in software.

Hence: one box. It is the smallest change that makes the room correct, and
there isn't a cheaper one that actually works.

## What this driver needs in each case

Nothing new. In all three the network gets a PTPv2 grandmaster that is
derived from the HDX clock, and this driver's job is simply to be a PTP
slave following it — which is what it does by default.

Concretely:

- Leave the clock source on **Internal**; it is not the reference and
  shouldn't claim to be.
- Leave **"Act as PTP master"** off, so BMCA doesn't hand grandmaster to
  this Mac over the real one.
- If the profile in use forces PTP slave (CP850, and CP950/CP950A), that
  is already the correct posture and nothing needs changing.
- Enable **"Run a PTP clock"** so the driver actually disciplines to the
  network grandmaster instead of free-running.

The thing to avoid is the inverse: selecting the Avid CoreAudio device as
this driver's clock source. It looks like it does what's wanted, and it
disappears the moment Pro Tools opens.

## Summary

| Route | Tap | Works while Pro Tools runs |
|---|---|---|
| Avid CoreAudio device as clock source | software | **No** — device vanishes |
| MTRX / MTRX Studio (Path A) | hardware | Yes |
| HD interface word clock out (Path B) | hardware | Yes |
| SYNC X / SYNC HD (Path C) | hardware | Yes |
| AAX plugin / DirectIO driver | software | Yes, but a different product |
| Word-clock-input grandmaster (old rooms) | hardware, one box | Yes |
| No shared clock at all | — | Only if the audio never sums |

Untested against real HDX hardware here — as with everything in this
driver that needs hardware to verify.
