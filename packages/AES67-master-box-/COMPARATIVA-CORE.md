# Moving the profiles, the web server and the daemon into aes67-core

A proposal, not a change. Nothing here has been implemented and nothing
has been measured. Written on 2026-09-03 by reading the three
repositories as they stand: `JaumeAP/AES67-master-box-` at `5d7a513`,
`JaumeAP/aes67-core` at `36c8b47`, `JaumeAP/aes67_macos_driver` at its
current `main`.

The ask is to put everything that is profile definition, configuration
web server and daemon into `aes67-core`, carried as a submodule, so that
one copy serves every implementation.

## Decided: the alternative was taken

On 2026-09-03, against the recommendation in this document,
`aes67-core` folds back into `aes67_macos_driver` and this box stays
standalone: it keeps its own profile table and its own web server, as it
has them today. The plan for carrying that out is `MIGRATION-CORE.md`.

**Everything between here and "The alternative: fold core back into the
driver" is what was NOT done.** It is kept for the reasoning, not as a
proposal. The cost the decision accepts is stated in that section's
"What kills it": merging ends the ability to build and test the core
layer off a Mac.

What follows from it for this repository: nothing changes. The four
profiles stay in `src/profiles.{h,cpp}` with their EEPROM persistence,
the server stays in `src/webconfig.{h,cpp}`, and `lib/t41-ptp` remains
the only submodule. The duplication described below stays too -- the two
priorities and the two intervals go on being said twice, here as log2
exponents and in the driver as milliseconds, with nothing making them
agree.

There is a prior decision to be aware of: `aes67_macos_driver`'s
`HANDOFF.md` records that the box does NOT consume `aes67-core`, and
that this was weighed on 2026-09-03 and left that way. The reasoning is
not written down there, only the outcome. What follows would have
reversed it; the decision taken kept it, and went further -- there will
be no `aes67-core` left to consume.

## The argument for doing it

The duplication is already real, not hypothetical. `aes67-core`'s
`NetworkEngine/PTP/PTPMasterSettings.h` carries `priority1` 128,
`priority2` 128, `clockClass` 248, `clockAccuracy` 0xFE,
`syncIntervalMs` 125, `announceIntervalMs` 1000, `delayReqIntervalMs`
1000, a delay mechanism and a DSCP. This repository's `src/profiles.h`
carries `domainNumber`, `logSyncInterval`, `logAnnounceInterval`,
`priority1`, `priority2` and `lockThresholdNs`.

Four of those are the same numbers said twice, in different units: 125 ms
against log -3, 1000 ms against log 0, and both priorities. The units
matter, because the exponent is what goes on the wire and the
milliseconds are a rendering of it. Two places that have to agree, and
nothing that makes them.

## The shape all three share

Each of the three is the same split the clock servo has just been
through in `t41-ptp`: the decision goes into a library over plain
values, and the thing that touches the world is injected. See "The clock
servo, now testable" in `HANDOFF.md`.

What differs is how much of each is decision and how much is world.

### 1. The profiles: move whole

`PtpProfile` is a POD of `const char*` and integers, the table is four
entries, and `profileSyncIntervalUs()` and `profileSyncTicksPerSecond()`
are arithmetic. No Arduino, no EEPROM, no network, no allocation. It
moves as it is, and the log2 to milliseconds conversion goes with it so
that the driver stops carrying its own.

Host tests come free, the same way the servo's did.

What does NOT move: `profileLoadSelection()` and `profileSaveSelection()`
in `src/profiles.cpp`. They are EEPROM — address 0, magic `0x41455337`,
a version byte, a checksum and a read-back — and the driver's equivalent
is a file. Same intent, no shared code; what can be shared is the
interface, with two implementations.

### 2. The web server: split at the byte sink

Into `aes67-core`: the routing (`?p=` and `?r=`, `webconfig.cpp:211`
onwards), the bounds check on the index, and the building of the page,
written into an abstract sink of bytes rather than into a client.

Left outside: the transport. Here it is a QNEthernet `EthernetClient`;
on a host it is a socket. `src/webconfig.cpp` would come down to
accepting the client, handing it over as a sink, and the two callbacks
it already registers from `main.cpp:308`.

Cost to weigh: the server as it stands costs 13296 B of flash and
2240 B of RAM1 (124304/48992 to 137600/51232). Page building written
against an abstract sink will not be smaller, and the Teensy's RAM1 is
already carrying the PTP stack.

### 3. The daemon: split at the socket, and the line is half drawn

`Daemon/aes67ptpd.cpp` is 173 lines, and almost none of it is the
daemon: it is argv parsing, `csignal`, the Unix socket path
(`kPTPServiceSocketPath`) and a `std::thread`. The engine is
`PTPService` and `PTPSlave`, which today live in the driver's
`NetworkEngine`, not in core.

Into `aes67-core`: that engine, with its transport injected the way
`PTPClockSource.h` is already an interface there.

Left outside: the process shell. Argv, signals, LaunchDaemon lifecycle.
This box has no such shell at all — its PTP engine is `t41-ptp` — so
what the box and the daemon would come to share is the state model and
the status protocol, not the process.

