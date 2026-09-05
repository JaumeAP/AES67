# Continuity notes

## Read this first

This repository changed project and history on 2026-08-24. What is here now
has NO continuity with what was here before. If you arrive with context from
an earlier session, treat it as expired.

**The old project** was an AES67 PTP grandmaster on a Raspberry Pi 5 with
Linux: installers, Digispark firmware, `driver-rpi/`, 170 tests, and the
`AES67-ddriver` submodule with the daemon and its web UI.

**The current project** is a PTP grandmaster on a Teensy 4.1, no Linux, bare
metal. It shares the idea (disciplining PTP with a PPS from the word clock) and
nothing else: not a line of code, no installers, no daemon, and none of that
web UI. What it has grown since -- a configuration server of its own, and host
tests in the library -- it grew here, and shares no ancestry with what was
there before.

## What happened to the old history

At the user's express request, step by step and confirming every action:

1. `main` was reset to `75fecc2` ("Initial commit") and force-pushed. All the
   history after it disappeared from the default branch.
2. `main` was emptied of files (commit `eec115b`).
3. PR #33 was closed without merging.
4. Deleting the remaining 38 branches was attempted: it **did NOT work**. The
   git proxy blocks `push --delete` with a 403 and there is no MCP tool to
   delete branches. They are all still on the remote.
5. `.claude/` and `CLAUDE.md` were removed from all 38, one commit per branch.

**Practical consequence**: the old project has NOT been lost. It lives on those
38 remote branches (`claude/webui-use-submodule`, for example), minus their
`.claude/` configuration. If anybody wants it back, it is there. If anybody
wants the branches really deleted, it has to be done from outside this session.

All of `.claude/` was lost too: skills, hooks and settings. The only thing
recreated is `CLAUDE.md`, with a single line ("Always reply in Catalan").

## The configuration server

The box serves a page on port 80 with the list of PTP profiles and the current
choice marked. The choice is stored in EEPROM and applied without a restart.

Profiles, in `src/profiles.cpp`: `master-box` (the default, and exactly what
the box did before profiles existed), `conservative` (1 sync/s, announce every
2 s), `tight` (16 sync/s, 50 ns lock window) and `standby` (like the default
but with priority1 200, so as not to win the election if there is another
master).

A profile carries the domain, the two intervals, the two priorities and the
lock window. It does NOT carry the `clockClass` or the `timeSource`, which
depend on whether there is PPS and not on the network; nor the servo gains and
boundaries, which are hardware tuning; nor the UTC offset, which is a property
of this box.

It goes by a closed list and not field by field on purpose: letting the values
be changed one at a time from a web page invites combinations that do not hold
together, such as the announced interval and the real rate coming apart. That
pairing used to be held by a `static_assert`; now it is guaranteed by both
coming out of the same `logSyncInterval` field.

Things to know before putting it into production:

- **It is plain HTTP with no password.** Anyone who can reach the box over the
  network can change its profile. It is meant for the audio network, which is
  not the internet.
- It costs 13296 B of flash and 2240 B of RAM1 (from 124304/48992 to
  137600/51232).
- None of it has been tested on hardware, like the rest of the project. Not the
  server, not the EEPROM, not the hot timer rate change.

### Where the old project's web UI ended up

Not to be confused with the configuration server described above, which this
box does have, on port 80, written here from nothing. What follows is about
the React interface of the Raspberry Pi project, which is gone from `main` and
worth being able to find.

The network configuration, unlike the profile, is still compile time only:
`src/net_config.h` and `platformio.ini`. The old UI belonged to the daemon,
and it can be located:

- The real web UI goes with the daemon, in the `AES67-ddriver` submodule ->
  `https://github.com/JaumeAP/AES67-ddriver-.git`. The pointer changes by
  branch: `83d136b` on `claude/webui-use-submodule`, `aca6c10` on
  `claude/remove-old-react-webui`.
- In the main repository there remains a React copy (Vite: `index.html`,
  `package.json`, `src/Tabs.jsx`, `src/main.jsx`, `src/styles.css`,
  `vite.config.js`) at `webui-daemon-src/`, inside the branch
  `claude/webui-use-submodule` (`5f49b2d`). Those are the 4034 lines the branch
  `claude/remove-old-react-webui` (`9c2d975`) removes: that branch is exactly
  the one that deleted it from the main repository.
