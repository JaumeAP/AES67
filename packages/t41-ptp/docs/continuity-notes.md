# Continuity notes

## State

**What this is.** An audited fork of `IMS-AS-LUH/t41-ptp`: IEEE 1588 for the Teensy 4.1 over
UDP multicast (`l3PTP`) or raw 802.3 frames (`l2PTP`). `CLAUDE.md` carries the architecture and
the commands; `.claude/rules/working-agreement.md` carries how work is delivered. Neither is
repeated here.

**History policy.** `main` is two base commits — the upstream snapshot as this fork found it,
and everything the fork changed on top of it — plus whatever has landed since. The `up` remote,
every other branch and the eleven pull requests offered upstream were deleted on 2026-09-04, and
the objects were pruned. Nothing about *why* a line looks the way it does survives in `git log`:
the comment at the fix site is the only record, which is why every fix carries one.

**Verification.** `make -C test EXTRA_CXXFLAGS=-Werror` — 887 checks, and CI builds with the
same flag. `make -C test board` builds the example for the board through PlatformIO; run it
locally on the Mac, never in a macOS CI job. Both pass as of this handoff.

## Open

1. **The servo amplifies path delay variation at eight Sync a second.** Measured in a host
   loopback — master and slave in one process, the servo's own output fed back into a clock
   model — which has since been deleted along with the rest of that session's scratch work:
   with a clean path the error settled at 17 ns, but at 10 µs of one-way PDV the clock error was
   33 µs and at 50 µs it was 366 µs, while at one Sync a second the same jitter gave 3.2 µs and
   18 µs. `getLockCount()` never left zero above about 2 µs of PDV, because the lock window is
   100 ns. The feedforward drift term is a single unfiltered ratio of two consecutive Sync
   pairs, so its noise is PDV divided by the Sync interval, and `applyProfile(AES67Media)`
   raises the rate eightfold without touching `KP`, `KI` or `KF`. Not fixed: the candidates are
   scaling the gains with the interval, filtering the drift term, or requiring a persistent
   drift before entering frequency mode. Confirming any of it means writing the loopback again.
2. **Two decisions flip after ~49.7 days of silence**, when the 32-bit `millis()` wraps:
   `externalReferenceLive()` reports the PPS live for 3 s after that long without an edge, and
   `filteredDelay()` can take a sample that old as fresh if measuring resumes. Every other time
   comparison is a difference and survives the wrap; verified against the board's types.
3. **Nothing has ever run on hardware.** No board execution, no PPS input, no interoperability
   with another implementation, no measured synchronisation accuracy. The board build compiles
   and links; that is all it says.

## Last session (2026-09-03/04)

Audited `src/ptp` three times over: correctness (a refused Announce ended synchronisation, an
unclamped interval, a clock step the PPS path never applied to its own pair), then memory,
stress and speed (a 1522-byte frame buffer for 82 bytes read, a `Pdelay_Req` flood that silenced
the master, three transmit-timestamp spins of up to a millisecond each), then the follow-ups
(the last spin, the remaining clamps, `begin()` re-initialising fully, the layer 2 destination
address). Every fix carries a regression test verified to fail without it; the suite went from
830 to 887 checks. Then the history was squashed to two commits, all branches and the upstream
pull requests were removed, and the old objects pruned.
