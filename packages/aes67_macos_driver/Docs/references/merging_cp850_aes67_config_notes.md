# CP850 AES67 mode — third-party confirmation (Merging Technologies)

Not a Dolby manual — a third-party interop guide, kept because it settles a
question the Dolby-authored sources in this folder couldn't:
`dolby_cp850_installation_manual_issue2.pdf` (2014, DAC3201-era) has zero
AES67 content, so it was an open question whether the CP850's AES67 mode
even has a real, documented UI. It does.

Source: "Configure MERGING and Dolby ATMOS CINEMA PROCESSOR CP850 device in
AES67 mode", Merging Technologies public Confluence page, retrieved
2026-08-23:
<https://merging.atlassian.net/wiki/spaces/PUBLICDOC/pages/4818738/Configure+MERGING+and+Dolby+ATMOS+CINEMA+PROCESSOR+CP850+device+in+AES67+mode>

## What this confirms about the CP850 itself

- **Tested against CP850 firmware V2.3.1.4** — later than the 2014 Issue 2
  manual's era, consistent with AES67 being a firmware-delivered
  "enablement" rather than something Issue 2 ever documented.
- AES67 mode lives in **System > Network > Dolby Atmos Connect tab**, with
  a **"legacy mode" checkbox that must be unticked** to use AES67 instead
  of the DAC3201-era protocol. This is the first real confirmation this
  driver has that the CP850's AES67 mode has an actual UI distinct from
  legacy/DAC3201 mode — the Dolby-authored sources only said the
  capability exists via "enablements," never described the toggle.
- **PTP role, independently confirmed a third time**: "Make sure the
  [Merging] device is set to PTP Slave... This will elect the CP850 as PTP
  GrandMaster." Same processor-is-master / downstream-device-is-slave
  relationship the DMA manual (amplifier fixed at priority 255) and the
  CP950/CP950A manual (processor defaults to priority 127 vs. 128) already
  established — now confirmed independently for CP850 too, by a party with
  no reason to align with Dolby's own documentation. Matches this driver's
  existing CP850 `ptpRole = ForcedSlave`.
- Sample rate 48 kHz used throughout the worked example — consistent with,
  not contradicting, this profile's existing 48/96 kHz assumption.

## What this does NOT confirm (and why the numbers below aren't in the code)

The guide's own worked example uses:
- PTP domain **0**, both PTP priorities **100** (not Dolby's own 109/127
  seen in the DMA and CP950/CP950A manuals)
- Destination multicast IP **239.1.25.20**, derived from the *Merging*
  device's own unicast address by the integrator's own convention (not
  Dolby's shared factory default 239.81.83.67)
- Source UDP ports 6517/6519/6521 and RTP destination ports
  6518/6520/6522 — the *opposite* pairing from the DMA/CP950A manuals'
  documented Dolby default (there: destination fixed at 6517, source
  stepped from 6518)

These read as the integrator's own **advanced-mode** values chosen to
match the *Merging* receiver's own requirements, not a second confirmed
Dolby default — the CP950/CP950A manual explicitly documents an
"advanced mode" checkbox precisely for "set[ting] up a configuration for
a device supplied by any party other than Dolby." Since Merging is
exactly that, a non-Dolby device, its own port/IP numbers appearing here
say nothing about what a CP850 talking to *Dolby* gear (DAC3202, DMA)
would use. `CompatibilityProfile`'s CP850 case therefore keeps its
existing values (`recommendedPtpDomain` unset, no
`recommendedMulticastAddress`, no `useFixedMulticastWithPerFlowSourcePort`)
rather than adopting this example's numbers — recorded here so a future
reader who finds this page doesn't "correct" the profile to match an
interop example that was never meant to be a default.