- Related: `claude/bump-submodule-webui-buttons` (`e3b2c8b`) and
  `claude/dedupe-daemon-src` (`5fff049`).

None of that is on `main`, which is the Teensy project and contains no trace of
it.

## Before buying anything: read COMPARATIVA-P4.md

Comparing this project with `JaumeAP/DTS-Player` (ESP32-P4) turned up that this
box's only technical advantage, hardware packet timestamping, **is no advantage
at all**: the P4 has it too, and what seemed to prevent it was a restriction of
one of its build paths, not of the silicon. On top of that, that project
already has BMCA, AES67, RTP, tests, and the word clock input stage already
designed.

`COMPARATIVA-P4.md` goes into detail. The conclusion is that perhaps this
project should not be continued separately. No decision has been taken.

`COMPARATIVA-SERVO.md` compares the two clock servos piece by piece. Its first
recommendation -- separate the servo from the hardware as they do, because
until then nothing in the servo can be tested without a Teensy connected -- has
been carried out in the library: see "The clock servo, now testable" below. It
also records two weaknesses of our servo that come from t41-ptp upstream, and
those are still there: the decision was moved, not rewritten.

`COMPARATIVA-CORE.md` weighs putting the profile definitions, the
configuration web server and the daemon into `JaumeAP/aes67-core` so one copy
serves every implementation. **It was decided against**, and the opposite move
taken instead: `aes67-core` folds back into `aes67_macos_driver`, and
`MIGRATION-CORE.md` is the plan for that. What it means here is nothing --
this box stays standalone, with its own profiles and its own server, exactly
as it is -- and the reasoning for both directions is in the first document,
with the decision marked at its top.

## Work left in the DTS-Player repository

Three documentation branches, no PR open, none merged. All three are additions
only, zero lines deleted, and they have been verified to merge cleanly in any
order:

- `claude/l2tap-findings` — L2TAP CAN be enabled, but only on its ESP-IDF build
  path, not the Arduino one. It expands the comment in `PtpHardwareClock.h`,
  which said the opposite and had gone stale.
- `claude/clock-only-firmware` — a proposal: split their firmware into a base
  (PTP grandmaster from word clock, which is exactly this box) and theirs as
  the base plus the DTS layer, over a single `lib/`.
- `claude/servo-comparison` — the servo comparison, seen from their side.

If anybody picks this up, those branches are the starting point for the
decision on whether this project continues.

## Hardware

Teensy 4.1 (NXP i.MX RT1062). The full reasoning for why this board and not an
Intel I210 or an RPi5, the bill of materials and the alternatives ruled out are
in `HARDWARE.md`. **The board was not available. Nothing here has ever been run
on hardware.**

To buy: the Teensy 4.1 and PJRC's Ethernet Kit separately, because the board
carries the controller and the PHY built in but not the RJ-45 connector. For
the enclosure, a desktop one has been decided (3D printed or acrylic) while
there is nothing validated; the rack path, and why PJRC's kit does not serve
there, are in `HARDWARE.md`.

Pins fixed by the silicon, they cannot be chosen: pin 15 PPS IN, pin 24 PPS
OUT, pin 13 lock LED.

## Structure

    platformio.ini      teensy41 environment, teensy platform, arduino framework
    src/main.cpp        the PTP master and the profile it announces
    src/wordclock.{h,cpp}
                        word clock to 1 PPS divider using QuadTimer 3
    src/profiles.{h,cpp}
                        predefined PTP profiles and the choice stored in EEPROM
    src/webconfig.{h,cpp}
                        HTTP server on port 80 for picking the profile
    src/net_config.h    network configuration (DHCP, static fallback, hostname)
    lib/t41-ptp         symbolic link to packages/t41-ptp, the sibling package
    lib/t41-ptp/libraries/QNEthernet
                        the fork JaumeAP/QNEthernet, branch multicast-ttl,
                        which the package carries
    HARDWARE.md         hardware, libraries and the PTP profile announced

`lib/` is PlatformIO's default library directory. QNEthernet, on the other
hand, hangs off t41-ptp, which is what includes it, and `lib_dir` does not
descend into the libraries it finds there: that is why `platformio.ini` adds
`lib_extra_dirs = lib/t41-ptp/libraries`. Nothing has to be initialised any
more: both used to be submodules, and inside the monorepo the link resolves on
its own.

## The split between the library package and this one

The rule is who owns each thing, not where it first ended up:

