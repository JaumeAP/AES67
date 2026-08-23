# References

Third-party manuals kept locally because they inform CompatibilityProfile
constraints (NetworkEngine/CompatibilityProfile.h) and are the source for
claims made in its caveats text. Not authored by this project — copyright
belongs to the original publisher (Dolby Laboratories).

- `dolby_multichannel_amplifier_manual.pdf` — Dolby Multichannel Amplifier
  (DMA) User Manual, Issue 7 (May 2025), part number 8800376. Covers the
  DMA16301/16302, DMA24300/24302, and DMA32300/32301 models. Source for the
  "DMA" CompatibilityProfileKind: confirms the amplifier is always PTP
  slave (fixed clock priority 255, §3.2.13 "PTP Domain Number"), the
  default PTP domain for Dolby installs is 109 (not 0), and the Dolby
  Atmos Connect port scheme (§3.2.4, "Source UDP and RTP Destination
  Ports") — a fixed RTP destination port (6517) with the source UDP port
  varying per 8-channel block (6518, 6519, 6520, 6521, ...), which does
  not match this driver's own flow-splitting scheme
  (StreamManager::createTxStreamFlows(), which instead increments the
  destination multicast IP's last octet per flow).
  Retrieved from
  <https://professional.dolby.com/siteassets/products/dolby-audio-products/dolby-multichannel-amplifier/dolby_multichannel_amplifier_manual_8800376_issue_7.pdf>.
