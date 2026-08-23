# Following a Pro Tools HDX clock

Notes behind the Avid HD entry in the PTP clock source list.

## The short version, corrected

**Do not plan on following an HDX clock from software.** Avid's
AudioServer does publish HDX / HD Native to CoreAudio as an ordinary
device with its own hardware clock domain — but only while Pro Tools is
closed. Pro Tools does not use CoreAudio for HDX; it takes the hardware
exclusively, and the CoreAudio device **disappears from the system
entirely** the moment Pro Tools launches, returning when it quits.

So the option exists and works, and is useless in exactly the situation it
looks made for: a Pro Tools room with Pro Tools running.

The first version of these notes recommended locking to it. That was
wrong, and the recommendation has been replaced with a warning.

### What to do instead

Full treatment in `../taking_clock_from_digilink.md` — three hardware
paths that genuinely put the network on the HDX clock. In brief:

Clock both worlds from one source rather than chaining one to the other:

- A dedicated **PTPv2 grandmaster** on the AES67 network — the recommended
  arrangement for AES67 infrastructure generally, not just here.
- **Word clock** from that same source into the HDX rig (via SYNC HD, or
  an interface that accepts it).
- Where a device bridges both worlds (an Avid MTRX / MTRX Studio, say, or
  a Dante leader clock with word clock out), let it be the one point where
  the two clock domains meet — it can be master on one side and slave on
  the other by design.

Chasing HDX from software inverts this: it makes the DAW's card the
reference for a network that has a proper grandmaster available, and then
loses that reference the moment the DAW opens.

### Where the CoreAudio device is still useful

Only with Pro Tools closed — playing out of other applications through the
HDX interfaces, for instance. Legitimate, just not a studio clock plan.

Avid's own knowledge base on the mechanism: "The AvidAudioServer allows
the HDX or HD Native hardware to be selected as the main output in System
Preferences > Sound so that any Mac application that uses Core Audio can
route audio through it." And on the exclusivity: "Avid hardware doesn't
use ASIO or CoreAudio drivers when it is running under Pro Tools, hence
the reason why it disappears from the system when Pro Tools is open."

## Why not DigiLink directly

Investigated and rejected, so nobody spends a week rediscovering it:

- **DigiLink is proprietary.** It's Avid's own serial transport carrying
  audio, control and clock over differential pairs — conceptually near
  AES50 or MADI, with different framing and its own handshake. There is no
  public specification.
- **The handshake is gated on hardware.** Genuine Avid interfaces carry an
  ID chip the card checks. Third parties (Lynx, Burl among them) have done
  reverse-engineering work to attach to HDX/HD Native, and it stays awkward
  without Avid's licensing precisely because of that chip.
- **No open implementation exists to build on.** Searched; the substantive
  public discussion is forum-level (GroupDIY, the Avid DUC), not code.
- **The frameworks on this machine can't help.** The Avid AudioServer
  bundle (`~/projects/Bundles/AUDIOSERVER_CONTENTS`) ships DirectIO,
  DFW, DSI and friends as PACE-wrapped binaries with no headers. Not
  something to pull apart.

None of which matters, because the CoreAudio route above gets the clock
without any of it.

## What the code does

`DriverManager.listAvailableClockSources()` already enumerated every
CoreAudio device with a nonzero clock domain (the CoreAudio convention for
"this has its own hardware clock rather than riding on someone else's").
Added on top:

- Detection by **manufacturer** (`kAudioObjectPropertyManufacturer`
  containing "Avid" or "Digidesign"), not device name. A user can rename a
  device in Audio MIDI Setup, and another vendor shipping "HDX" in a
  product name shouldn't be enough to claim it's Avid hardware.
- The entry is labelled "… — Pro Tools hardware (gone while Pro Tools
  runs)", so the caveat travels with the option itself rather than living
  only in a note someone might not read. Sorted plainly alphabetically;
  an earlier version promoted it to the top as recommended, which was
  exactly backwards.
- A warning appears under the picker **only when such a device is
  present**, stating the exclusivity and pointing at the one-house-clock
  arrangement instead.
- A second warning appears when the selected clock device is missing
  (`selectedClockSourceMissing`), because the clock quality drops at that
  moment and the cause is otherwise invisible.

## What this does not give you

- **Nothing at all while Pro Tools is running.** The headline caveat, and
  the reason this isn't a clock plan for a working HDX room.
- No sample-accurate lock to HDX even when it is present. We follow the
  CoreAudio device's clock domain, which is what any CoreAudio client can
  do; it is not a DigiLink clock feed.
- Nothing appears if the Avid HD Driver isn't installed, since without it
  there is no CoreAudio device to follow.
- Untested against real HDX hardware, like everything else in this driver
  that needs hardware to verify.

## How the driver behaves when the device vanishes

Already correct before this was understood, by luck rather than design:
`CoreAudioClockSource::isDeviceLocked()` re-checks presence and clock
domain on every query, so when the device goes it reports clockClass 248
and Unknown accuracy instead of 13 and Within1Microsecond. The driver
therefore advertises itself as a poor clock and BMCA lets a better one
win, rather than continuing to claim a reference it no longer has. The UI
warning was added so the user can see why.