**In the library package, library code and nothing else.** `packages/t41-ptp`,
the fork of `IMS-AS-LUH/t41-ptp`: the two changes offered upstream
(configurable ANNOUNCE dataset and the `currentUtcOffsetValid` bit), the nine
fixes from the library audit, the servo split and its host tests, and
QNEthernet under `libraries/`. `examples/` is upstream's, untouched.

**Here, everything that is application**, that is whatever the person writing
the sketch decides and not the library:

- The profile announced: `clockClass` 13 locked / 248 free, `timeSource` 0x90 /
  0xa0, sync at 8 Hz, announce at 1 Hz, `clockAccuracy` and variance unknown,
  UTC 37 marked invalid. All in `src/main.cpp`, calling the setters the library
  now offers.
- The PTP domain (`kDomainNumber`, 0), the maximum drift before declaring the
  reference invalid (`kMaxDriftNsps`, 100000 ns/s = 100 ppm) and the servo
  gains (`kServoKp` 1.0, `kServoKi` 0.5). All three are the values the library
  already used; what changes is that they are now chosen here and visible. The
  first two were hardcoded in the library and `setDomainNumber()` and
  `setMaxDriftNsps()` were added to it (commit `91e1751`).
- The definition of locked (`kLockThresholdNs`, 100 ns) and the two servo mode
  boundaries (`kFreqModeThresholdNsps` 1000 ns/s, `kCoarseModeThresholdNs`
  1000 ns). The first one counts: the lock count it feeds is what decides
  whether we announce `clockClass` 13 or 248, that is, this number is what the
  network reads as "this box is locked". New setters in the library at commit
  `d249676`. None of the three values has been measured on hardware: they are
  the ones that were hardcoded.
- The library's serial log level, picked with the build flag
  `-DT41PTP_LOGGING_LEVEL=0` in `platformio.ini`. It was a file-scope constant
  inside the library; now it is a macro defaulting to 0 (commits `d82f7b4` and
  `5569e1f`).

  It goes through a macro and not a call because both were tried: with a
  run-time `setLoggingLevel()` the compiler can no longer drop the
  switched-off `Serial.printf` calls or their literals, and the binary grew by
  1472 B of flash and 1024 B of RAM without turning anything on. Measured:
  124304/48992 with the macro against 125776/50016 with the setter.

  Verified that the flag really does reach the library by building with level 1
  (125456 B of flash) and going back to 0.
- The 1588 compare channel delay (`kCompareChannelDelayNs`, 60 ns). This one is
  NOT ours: it is a property of the silicon and should live in the library, but
  the library does not configure those channels, the sketch does. It has stayed
  here with a name and in one single place, instead of the literal 60 repeated
  twice.
- The BMCA priorities (`kPriority1` and `kPriority2`, both 128). They are the
  standard's neutral value and what the library already used; they are now
  chosen here because the BMCA looks at them before the `clockClass`, that is,
  who wins the master election depends on these two numbers and not on the
  clock's quality. Nobody has yet decided what role this box should play on the
  network it will go on.
- The word clock to 1 PPS divider: `src/wordclock.{h,cpp}`.
- The state shared with the ISRs: the `volatile`s, `takeFlag()` and the
  coherent copy of the four `int64_t`s.

The equivalent tweaks that had been made in the submodule's
`examples/PTPMaster/PTPMaster.ino` have been undone (commit `7399efd`): they
were duplicated application code, and the one that counts is the one here.

Practical consequence: if the library changes ever land upstream, the fork can
be dropped without touching anything in the profile, because the profile does
not live there.

## Second pass over the split: AES67 here, IEEE 1588 in the library

The split above was made by ownership: library code in the submodule,
application code here. It was audited again against a different rule --
anything AES67-specific belongs here, anything the IEEE 1588 standard itself
defines stays in the library -- and under that rule nothing had to move.

**The library holds no AES67-specific code.** The announced dataset, the
domain, the two priorities, the two message intervals and the lock window all
live here already, in `src/main.cpp` and `src/profiles.cpp`. What looks
network-specific inside the library is plain IEEE 1588 and is shared with
every other profile, AES67 included:

- multicast `224.0.1.129` (E2E) and `224.0.0.107` (P2P), ports 319 and 320,
  in `src/ptp/l3ptp.cpp` -- Annex D.
