# Auto-detecting DAC3202 units: is it possible, and how many?

Feasibility study. **No code written** — this evaluates whether the driver
could detect the presence and count of Dolby DAC3202 amplifiers on the
network, which signals are available to do it, and what each would cost and
be worth.

## The core difficulty

In the DAC3202 (and DMA) topology, the amplifier is a **receiver**: this
driver is the AES67 **source** (profile `direction = TransmitOnly`, `ptpRole
= ForcedMaster`), the amplifier renders nothing of its own and only consumes
the Atmos Connect flows we send. A pure multicast receiver is **silent at the
SAP/SDP layer** — SAP announces *sources*, not sinks — so the discovery path
this driver already has (`SAPListener` / `SAPAnnouncer`) cannot see a DAC3202
at all. It never sends an SDP; it only joins a group and listens.

So "is a DAC3202 there, and how many" has to be answered from some *other*
signal the amplifier necessarily emits. The candidates, best first:

## Vector A — observe PTP slaves (recommended)

For exactly the profiles where this matters (DAC3202, DMA) the driver is
`ForcedMaster` on **PTP domain 109**, and the amplifiers are forced slaves.
A PTP slave is **not** silent: to measure its path delay to the master it
sends **Delay_Req** (end-to-end mechanism) or **Pdelay_Req** (peer-to-peer),
each carrying the slave's own `sourcePortIdentity.clockIdentity`. The master
sees every one of them.

Counting the **distinct** slave clock identities that talk to us on domain
109 therefore yields the number of PTP slaves present — which, in a Dolby
Atmos Connect install where the only domain-109 slaves are the amplifiers,
is the number of amplifier units.

### Why it isn't happening today

`PTPMaster::receiveThread()` only reads the **general** socket (port 320) and
only acts on **Announce** messages (for BMCA). Delay_Req/Pdelay_Req are
**event** messages on port **319**, and although `PTPMaster::createSockets()`
creates `eventSocket_`, it binds and multicast-joins only the general socket;
the event socket is set up for *sending* Sync, never for receiving. So the
Delay_Req packets are on the wire, addressed to us, and we simply don't look.

### What it would take

1. Bind `eventSocket_` to port 319 and join the PTP multicast group on it,
   then `recv()` it in `receiveThread()` alongside the general socket
   (the header parse already extracts `sourcePortIdentity`).
2. On a Delay_Req (0x01) or Pdelay_Req (0x02), record
   `clockIdentity -> lastSeen` in a small timed set (evict after N missed
   intervals, mirroring how `SAPListener` ages sessions). Expose `count()`
   and the identity list through `PTPDiagnostics` and the existing
   diagnostics custom-property gateway.
3. **Refine to "Dolby" with the OUI.** A PTP clock identity is the slave's
   MAC as an EUI-64 (`PTPClockIdentity::fromMAC`), so bytes 0–2 are the
   vendor **OUI**. Filtering the count to Dolby's OUI(s) turns "N PTP slaves"
   into "N Dolby devices," which is much closer to the real question and
   rejects an unrelated slave that happens to share the domain.

Effort: **moderate**, and additive — it observes traffic already arriving,
reusing the existing socket, header parser, and diagnostics channel. No
real-time-path change.

### Honest limits

- It counts PTP **slaves**, not DAC3202s specifically. The OUI narrows it to
  "Dolby gear"; PTP alone **cannot tell a DAC3202 from a DMA or a CP950** —
  they'd share a vendor OUI. "How many Dolby PTP slaves" is the honest claim,
  not "how many DAC3202."
- It only sees a slave that actually runs the delay mechanism and sends
  Delay_Req/Pdelay_Req. The AES67 media profile mandates end-to-end delay
  request, so a conformant amplifier should be visible; a device parked in a
  passive/monitor mode that consumes Sync without measuring delay would not.
- Unicast delay request (negotiated via Signaling) would not arrive on the
  multicast event socket; AES67's default is multicast, so this is a corner
  case, but worth stating.
- It only holds while **we** are the elected master. The DAC3202/DMA profiles
  force that (`ForcedMaster` vs the amps' `ForcedSlave`), so in-profile it
  holds; out of profile it doesn't apply.

### Bonus: it also fixes a real gap

We don't currently reply **Delay_Resp** to a slave's Delay_Req (we never read
the event socket). That means a real DAC3202 slaving to us cannot complete its
path-delay measurement and its sync to us is degraded. Adding event-socket RX
for detection is the same plumbing needed to answer Delay_Req properly — the
detection work and the correctness fix overlap.

## Vector B — RTCP receiver reports (possible, not built)

AES67 receivers **should** send RTCP Receiver Reports back toward the source
(RTP port + 1). If the DAC3202 does, an RTCP listener on each transmit
stream's RTCP port would count receivers by distinct RTCP sender SSRC /
source IP — directly counting *stream* receivers, vendor-neutrally, without
depending on PTP roles.

But there is **no RTCP anywhere in the codebase** (`grep` finds none), so this
is a new subsystem, not a small addition. And whether a DAC3202 emits RTCP at
all is unverified without the hardware. Higher effort, uncertain payoff —
lower priority than Vector A, though it's the more general answer if built.

## Vectors rejected

- **IGMP membership.** The amplifier joining our multicast group is an IGMP
  event, but a host cannot see *other* hosts' IGMP joins from userspace; that
  visibility lives on the switch (IGMP snooping), not on this driver's host.
  Not feasible from the driver.
- **mDNS / Bonjour device advertisement.** Would need the mDNS subsystem
  (evaluated and deferred separately) *and* a known Dolby service type, which
  isn't documented for the DAC3202. Speculative on both counts.
- **Dolby control protocol.** The DMA/DAC "advanced mode" configuration is a
  control channel, not a discovery protocol, and is undocumented for querying
  a unit count. Out of scope.

## Recommendation

If auto-detection is wanted, **build Vector A**: observe PTP Delay_Req /
Pdelay_Req on the event socket, count distinct slave clock identities on
domain 109, and filter by Dolby OUI. It reuses infrastructure that already
exists, is confined to the control plane (no audio-path risk), answers "is a
Dolby amp present and roughly how many" honestly, and drags along the
Delay_Resp correctness fix for free. Report it as a **count of Dolby PTP
slaves**, capped for display at the profile's `maxUnits` (3), and never dress
it up as positively identifying the DAC3202 model — PTP can't do that.

Everything here is a paper analysis; none of it is verifiable without a real
DAC3202 (or another PTP slave) on the wire, so any implementation lands behind
the same "unverified against hardware" caveat the PTP subsystem already
carries.
