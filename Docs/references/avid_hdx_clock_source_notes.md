# Following a Pro Tools HDX clock

Notes behind the Avid HD entry in the PTP clock source list.

## The short version

We don't talk to HDX. Avid's own AudioServer publishes HDX / HD Native to
CoreAudio as an ordinary audio device with its own hardware clock domain,
and this driver's existing "lock to a local audio device" clock source
already reaches anything in that list. Recognising *which* device it is —
so it can be named and recommended instead of sitting there as one more
opaque entry — is the entire feature.

From Avid's own knowledge base: "The AvidAudioServer allows the HDX or HD
Native hardware to be selected as the main output in System Preferences >
Sound so that any Mac application that uses Core Audio can route audio
through it."

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
- The entry is labelled "… — Pro Tools hardware clock" and sorted directly
  after *Internal*, ahead of the other hardware devices.
- A note appears under the picker **only when such a device is present**,
  explaining that in an HDX room this is usually the house clock
  everything else already follows — so locking to it keeps this driver in
  step with the studio rather than competing with it.

## What this does not give you

- No sample-accurate lock to HDX. We follow the CoreAudio device's clock
  domain, which is what any CoreAudio client can do; it is not a DigiLink
  clock feed.
- Nothing appears if the Avid HD Driver isn't installed, since without it
  there is no CoreAudio device to follow. That's the intended behaviour —
  "an extra option when it's found installed" — not a failure.
- Untested against real HDX hardware, like everything else in this driver
  that needs hardware to verify.