- MAC `01:80:C2:00:00:0E` and `01:1B:19:00:00:00`, EtherType `0x88f7`, in
  `src/ptp/l2ptp.cpp` -- Annex F. AES67 itself runs over Annex D, but the L2
  transport is standard and costs nothing where it is.
- the PTPTimescale flag, the two-step flag, portNumber 1 and the `0x7f`
  logMessageInterval of the generic header, in `src/ptp/ptp-base.cpp`.

**Nothing standard is stranded here either.** Everything in `src/` is either
the announced profile, the word clock divider, the network setup, the web
server, or the state shared with the interrupt handlers.

**And the audio arrives here, and has started to.** `src/audio` is the AES67
tone sender -- one channel, 48 kHz, L24, a millisecond a packet, a 1 kHz sine
at -20 dBFS RMS, its RTP timestamp taken from the PTP timeline -- and
`src/nmos` is the IS-04 registration. Both came out of `lib/t41-ptp`, where
they had been written and where they did not belong, with their tests:
`make -C test` runs them against the library's own host stubs. The rule they
are the first case of stands as it was written. Everything that carries or
describes a stream belongs in this repository and not in `lib/t41-ptp`:

- sending and receiving audio: the RTP transmit and receive paths, the packet
  timing, whatever buffering it ends up needing.
- describing it: SDP, and SAP announcing and listening.
- being found: the discovery around them, whatever form it takes.

That library is IEEE 1588 and nothing else, which is what makes it something
that could still go back upstream one day. A media stream inside it would end
that, and would be in the wrong place besides: the whole reason the split was
made by ownership is that the sketch decides what the box does with audio and
the library decides nothing about it.

The line is not "does it use the network" -- the library is full of sockets --
it is "does IEEE 1588 define it". A Sync message does; an RTP payload, an SDP
body and a SAP announcement do not.

### What is neither, and why it stays upstream

Three things in `lib/t41-ptp/src/ptp/ptp-base.cpp` are neither AES67-specific
nor defined by the standard. All three came from upstream untouched -- they
are already in commit `541d9f3`, and further back -- and the decision is to
leave them in the library:

- `hwOffset = -200` (line 7), the MAC and PHY latency, added to `t1`, `t4` and
  `t4s`. This is the one item the earlier split left pending; it is hardware
  calibration and still unmeasured on this board.
- `+500` in the P2P offset. Same nature, undocumented upstream, and unused
  here because the box runs E2E.
- the clock servo itself: the PI loop, its KP and KI, the frequency and coarse
  mode boundaries, and the drift limit. IEEE 1588 does not specify a servo, so
  strictly it is not standard either. It stays in the library, but it is no
  longer welded to the hardware: its decision is now `servoUpdate()` over plain
  numbers, which is what made it testable. See "The clock servo, now testable".

This repository introduced none of them. It only made the third settable --
`setMaxDriftNsps()`, `setFreqModeThresholdNsps()`, `setCoarseModeThresholdNs()`
and `setLockThresholdNs()`, alongside the KP and KI setters upstream already
had. `hwOffset` still has neither a setter nor a macro: it is the one number in
the library the application cannot choose.

## The clock servo, now testable

`PTPBase::updateController()` used to compute the drift, choose between the
four modes and call `EthernetIEEE1588` from inside the choosing. That is why
the library had never had a test of its servo: there was no way to ask it what
it would do with a pair of timestamps without a Teensy on the desk.

The decision is now `t41ptp::servoUpdate()` in
`lib/t41-ptp/src/ptp/ptp-servo.{h,cpp}`, over plain numbers -- no Arduino, no
QNEthernet, no clock. It returns what should happen; `updateController()` does
it and logs it. The three loose members it used to keep (`lockcount`,
`nspsAccu`, `driftNSPS`) are one `ServoState`.

**The behaviour did not change.** The modes, the thresholds, the accumulators
and the lock counting are the code that was there, moved rather than
rewritten. In particular the integral accumulator is still a 32-bit int:
widening it would change what happens when it overflows, and that is not a
decision anybody has taken. The two weaknesses `COMPARATIVA-SERVO.md` records
are therefore still present.

`lib/t41-ptp/test/host/test_servo.cpp` has 73 checks over the four modes, their
boundaries, the accumulators and the lock count. Run them with
`lib/t41-ptp/scripts/run-host-tests.sh` -- g++, no board, a couple of seconds.
Each case was checked to fail when what it covers is broken.

