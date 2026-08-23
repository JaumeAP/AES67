# Audio-over-IP standards — what this driver does and doesn't reach

Research notes, August 2026. Which AoIP standards exist alongside AES67, and
which of them this driver can talk to as a consequence of implementing AES67.

Companion document: `st2110_30_vs_aes67.md`, which covers the one standard
close enough to AES67 to be worth a detailed comparison.

## AES67 is the interoperability layer, not a competitor

AES67 (AES, 2013) is a layer-3 protocol suite deliberately assembled from
existing standards — RTP for transport, PTPv2 (IEEE 1588-2008) for
synchronisation, SDP for session description, multicast with IGMP for
distribution. It was written to let otherwise-incompatible AoIP systems
interoperate, not to replace them.

Its mandatory baseline is narrow and specific: **1–8 channels per flow,
16 or 24-bit, 44.1 / 48 / 96 kHz**. The 8-channel-per-flow limit is where
`StreamManager::createTxStreamFlows()` comes from — see
`StreamChannelMapper::kMaxChannelsPerFlow`.

## The AES67-compatible family

Implementing AES67 correctly means this driver can, in principle, exchange
audio with all of these. Each is a full AoIP system in its own right; AES67
is the common subset they meet on.

| System | Vendor / origin | Sector | Notes for this driver |
|---|---|---|---|
| **Dante** | Audinate | Pro AV — over 90% of deployments | Needs **AES67 mode enabled explicitly**. Uses **PTPv1** natively; AES67 mode is what switches it to PTPv2. Requires multicast in `239.69.0.0/16` — the driver's existing `239.x.x.x` check in `StreamManager.cpp` already satisfies this |
| **RAVENNA** | ALC NetworX | Broadcast | Natively AES67; adds discovery (Bonjour) and stream redundancy that AES67 itself doesn't define. See `aes67_driver_compatibility_review.md` in the `new_renderer` repo |
| **Livewire+** | Telos / Axia | Radio | AES67-compliant |
| **Q-LAN** | QSC | Installed systems | AES67-compliant |
| **WheatNet-IP** | Wheatstone | Radio | AES67-compliant |
| **Gibraltar** | — | — | AES67-compliant |

Practical consequence: the 8-channel flow splitting added for Dante
compatibility is not Dante-specific. It applies identically to Livewire+,
Q-LAN, WheatNet-IP and RAVENNA, because the limit belongs to AES67 itself.

## SMPTE ST 2110-30 — a constrained subset, not a separate target

ST 2110-30 is best understood as AES67 with extra restrictions layered on:
same transport, same packet construction, largely the same signalling.
Anything conformant to ST 2110-30 Level A is conformant to AES67's
mandatory configuration.

This does **not** need implementing as a separate feature. See
`st2110_30_vs_aes67.md` for the specific differences and where this driver
already meets them.

## AVB / TSN and Milan — a different family entirely

**Not** AES67-compatible, and not reachable by extending anything here.

- **AVB/TSN** (IEEE 802.1) operates at **layer 2**, not layer 3 IP. It
  requires switch hardware that participates in the protocol; it is not an
  IP protocol suite at all.
- **Milan** (Avnu Alliance) is a profile built on AVB/TSN. There is no
  compatibility between Milan and ST 2110, nor with AES67.

Supporting Milan would mean a separate layer-2 implementation from scratch,
not an extension of this driver's RTP/PTP stack. Recording it here so the
question doesn't get re-opened as if it were a small addition.

## Sources

- RAVENNA — Standards Comparison:
  https://www.ravenna-network.com/overview/standards-comparison/
- RAVENNA — RAVENNA / AES67 / ST 2110 comparison table (PDF, 2024):
  https://www.ravenna-network.com/wp-content/uploads/2024/04/RAVENNA_RAVENNA-AES67-ST-2110-Comparison_V03.pdf
- Audinate — Dante, AES67 and SMPTE ST 2110 interoperability (PDF):
  http://go.audinate.com/hubfs/campaign/DDM/broadcast/audinate-dante-domain-manager-broadcast-aes67-smpte-2110-interoperability-wp.pdf
- Avnu Alliance — Milan Specification:
  https://avnu.org/resource/milan-specification/
- AVT — Audio over IP quick guide, AES67/Dante/RAVENNA/Livewire+ (PDF):
  https://www.avt-nbg.de/sites/default/files/downloads/quick-guides/audio_over_ip_aes67_dante_ravenna_livewire_000757_v0003.pdf
- Wikipedia — AES67: https://en.wikipedia.org/wiki/AES67
