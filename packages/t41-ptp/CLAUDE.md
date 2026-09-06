# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An Arduino-compatible library implementing IEEE 1588 PTP (Precision Time Protocol) for the
Teensy 4.1, over either UDP multicast (`l3PTP`) or raw 802.3 frames (`l2PTP`). It underlies the
paper "Sub-Microsecond Time Synchronization for Network-Connected Microcontrollers"
(doi: 10.1109/ICCE59016.2024.10444401). This fork carries an audit of the upstream implementation
(IMS-AS-LUH/t41-ptp) and the fixes that came out of it — see the "What this fork changed" section
of README.md, and the comments throughout `src/ptp/ptp-base.h`/`.cpp`, which document each defect
they fix and why it mattered.

## Commands

Host-side unit tests (the primary thing to run while developing):

    make -C test          # build and run
    make -C test clean

CI builds with `-Werror` (`EXTRA_CXXFLAGS=-Werror`) — match that locally before pushing:

    make -C test EXTRA_CXXFLAGS=-Werror

There is no per-test filter: `test/test_main.cpp` runs `runPtpBaseTests()` and
`runTransportTests()` unconditionally in one binary, and the `CHECK`/`CHECK_EQ` macros
(`test/test_harness.h`) just count failures and print `FAIL file:line: expr` — grep the output for
the file/line of interest.

Board build (compiles the example for actual Teensy 4.1 hardware via PlatformIO; needs `pio` on
the path, `PIO=` to override the command, and the submodules checked out):

    make -C test board

That target writes `ci/src/main.cpp` from `examples/PTPNode/PTPNode.ino`, prepending the ISR
prototypes a `.cpp` build needs and a real `.ino` gets from the Arduino preprocessor, and then
runs `pio run -d ci`. It is the only copy of that recipe — `scripts/gate.sh --board` runs the same
target, so adding or renaming an ISR in the example means editing `test/Makefile` and nothing else.
`make -C test clean` removes `ci/src/main.cpp` and `ci/.pio` along with the host binary.

`scripts/gate.sh` is the gate: host tests with `-Werror`, and the board build only with `--board`,
since PlatformIO downloads a toolchain on first use. The monorepo's `scripts/gate.sh` calls it and
`.githooks/pre-push` runs that. The GitHub Actions workflow this replaced was deleted: Actions only
reads workflows from the root of a repository, and this is a package inside one, so it never ran
here and the badge it fed pointed at an archived repository.

`libraries/QNEthernet` and `libraries/Time` are required for the board build, not for the host
tests, which stub them out. They were submodules of this repository; inside the monorepo they are
plain directories and nothing has to be initialised. `libraries/QNEthernet` is the fork
`JaumeAP/QNEthernet` branch `multicast-ttl`. There was a `packages/QNEthernet` in this monorepo
too, byte-identical and consumed by nobody; it is gone, and this is the only copy.

## Architecture

**`PTPBase`** (`src/ptp/ptp-base.h`/`.cpp`, ~1700 lines) holds essentially all protocol logic:
message parsing/building, the best master clock algorithm (BMCA) and port state machine, the T1–T6
timestamp bookkeeping for both the two-way (`Delay_Req`/`Delay_Resp`) and peer-delay
(`Pdelay_Req`/`Pdelay_Resp`) exchanges, and the minimum-of-*N* delay filter. It is
transport-agnostic: it calls three protected
virtuals (`initSockets`, `closeSockets`, `updateSockets`, plus `sendPTPMessage`) that the concrete
transport implements.

**`src/ptp/ptp-bmca.h`** holds the 1588 §9.3 dataset comparison and the `MasterDataset` it
compares, split out of `ptp-base.h` and included back into it. It is consumed off the board as
well: the AES67 macOS driver's platform-free core includes this exact file rather than keeping the
copy of the comparison it used to have. Nothing of Arduino, QNEthernet or the Teensy may go into
it — that core's `scripts/check-platform-free.sh` follows the include and fails if it does.

