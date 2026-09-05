# PTPNode

One sketch, flashed to every board on the segment. Each node announces itself and
follows a better clock instead of going quiet when one turns up, so which of the
two roles it is playing at any moment is the best master clock algorithm's answer
rather than a compile-time choice. Two boards running this file unchanged, one of
them wired to a reference, settle on the one with the reference.

## What it needs

- A Teensy 4.1 with the Ethernet kit, on a segment that passes multicast.
- Optionally a reference on pin 15. Everything works without one; the node then
  says on the wire that it has none.

## Pins

| Pin | Direction | What it is |
|---|---|---|
| 15 | in | Reference input, `ENET_1588_EVENT2_IN` (IOMUX ALT4), rising-edge capture |
| 24 | out | PPS output, `ENET_1588_EVENT1_OUT` (IOMUX ALT6), 25 ns pulse |
| 13 | out | The on-board LED, used as a state light |

Serial runs at 2000000 baud.

The reference input expects a word clock at the audio sampling rate, not a
one-per-second pulse: `interrupt_1588_timer()` counts `WORD_CLOCK_DIVISOR`
(48000) rising edges and calls the 48000th the reference edge. Feeding it a 1 PPS
signal directly means setting that divisor to 1.

## Network

`staticIP` is the one line to change per board — the sketch ships with
192.168.0.211. Transport is `l3PTP`, UDP multicast on ports 319 and 320; the
`l2PTP` line right below it swaps in raw 802.3 frames instead, and nothing else
in the sketch changes.

## What it announces

`priority1` is left at the standard's 128 on every node. That is deliberate: it
is the first field the comparison looks at, so leaving it tied is what lets
`clockClass` — the field that actually knows whether the reference is there —
decide. `setPriority1()` is for preferring one node over another for reasons the
datasets cannot carry, such as which one sits on the better switch port, and this
sketch has no such reason to presume.

`clockClass` moves with the input, through the three values of Table 5 the sketch
can honestly claim:

| State of the reference | `clockClass` | Table 5 |
|---|---|---|
| Edges arriving on pin 15 | 13 | Synchronized to an application-specific source of time |
| Quiet for six seconds, up to `HOLDOVER_MS` (60 s) | 14 | The same clock in holdover |
| Quiet for longer | 248 | Free-running, no history worth mentioning |

The holdover step is worth the trouble because `adjustFreq()` is a standing
correction: a clock that has just lost its reference is still running at the
frequency that reference taught it and drifts from there with temperature, which
makes it better than one that was never disciplined at all. A minute later it is
not, and the announcement stops saying so.

`timeSource` stays at `INTERNAL_OSCILLATOR`.

## The state line

Printed once a second:

    [ptp] state=slave pps=none sync=ok master=chosen offset=-198ns delay=10342ns lock=7 tx_fail=0 bind_fail=0

| Field | Meaning |
|---|---|
| `state` | Port state: `init`, `listening`, `master`, `passive`, `slave` |
| `pps` | `yes` while reference edges are arriving on pin 15, `none` once they stop |
| `sync` | `ok` while a master's Sync is still being heard, `lost` after the receipt timeout |
| `master` | `chosen` once the algorithm has picked one to follow |
| `offset` | Last offset the servo corrected for |
| `delay` | Path delay, after the minimum-of-*N* filter |
| `lock` | Consecutive corrections inside ±100 ns |
| `tx_fail` | Sends that failed since boot — a grandmaster whose Sync never leaves is otherwise silent about it |
| `bind_fail` | Sockets or multicast group joins that could not be opened |

The LED on pin 13 means the port is doing its job, whichever job that is: as
slave, locked to a master still being heard; as master, on air — either
disciplined by the reference or, with no reference at all, free-running on its
own crystal, which is what a master with nothing to follow is. The lock count
means nothing in that last case, so the light does not wait for it.

## Without a reference

Nothing to change. After six seconds of silence on the input the node starts
pacing its own Sync, and the `clockClass` ladder above already says what it is.
Two nodes that both end up at 248 announce identical datasets and the comparison
falls through to clock identity, which is arbitrary and costs nothing, since
neither is better than the other.

## A reference that carries an epoch

The sketch declares the reference edge to be the top of a second (`pps_ns=0`). A
GPS pulse really is one; a word clock is not, because it carries a rate and not
an epoch, so the clock locks to the right frequency with a fixed and arbitrary
offset in the seconds. Feed it a GPS 1 PPS instead and that offset goes away —
and 6, not 13, becomes the honest `clockClass` to announce for it.

## Rates

Sync goes out eight times a second (`SYNC_INTERVAL_US`, 125000 µs) and Announce
once, which is what an AES67 endpoint expects of a master. `setLogSyncInterval(-3)`
puts the same interval in the messages themselves — the field is a log2 of the
interval in seconds — and the two have to be changed together: a slave that reads
one rate and is fed another times its receipt timeouts against something that is
not happening.

One tick of that timer sends one Sync, provided the node has business announcing
time at all: either it is free-running, or the servo is locked to the reference it
has. The reference edge itself no longer paces the Sync, it only disciplines the
clock — while it did, the rate was whatever the input ran at, one a second here,
however fast the timer ticked.

`NO_REFERENCE_TICKS` (48) is where the count of ticks without an edge becomes
free-running: six seconds, the same six as when the timer ran at one second and
the number was 6. It counts ticks, so it moves whenever `SYNC_INTERVAL_US` does.
