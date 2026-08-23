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
  varying per 8-channel block (6518, 6519, 6520, 6521, ...). This differed
  from this driver's own flow-splitting scheme at first (it instead
  incremented the destination multicast IP's last octet per flow); fixed
  in StreamManager::createTxStreamFlows() via
  CompatibilityProfile::useFixedMulticastWithPerFlowSourcePort.
  Retrieved from
  <https://professional.dolby.com/siteassets/products/dolby-audio-products/dolby-multichannel-amplifier/dolby_multichannel_amplifier_manual_8800376_issue_7.pdf>.

- `dolby_dac3202_product_sheet.pdf` — Dolby Atmos Connect Interface
  DAC3202 product sheet (2 pages). Overview/specs only — Dolby doesn't
  publicly host a standalone DAC3202 user manual; its network/AES67
  configuration is documented instead in the sending processor's own
  manual (see `dolby_cp950_cp950a_manual.pdf` below) and in the DMA
  manual above (§2.5.1, DAC3202s share a network segment with DMAs the
  same way). Retrieved from
  <https://professional.dolby.com/siteassets/cinema-products---documents/dolby-dac3202-product-sheet.pdf>.

- `dolby_cp950_cp950a_manual.pdf` — Dolby Cinema Processor CP950 and
  Dolby Atmos Cinema Processor CP950A Manual, Issue 13 (15 August 2024),
  part number 8800298. CP950/CP950A are the current-generation
  equivalents of the CP850 CompatibilityProfileKind covers — same Dolby
  Atmos Connect/AES67 sending role. §3.8 "Modifying the network settings"
  documents the processor (sending) side of the same AES67 link the DMA
  manual documents from the receiving side: the Dolby Atmos Connect tab's
  static source IP address and AES67 settings used to reach "a Dolby
  Multichannel Amplifier or Dolby DAC3202 using AES67". Not yet mined for
  profile-affecting specifics beyond confirming the shared protocol —
  kept for whenever CP850/DAC3202's own constraints need the same level
  of verification the DMA profile now has. Retrieved from
  <https://professional.dolby.com/siteassets/products/cp950a/dolby_cp950-cp950a_manual_issue_13.pdf>.