**`t41ptp::servoUpdate()`** (`src/ptp/ptp-servo.h`/`.cpp`) is the PI(+feedforward) servo's
decision, apart from the hardware that carries it out: free functions over plain numbers, no
Arduino and no QNEthernet, taking a `ServoTuning` and a `ServoState` and returning what should
happen to the clock. `PTPBase::updateController()` calls it and does it — `offsetTimer()` for a
coarse step, `adjustFrequency()` for a rate change, which is also where the correction is clamped.
The modes, thresholds, accumulator bounds and lock counting are all in the servo, and
`test/test_servo.cpp` exercises them without a board.

**`l3PTP`** (`src/ptp/l3ptp.*`) and **`l2PTP`** (`src/ptp/l2ptp.*`) subclass `PTPBase` and supply
those virtuals — `l3PTP` opens four `EthernetUDP` sockets (event/general × default/peer-delay
multicast group, ports 319/320) via QNEthernet; `l2PTP` reads/writes raw EtherType `0x88F7` frames
into a member buffer sized for `MAX_ETHERNET_FRAME_LEN`. Everything else (parsing, state,
servoing) is shared and untouched by which transport is in use. `src/t41-ptp.h` is just the public
umbrella header pulling in both.

A sketch owns one `l2PTP`/`l3PTP` instance, calls `begin()`/`update()`/`end()`, and drives
`syncMessage()`/`announceMessage()` from timers at the rate it configured via
`setLogSyncInterval()`/`setLogAnnounceInterval()` — the library does not schedule its own sends,
it only answers what arrives and emits what the sketch asks it to. `ppsInterruptTriggered()` feeds
an external PPS reference in from an ISR. See README.md's "Using the library" section for the full
API surface (state getters, Announce dataset setters, servo gains, rate/identity setters).

This library is IEEE 1588 and nothing else. Anything that carries or describes a media stream --
the RTP paths, SDP, SAP, the discovery around them, an IS-04 registration -- belongs to the sketch
that owns the audio and not here; `JaumeAP/aes67-master-box` holds those, and its `docs/continuity-notes.md`
sets the rule out. The line is not "does it use the network", since this is full of sockets: it is
"does IEEE 1588 define it".

Config constants (message length guards, timeouts, safety bounds like `MAX_FREQ_ADJUST_NSPS`) live
at the top of `ptp-base.h`, each with a comment explaining the bug it was added to close — read
those before changing bounds or timeouts.

Serial output goes through `T41PTP_LOGGING_LEVEL`, a compile-time flag defaulting to 0 in
`ptp-base.h`: 0 silent, 1 the normal messages, 2 and above the detail of every message. A level
decided at run time would keep every switched-off `Serial.printf` in the binary.

## Tests

`test/` builds `src/ptp/*.cpp` unmodified against local stubs for `Arduino.h`, `QNEthernet.h` and
`TimeLib.h` (`test/stubs/`), so the same production code runs on the host. `test/ptp_messages.*`
are builders for raw PTPv2 message bytes used to drive the parser from tests.
`test/test_ptp_base.cpp` covers message parsers/length guards, the end-to-end and peer-delay
exchanges, the servo through `PTPBase`, master selection and receipt timeouts;
`test/test_servo.cpp` covers the servo's decision on its own; `test/test_transport.cpp` covers the
transports. Over 800 assertions total.

When you fix a bug in the library, the convention in this codebase is to also add a regression
test and a comment at the fix site explaining the defect in concrete terms — not just what
changed, but what it let through and why that mattered. The comments in `ptp-base.h`/`.cpp` are
the examples to follow, and they are also the only record: the history is two commits, the
upstream snapshot and everything this fork changed on top of it, so nothing about *why* a line
looks the way it does can be recovered from `git log`. It has to be at the line.
