# Bring-up, with the board in your hand

The order matters. Every step assumes the one before it passed, and each one
says what a failure points at, because the expensive part of bring-up is not
finding a fault, it is looking for it in the wrong place.

Nothing in this file has been done. It is written from the code and the
hardware specification, not from a bench.

## What you need

- Oscilloscope. Not optional: three of the steps below cannot be done any other
  way, and one of them is the accuracy measurement this project has never made.
- Multimeter.
- The Rosendahl Nanosyncs, or a signal generator that can put out a square wave
  from 40 to 200 kHz.
- A laptop on the same switch, with Wireshark. `linuxptp` if you have it.
- A time interval counter, for step 9 only. Everything before it is doable
  without one.

## 0. Before any power

With the board bare, before anything is soldered to it.

- **The VUSB to VIN cut.** Meter across the two pads: open circuit. If it reads
  short, the cut is not done, and plugging in both the adapter and a laptop
  puts two supplies in parallel. `HARDWARE.md`, section "Power".
- **No short from 3V3 to 0 V**, and none from VIN to 0 V.

Fails: stop. Nothing below is worth trying.

## 1. The board alone

Flash the firmware with no Ethernet kit, no conditioning board, nothing on the
panel. USB only.

Expected on the serial console at 2000000 baud:

- The network lines, saying which configuration was taken.
- `[Clock] divider started for 48000 Hz`.
- `[Clock] WARNING: no word clock arriving on pin 14`. **That warning is the
  correct result here**: there is no word clock yet.

Fails: nothing on serial at all means the wrong baud rate or a board that did
not take the firmware. The console waits up to `AES67_SERIAL_WAIT_MS` for the
port, so the first lines are not lost; if you see later lines but not the first
ones, that timeout is what to look at.

## 2. Network

Ethernet kit soldered, patch lead to a switch.

- `[Ethernet] Link 100Mbps ON` on the console.
- An address, over DHCP or the static fallback, and the box answers `ping`.
- `http://<address>/` serves the page, with the PTP profile and the word clock
  rate.
- Pick another profile. The console reports the change, the page comes back
  with it selected, and after a power cycle it is still selected. That is the
  EEPROM working.
- Wireshark on the laptop: Announce and Sync messages leaving the box.
  `clockClass` 248 and `timeSource` INTERNAL_OSCILLATOR, because there is no
  reference yet. **That is the correct result at this step.**

Fails: no link, look at the kit's solder joints first. Link but no address and
no static fallback either, read the console: it says which path it took and why.

## 3. The conditioning board, on the bench

Before it goes anywhere near the box. Feed it from the generator, 3 Vpp at
48 kHz into the 75 ohm termination.

- At node A, a square wave centred on 1.65 V, swinging about 1.5 V either way.
- At U1's output, a full 0 to 3.3 V square wave, same frequency, edges in the
  nanoseconds.
- Drop the input to 1 Vpp: it should still switch cleanly. This is the
  hysteresis doing its job.
- Take the input away: the output should sit still, at one rail. If it
  oscillates, the hysteresis is not working, and Rf is the first thing to look
  at.

Fails at node A: the divider, R2/R3, or C1. Fails only at the output: U1, its
decoupling, or the reference divider R4/R5.

## 4. The divider

Conditioning board feeding pin 14. No bridge yet.

- Console at start-up: `[Clock] word clock measured: 48000 Hz`, or near enough.
  The measurement has a 2% tolerance and 5 Hz of resolution.
- Scope on pin 19: **one pulse per second**, about 4 ms wide.

Fails, no pulse on 19: the QuadTimer configuration, or the conditioning board
is not actually reaching pin 14. Check pin 14 with the scope first.

Fails, pulses at the wrong rate: the rate the box is configured for does not
match what the generator puts out. The console will have said so.

## 5. The bridge

Fit R7 and the wire, pin 19 to pin 15.

- The same pulse on pin 15.
- The box starts locking: within seconds the LED on pin 13 lights, and stays
  lit.

Fails: **a missing or bad bridge is silent**, which is the trap this step
exists for. The box measures its word clock, says the divider started, and
never locks. There is no message for it. Scope pin 15 and you will know in
seconds.

## 6. What the network sees

Wireshark again, with the box locked.

- Announce with `clockClass` 13, `timeSource` 0x90 OTHER.
- `currentUtcOffset` 37 with the valid flag **false**. That is deliberate: this
  box has no traceable absolute time. `HANDOFF.md` says why.
- Sync at the profile's rate, 8 per second by default, and the announced
  `logMessageInterval` matching the real rate.

With `linuxptp` on the laptop, `ptp4l -s` should follow it and report an offset
that settles.

## 7. Losing the reference

With the box locked, pull the BNC out.

- Within about five seconds, the LED goes out.
- Announce goes back to `clockClass` 248 and INTERNAL_OSCILLATOR.
- Put it back: it locks again.

This is the one behaviour that matters most to a network with more than one
grandmaster on it, and it is five seconds because `kNoPPSSeconds` is 5.

## 8. Changing rate

Set the generator to 96 kHz and pick 96000 on the web page, in that order.

- The console reports the divider restarted for the new rate.
- Pin 19 keeps putting out one pulse per second. That is the whole point: the
  PPS rate does not depend on the word clock rate, once they agree.
- Now get it wrong on purpose: leave the generator at 96 kHz and select 48000.
  The PPS should come out at 2 Hz, and PTP with it. The box does not correct
  this and does not pretend to; at the next restart the console warns.

## 9. Accuracy

The measurement this project has never made, and the reason for the PPS test
point.

Time interval counter between TP1 and a reference PPS. Read the fixed bias and
the spread. The fixed part is what `kCompareChannelDelayNs` is for, and it is
the number that would justify putting a real figure in `clockAccuracy` instead
of "unknown".

Until this is done, nothing should be claimed about the box's precision.

## 10. Soak

Leave it locked, running, on the network, for a day. Watch for the LED going
out, for lock counts resetting, for the console saying anything at all.

A grandmaster that works for ten minutes has not been tested.