Two things it deliberately does not do:

- **It does not check that the firmware still builds.** That needs the Teensy
  toolchain, and is a separate thing to run: `pio run`, which has now been
  done and is recorded under "Building".
- **It does not cover the `isfinite()` guard**, and cannot: the timestamps are
  `int64`, so once `t1diff` is not zero the drift is finite by construction.
  There is a note in the test file saying so, rather than a test that does not
  test it.

Along the way `getTxFailureCount()` was removed from the library: nothing
called it -- not this box, not the examples, not `aes67-core`, not
`aes67_macos_driver`. The counter stays protected, and so does the check on the
result of `send()` that feeds it, which is the audit finding.

## Building

    pio run

PlatformIO does not come installed in the sandbox: `pip install platformio`.
The Teensy toolchain downloads itself and the proxy lets it through.

Verified when closing the session with a **fresh clone from GitHub**, not with
the working tree: `git clone --recursive` and `pio run` from scratch, exit
code 0. That is, what is published built on its own, with the QNEthernet
submodule initialising correctly. It took 123 KB of flash and 47 KB of RAM1.

Verified again with QNEthernet now hanging off t41-ptp: `pio run` again, exit
code 0 and the same figures (123664 B of code in FLASH, 48960 B of variables in
RAM1). The `lib_extra_dirs` does its job; the dependency graph finds QNEthernet
there and builds with `-Ilib/t41-ptp/libraries/QNEthernet/src`, that is, it is
the nested one and not any downloaded copy.

And once more after the split, with the submodule's example returned to
upstream: `pio run` exit code 0, the same figures again, and the only project
object compiled is `src/main.cpp.o`.

Both branches of the `#if AES67_USE_DHCP` have also been compiled, not just the
default one, so that the static configuration path does not rot without anybody
noticing.

And once more after the three library bumps of 2026-09-03 -- the servo split,
its boundary tests and the removal of `getTxFailureCount()` -- which is the
first time any of them has been through the Teensy toolchain. Clean build from
scratch, 148 objects, exit code 0, both branches of the `#if AES67_USE_DHCP`
again:

    DHCP (the default)  FLASH code 138500, data 19400, headers 9008
                        RAM1 variables 52256
    static              FLASH code 138244, data 19400, headers 8240
                        RAM1 variables 52256

The static path is the smaller of the two, by 256 B of code, and carries the
same variables.

Against the 137600/51232 recorded above for the web server, the three bumps
cost 900 B of flash and 1024 B of RAM1.

**No warnings at all**, which is a change: this section used to record two
coming from inside t41-ptp, an unused variable and some parentheses. The
library audit and the servo split between them took both out.

## Patches sent upstream

Three PRs to third-party code we use, out of things found along the way.
**The user says they have opened all three; this could not be verified from the
session**, because `HedgeHawk` and `IMS-AS-LUH` fall outside the GitHub scope
here.

The numbers below are not verified either: the user said "3 and 4" for the
t41-ptp ones and it was assumed they go in the order they were opened. Check
them before citing them.

1. **QNEthernet**, branch `fix-missing-math-h-include` -> `HedgeHawk` (number
   unknown).
   `#include <math.h>` was missing where `round()` is called in
   `enet_ieee1588_adjust_freq()`. With no declaration, C assumes it returns
   `int` when it returns `double`, and this is on PTP's fine frequency
   adjustment path. With GCC 14 it does not compile at all. Already fixed in
   the `JaumeAP/QNEthernet` fork (commit `3a66886`), which is where the
   submodule points.
2. **t41-ptp #3**, branch `announce-dataset-configurable` -> `IMS-AS-LUH`
   (commit `bb336fe` in the `JaumeAP/t41-ptp` fork). The ANNOUNCE dataset and
   the announced intervals were hardcoded.
3. **t41-ptp #4**, branch `utc-offset-valid-flag` -> `IMS-AS-LUH` (commit
   `69b4bfc` in the same fork). The `currentUtcOffsetValid` bit was fixed at
   false, because `buf[7]` was the literal 8.

Both t41-ptp ones come off upstream commit `541d9f3`, not stacked one on the
other, so they are independent and can be merged in any order. That was
deliberate: stacking them would have left the first PR's body not describing
its own content, and a third-party repository's PR cannot be edited from the
session.

