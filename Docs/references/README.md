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
  replacement line for CP850 (the manual says so explicitly) — same Dolby
  Atmos Connect/AES67 sending role. Source for the "CP950"
  CompatibilityProfileKind and for upgrading DAC3202 (and DMA) from
  assumption to confirmation. §3.8 "Modifying the network settings"
  documents the processor (sending) side of the same AES67 link the DMA
  manual documents from the receiving side: same RTP source/destination
  port table (fixed destination 6517, source stepped 6518/6519/6520/...,
  explicitly shared by "a Dolby Multichannel Amplifier or Dolby DAC3202"),
  factory-default PTP domain 109, factory-default destination multicast
  address 239.81.83.67 (named as shared out of the box by CP950/CP950A,
  DMA, and DAC3202), and PTP priority defaulting to 127 vs. downstream
  devices' 128 — confirming the processor wins grandmaster by default,
  the CP850/CP950/CP950A profiles' ForcedSlave role. §3.8 also notes
  installers routinely override the PTP domain and multicast address per
  auditorium in multi-screen installs — these are factory defaults, not
  requirements. Retrieved from
  <https://professional.dolby.com/siteassets/products/cp950a/dolby_cp950-cp950a_manual_issue_13.pdf>.

- `dolby_cp850_base_product_sheet.pdf`, `dolby_cp850_line_product_sheet.pdf`
  — official CP850 product sheets (2 pages each, March/August 2020). Same
  situation as DAC3202: Dolby doesn't publicly host a standalone CP850
  installation/user manual (its product page only lists these sheets and
  an EULA) — likely because CP950/CP950A has superseded it. No AES67/PTP/
  network specifics in either sheet, only feature/physical/power specs.
  One real fact worth noting: "A Dolby Atmos Cinema Processor CP850
  together with a single Dolby Atmos Connect Interface can support up to
  48 speaker feeds" (CP850's own 16 built-in analog outputs + one
  DAC3202's 32) — a real-installation total, not a change to either
  profile's own maxTotalChannels (CP850's 64ch render cap and DAC3202's
  32ch output cap are each independently correct and unaffected).
  Retrieved from
  <https://professional.dolby.com/siteassets/cinema-products---documents/dolby_cp850_base_product_sheet.pdf>
  and
  <https://professional.dolby.com/siteassets/products/cp850/dolby_cp850_line_product_sheet.pdf>.
