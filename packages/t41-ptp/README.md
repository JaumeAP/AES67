![tests](https://github.com/JaumeAP/t41-ptp/actions/workflows/tests.yml/badge.svg)

# t41-ptp

This [repository](https://github.com/IMS-AS-LUH/t41-ptp) provides the source code for the research paper "Sub-Microsecond Time Synchronization for Network-Connected Microcontrollers" ([Final published article](https://doi.org/10.1109/ICCE59016.2024.10444401), [Open Access: accepted version](https://doi.org/10.15488/16561)). This Arduino compatible library has been created to provide high precision time synchronization for the Teensy 4.1 microcontroller platform.

The library was created by the Architectures and Systems Group of the Institute of Microelectronic Systems ([IMS/AS](https://www.ims.uni-hannover.de/de/institut/architekturen-und-systeme/)) at the [Leibniz University](https://www.uni-hannover.de) in Germany. This fork carries an audit of the implementation and the fixes that came out of it; the section on what changed is at the end.

![Screenshot](/doc/0.png?raw=true)

## Getting started

### Prerequisites

The parts this runs on, and where they can be ordered, are in
[doc/hardware.md](doc/hardware.md).

The parts this runs on, and where they can be ordered, are in
[doc/hardware.md](doc/hardware.md).

- Teensy 4.1 with Ethernet adapter
- Working Teensy Arduino environment

The two libraries this one needs are submodules:

    git clone --recurse-submodules https://github.com/JaumeAP/t41-ptp

- `libraries/QNEthernet` — QNEthernet with IEEE 1588 support, from [JaumeAP/QNEthernet](https://github.com/JaumeAP/QNEthernet/tree/multicast-ttl), itself [HedgeHawk's `ieee1588-2-fix`](https://github.com/HedgeHawk/QNEthernet/tree/ieee1588-2-fix) plus three commits. See below for why this is a fork and not upstream.
- `libraries/Time` — `TimeLib.h`, used for the log output. This one is upstream's own, [PaulStoffregen/Time](https://github.com/PaulStoffregen/Time).

#### Why QNEthernet comes from a fork

Not for convenience. Upstream cannot supply this code, in three separate ways:

- **`ssilverman/QNEthernet`'s `master` has no IEEE 1588 at all** — no `EthernetIEEE1588Class`, no `adjustFreq()`, no frame timestamping. Everything this library stands on has only ever lived on branches that were never merged into it.
- **The branch it did live on has been rewritten since.** The 2022 commit this fork is built on is no longer served by upstream at all: asking for it answers `couldn't find remote ref`. That history survives only in the fork.
- **Six commits between that base and this fork's tip are load-bearing**, by Jens Schleusner (IMS Hannover, the authors of the paper): the timer-event functions the reference input needs, `EthernetIEEE1588Class::offsetTimer()` — the servo's coarse step, called at `src/ptp/ptp-base.cpp:719` — and `adjustFreq()` taking a `double` rather than an `int`, which is what lets the fine term of the servo command less than a nanosecond per second. Today's upstream `ieee1588-2` branch, 2100 commits further on, has neither of the last two.

The three commits on top of that are this fork's own: `EthernetUDP::setMulticastTTL()`, called six times from `src/ptp/l3ptp.cpp` — upstream's nearest equivalent, `setOutgoingTTL()`, has a different signature and is not multicast-specific; `~EthernetUDP` made virtual; and `math.h` included in `lwip_t41.c`, without which the library does not build on current GCC.

Moving to upstream would therefore not be a change of URL but a port to a four-years-newer QNEthernet, with the servo's coarse step to reimplement and its frequency resolution to give up.

### Installation

Clone the library folder into your Arduino libraries path and restart the IDE, or point PlatformIO at it as this repository's own `ci/platformio.ini` does.

### Examples

`examples/PTPNode` is a Teensy 4.1 at either end, because it is configured as both: it announces itself, and it follows a better clock instead of going quiet if one turns up on the segment. Which of the two it is at any moment is the best master clock algorithm's answer, not a compile-time choice. It takes a reference input on pin 15, emits a PPS on pin 24, and prints a line of state every second at 2000000 baud:

    [ptp] state=slave pps=none sync=ok master=chosen offset=-198ns delay=10342ns lock=7 tx_fail=0 bind_fail=0

`examples/PTPNode/README.md` has the rest: the pins and what each is wired to, the address to change per board, what the sketch announces as its `clockClass` while the reference comes and goes, how to read the state line, and the two places the Sync rate is written.

### What it costs on the board

The example built for the Teensy 4.1, which has 512 KB of RAM1, 512 KB of RAM2 and 8 MB of flash:

| | Used | Left |
|---|---|---|
| RAM1 | 180352 B (34%) — 49280 variables, 123448 code, 7624 padding | 343936 B for the stack and locals |
| RAM2 | 51808 B (10%) — `DMAMEM` variables | 472480 B for `malloc`/`new` |
| Flash | 151548 B (1.9%) — 126032 code, 16328 data, 9188 headers | 7974916 B |

What fills RAM1 is the code copied there to run from, not the data. The RAM2 figure is essentially QNEthernet's descriptors and buffers, which live there to be reached by DMA. These are the numbers `pio run -d ci` prints; a sketch of your own moves them.

## Using the library

    #include <t41-ptp.h>

    l3PTP ptp(master, slave, p2p);   // UDP multicast, ports 319 and 320
    l2PTP ptp(master, slave, p2p);   // raw 802.3 frames, EtherType 0x88F7

    ptp.begin();     // opens the sockets; zeroes the clock the first time only
                     // (called again on a port already up, it re-initialises it:
                     //  sockets closed and reopened, port state dropped)
    ptp.update();    // call from loop(): receives, answers, disciplines
    ptp.end();       // closes them again; begin() afterwards starts fresh

A master sends `Sync` and `Announce` when the sketch calls `syncMessage()` and `announceMessage()`, sends the `Follow_Up` from the next `update()` once the hardware has posted the Sync's departure time, and answers `Delay_Req` — and, in peer-delay mode, `Pdelay_Req`. A slave follows the master it chooses, requests the path delay at the rate that master asks for, and disciplines the hardware clock.

### Reading the state

| Call | Says |
| --- | --- |
| `getOffset()` | last measured offset from the master, in nanoseconds |
| `getDelay()` | current path delay estimate |
| `getLockCount()` | consecutive corrections inside ±100 ns |
| `isSyncReceiptValid()` | whether a Sync has arrived recently enough for the lock to mean anything |
| `hasSelectedMaster()`, `getSelectedMaster()` | which master the dataset comparison chose |
| `getPortState()` | `Initializing`, `Listening`, `Master`, `Passive` or `Slave` |
| `getTxFailureCount()`, `getBindFailureCount()` | sends that failed, and multicast groups that could not be joined |

### The Announce dataset

`setClockClass()`, `setClockAccuracy()`, `setOffsetScaledLogVariance()`, `setPriority1()`, `setPriority2()`, `setTimeSource()`, `setCurrentUtcOffset()`, `setUtcOffsetValid()`, `setLeap59()`, `setLeap61()`, `setTimeTraceable()`, `setFrequencyTraceable()`, `setStepsRemoved()`.

The defaults announce a free-running clock disciplined by nothing. A grandmaster with a real reference should say so: `setClockClass(6)` and `setTimeSource(0x20)` for GPS, and `setTimeTraceable(true)` once it is locked.

### Rates and identity

`l3PTP::setDscp()` marks the PTP datagrams: the value is the DSCP itself, 0 to 63, and 0 — unmarked, what lwIP sends — is the default. The AES67 and RAVENNA guides mark PTP so it does not queue behind the audio it is timing; which value depends on the network, so the library takes it rather than choosing (EF is 46, and CS7, what Dante marks PTP with, is 56). It needs `EthernetUDP::setOutgoingDiffServ()`, which is in the QNEthernet fork this library carries.

`applyProfile()` sets the whole combination a given ecosystem expects in one call: `Profile::Default1588` (Sync 1 s, Announce 2 s, Delay_Req 1 s), `Profile::AES67Media` (Sync and Delay_Req eight per second, Announce 1 s, which is what AES67 and RAVENNA gear runs) and `Profile::GPTP` (Sync eight per second, Announce 1 s, Pdelay_Req 1 s, majorSdoId 1). It sets the numbers only: the delay mechanism is chosen when the object is built, and the transport by which class is used, `l3PTP` for AES67 and `l2PTP` for 802.1AS. The individual setters remain, for anything a profile does not cover.

`setLogSyncInterval()`, `setLogAnnounceInterval()` and `setLogMinDelayReqInterval()` set what the header advertises — the sketch still has to send at the matching rate — and each is clamped to the −7 to 7 a `logMessageInterval` may carry. Peer to peer, `setLogMinDelayReqInterval()` also paces this port's own `Pdelay_Req`. `setDomainNumber()` and `setMajorSdoId()` set the domain and profile; messages carrying anything else are ignored. `setMasterIdentity()` pins the clock a slave will follow, which is the only defence against an Announce that simply claims to be better.

### The servo

The servo's decision is `t41ptp::servoUpdate()` in `src/ptp/ptp-servo.h`: the drift, the choice between refusing the measurement, correcting frequency, stepping the clock and closing the PI loop, the accumulator bounds and the lock counting, as free functions over plain numbers. `PTPBase::updateController()` calls it and applies what comes back. Nothing in it needs a board, which is what lets `test/test_servo.cpp` ask it what it would do with a given pair of timestamps.

`setKp()`, `setKi()` and `setKf()` are the proportional, integral and frequency-mode gains. `setMaxDriftNsps()`, `setFreqModeThresholdNsps()`, `setCoarseModeThresholdNs()` and `setLockThresholdNs()` are the thresholds it decides by: when to refuse the measurement, when to correct frequency alone, when to step the clock rather than steer it, and what counts as locked. They were literals inside the controller, so a board whose oscillator, network or idea of "locked" was not the one those numbers were chosen against had no way to say so. `setDelayFilterLength()` sets how many delay measurements the minimum filter keeps, from 1 to 8. `setTimestampOffset()`, `setPeerOffsetCorrection()` and `setPpsOffset()` are the compensations applied to the network, peer-delay and PPS paths. Every one of them reads back through the matching getter.

`l3PTP::setMulticastTTL()` sets the scope of the outgoing multicast; it is 1 by default.

### Logging

`T41PTP_LOGGING_LEVEL` is how much the library says on the serial port: 0 silent, which is the default, 1 the normal messages, 2 and above the detail of every message. It is a compile-time flag — on PlatformIO, `build_flags = -DT41PTP_LOGGING_LEVEL=1` — because a level decided at run time keeps every switched-off `Serial.printf` inside the binary, some 1.5 KB of flash and 1 KB of RAM for nothing.

## Tests

`test/` builds the library on the development machine against stubs for Arduino, QNEthernet and TimeLib, and runs it:

    make -C test

Over eight hundred assertions covering the message parsers and their length guards, the end-to-end and peer-delay exchanges, the servo — both through `PTPBase` and, in `test/test_servo.cpp`, on its own — the master selection, the receipt timeouts and both transports. `ci/` builds the example for the board with PlatformIO, which the same makefile drives:

    make -C test board

It needs PlatformIO on the path (`PIO=` overrides the command) and the submodules checked out. Nothing it produces is versioned; `make -C test clean` removes it along with the host binary. Both builds run on every push.

## What this fork changed

The implementation was audited five times and the findings fixed. The changes that alter behaviour rather than merely fixing a defect:

- The clock is corrected on every Sync pair, against the path delay measured most recently, and the `Delay_Req` follows the interval the master announces instead of going out once per Sync.
- Peer delay runs between neighbours and not off the synchronisation tree: a port measuring the delay peer to peer sends its `Pdelay_Req` on its own interval, in whatever state it is in and whether or not any Sync is arriving, and `setLogMinDelayReqInterval()` is what sets that interval — there is no master to name one, and the `Pdelay_Resp` carries `0x7f` where the end-to-end answer carries a rate. It used to be armed by a matched Sync pair, like the end-to-end request, so a peer-delay master never measured its link at all and a slave did not until a master turned up.
- The path delay is the minimum of the last eight measurements rather than the last one. `setDelayFilterLength(1)` restores the old behaviour.
- A slave parses `Announce`, chooses among the masters it hears with the 1588 dataset comparison, and follows only that one. Before the first Announce arrives, any Sync is followed, so a master that sends none still works.
- A port configured as master runs the same comparison against its own dataset and falls silent while a better master is on the segment — `getPortState()` reports `Passive` — coming back when that master stops announcing. This is what 1588 asks for, and it means two masters left at the default `priority1` and `clockClass` will settle on one of them by clock identity. A master with a real reference should say so through `setClockClass()`; `setBmcaEnabled(false)` keeps a configured master sending whatever else it hears; on a port configured as master *and* slave it leaves the roles to decide instead, so the port follows whatever master has been chosen, however poor, and announces only while none has been.
- A master that goes quiet for three of its own intervals takes the lock with it; the clock free-runs at the rate it last learned.
- Outgoing multicast has a TTL of 1, and peer-delay messages go to 224.0.0.107 rather than the default group.
- Over layer 2, only peer-delay messages go to `01-80-C2-00-00-0E`. Everything else goes to `01-1B-19-00-00-00`, which is what Annex F of 1588 asks for and what a bridge will forward — the peer-delay address is inside the range 802.1D reserves for link-local traffic. A port set to `majorSdoId` 1, which is 802.1AS, keeps every message on `01-80-C2-00-00-0E`, because gPTP is hop by hop.
- `reset()` no longer zeroes the hardware clock: that happens once, on the first `begin()`.
- `getBindFailureCount()` counts since boot, like `getTxFailureCount()`: `reset()` used to clear the bind failures and leave the transmit ones alone.
- `clockIdentity` is the EUI-48 to EUI-64 mapping the standard defines, not the MAC with `FF FF` in front, so the identity on the wire is different from earlier versions.

## Citation

If you use this PTP library (or parts of it) in scientific work, please cite the ICCE 2024 paper ([Final published article](https://doi.org/10.1109/ICCE59016.2024.10444401), [Open Access: accepted version](https://doi.org/10.15488/16561)).

    @inproceedings{schleusner2024sub,
     title={Sub-Microsecond Time Synchronization for Network-Connected Microcontrollers},
     author={Schleusner, Jens and Fahnemann, Christian and Pfleiderer, Richard and Blume, Holger},
     booktitle={2024 IEEE International Conference on Consumer Electronics (ICCE)},
     month=jan,
     year=2024,
     keywords={Microcontrollers;Hardware;Synchronization;Phase locked loops;Standards;Global Positioning System;Clocks;PTP;Precision Time Protocol;Microcontroller;Embedded System;TSN;Time Sensitive Networking},
     doi={10.1109/ICCE59016.2024.10444401}
    }

## Test setup

A GPS module ([Adafruit Ultimate GPS](https://www.adafruit.com/product/746)) is connected to the PTP master ([Teensy 4.1](https://www.pjrc.com/store/teensy41.html)) via a PPS signal. The PTP slave is synchronized to the master over a [network connection](https://www.pjrc.com/store/ethernet_kit.html).

![Setup](/doc/1.jpg?raw=true)
