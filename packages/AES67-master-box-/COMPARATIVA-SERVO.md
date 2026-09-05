# The two clock servos, compared

This project and `JaumeAP/DTS-Player` both discipline a hardware PTP
clock, with independent designs. This document is what comes out of
reading them side by side.

Comparison made on 2026-08-25, by reading code. **Neither has been
measured running**, so everything said here about behaviour is what the
code says it does, not what it has been seen doing.

## The two designs, in one sentence

Here: t41-ptp's `PTPBase::updateController()`. It computes the drift
from the t1/t2 intervals and picks between three modes: pure frequency
correction if the drift goes past 1000 ns/s, a clock step if the offset
goes past 1000 ns, and a PI loop (KP=1.0, KI=0.5) with the accumulated
drift term once it is close. It rejects any measurement implying more
than 100 ppm of drift.

There: `PtpClockDiscipline` decides and `PtpHardwareClock` executes. Two
complementary corrections on every reference tick: phase, correcting
only a quarter of the measured drift, and frequency, with a scale
factor.

## What we should learn from them

### 1. Separate the servo from the hardware

It is the difference that weighs most, and it explains why this project
does not have a single test. `PtpClockDiscipline` depends on neither
Arduino nor `esp_eth`: it is pure arithmetic over clock readings.
`PtpHardwareClock` only makes the calls. That is why they can test the
servo on the host, with no board, and have `test_ptp_clock_discipline`
working.

Our `updateController()` calls `EthernetIEEE1588` from inside the
decision logic. There is no way to test it without a Teensy connected.

**If this project continues, this is the first thing to do, before any
new feature.** Until then nothing in the servo is verifiable.

### 2. Do not correct the whole offset at once

When ours enters coarse mode it calls `offsetTimer()` with the entire
correction. It is a discontinuous step, and a slave downstream sees it
as a shove of the clock.

They correct a quarter of it per tick and converge geometrically over a
handful of ticks, with no single step going past a quarter of the
measured drift. The comment in their code says exactly why.

### 3. BMCA

They have an election (`lib/PtpBmca`) and listen to other masters'
announcements (`lib/PtpExternalReference`). Ours proclaims itself master
always and listens to nobody: on a network with another grandmaster,
both shout.

## What we have and they do not

### 1. Hardware packet timestamping, working

t41-ptp's `syncMessage()` marks the frame with `timestampNextFrame()`,
waits for the hardware transmit stamp with `readAndClearTxTimestamp()`
and sends the Follow_Up carrying that value. It is a complete two-step
implementation.

The P4 does not have it yet (see its
`docs/l2tap-hardware-timestamping.md`). That is what is worth taking
there if that ends up being the path: not our code, but the shape of
this sequence.

### 2. The impossible-drift check

We reject any measurement implying more than 100 ppm, taking the master
to be invalid rather than chasing it. It is cheap and it stops a
reference doing something odd from dragging the clock along.

On a box whose reference is a film's playback head, that is worth even
more than it is here.

## What I do not like about ours

So as not to sell it better than it is.

`nspsAccu` is a 32-bit `int` accumulating nanoseconds of offset with no
cap at all. It is reset on a mode change, which mitigates it, but inside
fine mode there is no limit: a small, persistent bias makes the integral
term grow unchecked, and at the extreme that is signed integer overflow,
which is undefined behaviour.

And `driftNSPS = fmod(driftNSPS + currentDriftNsps, NS_PER_S)` looks
more like a patch than a design: it is not obvious what accumulating a
drift modulo a thousand million is supposed to mean.

Neither of those two is ours, they come from t41-ptp upstream.

## A coincidence

Both servos work with sync at eight per second, each arrived at
independently. There `SYNC_INTERVAL_MS = 125`; here it was set in this
session by reasoning from the AES67 specification.

## Not verified

Nothing measured. Neither clock has been seen running: this project has
no hardware, and its `CLAUDE.md` says nothing has ever run on an
ESP32-P4. The claims about which converges better come from reading the
code and its comments.