If any of them is merged upstream, the fork or the vendored copy can be
dropped. Careful: the vendored copy of t41-ptp carries the CHANGES OF BOTH
PRs, so one alone will not be enough to go back to following upstream.

## Code audit: what was fixed and what is left

`src/main.cpp` was audited by reading the generated machine code, not just by
compiling. Six findings.

Fixed (commits `a862f98` and `cdd0a27`):

1. Data race on the PPS timestamps. The four `int64_t`s were read with
   interrupts open, so a pulse arriving in the middle mixed pulse N with N+1
   and injected up to a second of error into the servo. They are now copied
   inside a critical section.
2. `volatile` was missing from all the state shared with the ISRs. It worked by
   accident, because the calls into `ptp.*` are in another translation unit and
   forced a reread. With LTO it would have broken.
3. Read-modify-write races on the flags and on `noPPSCount`.
4. The grandmaster always announced itself as a free-running oscillator. It now
   announces the real state and changes when it loses the PPS.
5. Sync ran at 1 Hz. It now runs at 8 Hz (log -3), the sector's usual value.
   Note: 1 Hz was **legal**, AES67 allows log -4 to +1.
6. Network configuration hardcoded. The box now asks for an address over DHCP
   and only falls back to a static configuration if DHCP does not answer.
   Everything configurable lives in `src/net_config.h`. Along the way the
   `Serial.printf` that came out before the USB port was open was fixed,
   because otherwise the lines saying which network configuration was taken
   would not be seen.

7. The TAI-UTC offset was hardcoded inside the library. The value and its
   validity bit are now configurable, in `src/main.cpp` with the rest of the
   PTP profile constants.

   A nuance that appeared while looking at it: the library already announced
   the offset as **invalid** (`buf[7]` only carried the PTPTimescale bit), so
   the problem was milder than the audit made it look. It has been left as it
   was, invalid, and now deliberately and documented: the box has no source of
   absolute time, its seconds counter is arbitrary, and a leap second would
   change the 37 without our being able to find out. For AES67 audio it does no
   harm, only relative synchronisation matters.

Pending:

None from the audit: it is closed.

One is left from the split, and it needs the board:

**`hwOffset = -200` in `lib/t41-ptp/src/ptp/ptp-base.cpp`.** It comes from
upstream, it is the MAC and PHY latency in nanoseconds, and it is added to all
three timestamps (`t1`, `t2` and `t4`), that is, it directly shifts the time we
publish.

It has neither a setter nor a macro: it is the one number in the library the
application cannot yet choose. It has not been moved on purpose, for two
reasons:

- Nobody has measured it on this board. The -200 are those of whoever wrote the
  library, with their hardware, and putting them in `src/main.cpp` as if they
  were a decision of ours would be pretending to know something we do not.
- It is hardware calibration, not profile. When it is measured, the natural
  place is `src/main.cpp` with the `kCompareChannelDelayNs`, which is the same
  case.

Measuring it needs the Teensy, a time interval counter and a reference master:
the output PPS is compared against the reference's and whatever fixed bias
remains is read off. Until then, leave it as it is.

8. The Nanosyncs puts out word clock (the sampling rate), not PPS, and the code
   expects 1 PPS. The division that a separate ATtiny85 did on the old project
   was missing. The same Teensy's QuadTimer 3 now does it, with no extra
   hardware and no CPU: `src/wordclock.{h,cpp}`.

**Two things about that are still pending and without them it will not work:**

- **The physical bridge from pin 19 to pin 15.** The divider puts the pulse out
  on 19 and the 1588 capture input is on 15, and they cannot be joined
  internally. It is one wire with a 220 ohm resistor at the pin 19 end, some
  10 mm on the same header row; `HARDWARE.md`, section "The bridge, specified",
  says why the resistor and how to check the wire. Without it no PPS arrives,
  and nothing says so: the box measures its word clock, reports the divider
  started, and simply never locks.
- **The word clock conditioning circuit**, which is designed but not built. It
  is 3 Vpp into 75 ohms and the pin wants 3.3 V logic: 75 ohm termination, AC
  coupling, bias to 1.65 V, and a TLV3501 comparator with 51 mV of hysteresis.
  Values, the reasoning and the layout notes are in `HARDWARE.md`, section
  "Signal conditioning: the circuit". Connecting the word clock directly will
  not work and may do damage.

All in `HARDWARE.md`, section "From word clock to PPS", with the pins, the ALT
values and why the division goes in two stages (the counters are 16 bits).