Worth keeping in view from that file's own comment: running it as a
LaunchDaemon is about lifecycle, not about privilege — on macOS 26.6.2
an unprivileged process binds UDP 319 and 320 (measured there, not
here).

## What this costs, stated plainly

**The socket code does not move; it gets rewritten.** `aes67-core`
declares itself platform-free — "C++17, no dependencies, no platform
headers" — and enforces it with `scripts/check-platform-free.sh`, whose
banned list is `CoreAudio`, `CoreFoundation`, `AudioToolbox`,
`Accelerate`, `mach`, `sys/socket`, `netinet`, `arpa` and `ifaddrs`.
That list exists precisely to stop items 2 and 3 being done the easy
way. Honouring it means `PTPService` and `PTPSlave` keep their protocol
and lose their transport, which is a rewrite on both sides, not a
relocation.

**A second top-level submodule.** The box would carry `lib/t41-ptp`
(which itself carries QNEthernet) and `aes67-core`. Three levels deep on
one path, two roots to keep in step.

## Two things to fix in core first

Neither depends on this proposal going ahead; both are true today.

**The platform-free contract is not being kept.**
`NetworkEngine/PTP/PTPMasterSettings.cpp` includes `<fstream>`, calls
`std::getenv("AES67_PTP_MASTER_CONFIG_PATH")` and `std::getenv("HOME")`,
calls `stat()`, and hardcodes `/Library/Application Support/AES67Driver/`
as a search path. That is macOS inside the platform-free library. It
passes the gate because the gate greps for a fixed list of headers and
none of those are on it. And it is exactly the class the box would be
consuming.

The fix is to take the persistence out to the driver, leaving the
settings struct behind, and to widen the gate so `<fstream>`, `getenv`
and absolute paths do not pass either.

**Core is not consumable by PlatformIO.** It is a CMake library
(`cmake_minimum_required(VERSION 3.15)`, an explicit `add_library`
source list) with sources spread across `Driver/`, `NetworkEngine/` and
`Shared/`. PlatformIO builds `src/` under a library root by convention.
It needs a `library.json` with a `srcFilter`, or a freestanding subset
directory that the firmware pulls and the host build ignores.

## Order of work

1. Fix the two things above in `aes67-core`. Independent of everything
   else here, and worth doing whether or not the rest happens.
2. Move the profile table and the conversions. Small, self-contained,
   testable on a host, and it is where the duplication actually is.
3. Split the web server at the byte sink.
4. Split the PTP engine at the socket.

Steps 3 and 4 are each larger than everything this repository has done
so far. Step 2 is not, and it collects most of the benefit.

## The alternative: fold core back into the driver

**This is the one that was chosen.** See the decision at the top.

Everything above assumes `aes67-core` gains a second consumer. If it is
not going to, the honest move is the opposite one: drop the submodule
and put the code back inside `aes67_macos_driver`.

The case for it is stronger than it looks.

**Core has exactly one consumer today.** `aes67_macos_driver` does
`add_subdirectory(external/aes67-core EXCLUDE_FROM_ALL)`
(`CMakeLists.txt:212`) and links it from `aes67_net`
(`target_link_libraries(aes67_net PUBLIC aes67_core)`, line 247), from
the plugin, and from the daemon (`aes67ptpd PRIVATE aes67_core
aes67_net`, line 374). This box does not consume it. Whether
`JaumeAP/DTS-Player` does has not been checked here — it is not an AES67
repository and was outside this session's scope.

**And core is not self-contained anyway.** It declares four
`NetworkUtils` symbols it does not implement, and the only thing that
resolves them is the driver's own `aes67_net`. A library with one
consumer, whose undefined symbols only that consumer satisfies, is
already a subdirectory of it in everything but layout.

The README's argument — that an ESP32-P4 firmware, a Linux daemon and a
macOS driver are three peers and none should have to take another's
platform along — is not used here as a reason to keep the split. It
describes an intention, and right now none of those other two exist.

### What kills it

`aes67_macos_driver` cannot be configured off a Mac: its `project()`
declares OBJCXX, so `cmake` fails on Linux before anything is built.
`aes67-core` standing alone can be built and tested anywhere, in under a
second: `cmake -S . -B build && cmake --build build && ctest`.

Fold core in and that goes. The measure of what it would cost is on
record: the PTP work of 2026-09-03 had to be verified by compiling
individual translation units by hand — `g++ -fsyntax-only -I.
-Iexternal/aes67-core`, with a local shim for `net/if_dl.h` and a stub
for `NetworkUtils::setQoSTrafficClass`, neither of them committed.
Merged, that becomes the only way to check anything, for every change,
instead of the exception it is now.

That is the argument to weigh, and it is a measured one rather than an
aspirational one. The platform-free gate itself is not part of the
trade: it does not need a repository boundary, and would work as well as
a directory rule inside the driver.

## Not verified

No code has been written for any of this. The flash and RAM figures
quoted are the measured cost of the server as it stands today, not an
estimate of what it would cost after the split. Nothing in this
repository has ever run on hardware, so none of the three moves has been
tested in the only way that would settle it.