On `src/net_config.h`: the static configuration values (192.168.1.211, gateway
.1) are a starting point, NOT a valid configuration for any particular network.
If anybody wants to depend on that path instead of DHCP, they have to change
them.

## Where the reference comes from: no GPS

t41-ptp's master mode is disciplined by an external PPS and does not care where
it comes from. Its reference setup is a GPS: `README.md` names an Adafruit
Ultimate GPS wired to the master over PPS, and that is how the fork stood when
it was taken. The GPS is the wiring and not the software, though: the whole
library entry point is `ppsInterruptTriggered(pps_ts, local_ts)`
(`src/ptp/ptp-base.cpp:818`), and there is no GPS code anywhere in it, no NMEA
and no serial port. That is why the substitution belongs here and not there,
and why the submodule keeps its GPS reference untouched.

In this repository the GPS disappears and the word clock takes its place: the
Nanosyncs feeds pin 14, QuadTimer 3 divides it down to 1 PPS, and the pulse
reaches the 1588 capture on pin 15 over the bridge from pin 19. What the box
loses by it is absolute time, not precision, which is why it announces
`clockClass` 13 and sends the UTC offset marked invalid. See HARDWARE.md,
section "From word clock to PPS".

The two cannot simply coexist: a GPS PPS and the divider pulse both land on
pin 15, the only 1588 capture input. Adding a GPS means picking the source and
reading NMEA for the time of day, not adding a wire.

## Traps that would be costly to rediscover

- **`noPPSCount` is counted in sync timer ticks, NOT in seconds.** With sync at
  8 Hz and PPS at 1 Hz, eight ticks fit between two pulses. A fixed threshold
  would have made the box declare itself unlocked every second with a perfectly
  healthy PPS. That is why the threshold goes in seconds and is converted. If
  anybody changes the sync rate again, this has to be looked at.
- **The real rate and the announced interval both come out of the profile's
  `logSyncInterval`.** If they come apart, the announcement lies. A
  `static_assert` used to hold the pairing when both were compile-time
  constants; with the profile picked at run time it is guaranteed by there not
  being two numbers.

  This is not a hypothetical worth being smug about: `aes67_macos_driver` had
  the same two numbers -- milliseconds in the settings, the log interval
  rounded from them for the wire -- and its transmit loop timed itself off the
  milliseconds while the port announced the rounded value, so anything that
  was not a power of two seconds announced one rate and sent another. Fixed
  there on 2026-09-03 by settling the exponent once and deriving both from it,
  which is what this box does. Milliseconds also cannot express `tight`: 16
  per second is 62.5 ms.
- **`clockClass` 13, not 6.** 6 is what usually gets put there, but it means
  locked to a primary reference such as GPS. The word clock gives frequency and
  one edge per second, no traceable absolute time. If a real GPS is ever added,
  this has to be revisited.
- **`clockAccuracy` and the variance stay at unknown on purpose.** Putting a
  figure there would be claiming a precision nobody has measured.
- **The critical section macros are lower case**: `__get_primask` and
  `__set_primask`, from `<util/atomic.h>`. The core carries the CMSIS ones in
  upper case in `cmsis_gcc.h`, but `imxrt.h` never includes it.

## Session limits you will hit too

- Third-party repositories cannot be forked and no PR can be opened against
  them. The scope is only `jaumeap/*`. The user has to make the forks by
  clicking; after that `add_repo` does give write access.
- `git push --delete` to the remote gives a 403 through the proxy. Branches
  cannot be deleted from here.

## Verification status, unembellished

Everything done in this repository is **compiling and reading the generated
machine code**. Nothing has ever been run on a Teensy, which was not yet
available. That it links says nothing about whether PTP works, nor about
whether the network sees it, nor about the accuracy.

The one exception is the library's clock servo: 73 host checks pass under
`lib/t41-ptp/scripts/run-host-tests.sh`. That is arithmetic run on a PC, and
it says nothing about the hardware around it -- but it is the only part of
this project whose behaviour has been checked rather than read.

The firmware carrying it does build, from a clean tree and with no warnings,
both network paths: see "Building". That closes the debt this section used to
record, and buys exactly what it always did. It links. Nothing more.

The real next step is to buy the board and the kit, and test it. `BRINGUP.md`
is the order to do it in: ten steps, each saying what to expect and what a
failure points at.
