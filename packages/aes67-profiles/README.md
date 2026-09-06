# aes67-profiles

The tables every implementation in this repository has to agree on.

A profile is not code. It is a set of numbers an ecosystem agreed on -- what
sample rates a device accepts, what packet times, which multicast range, how
often a clock announces itself -- and there is nothing in it to run. Kept
inside one implementation, it is a table the others copy; copies drift, and a
receiver and a sender disagreeing about what a profile says is a fault nobody
can see from either side.

So the tables live here, and the implementations read them:

- `Profiles/CompatibilityProfile.h/.cpp` — what each flavour of AoIP gear
  *restricts*. Every profile is AES67 underneath; what differs is what each one
  rules out. ST 2110-30 Level A permits only 48 kHz where AES67 permits three
  rates; Dante wants multicast inside its own range; CP850 receives and never
  transmits. A profile is a filter, never a capability grant: selecting one can
  only narrow what is accepted.
- `Profiles/DolbyModelCatalog.h` — the fixed per-model facts: channel counts,
  direction, which unit is which.
- `Profiles/PtpProfiles.h` — the five numbers each PTP ecosystem fixes: domain,
  majorSdoId, and the Sync, Announce and Delay_Req intervals. The IEEE
  1588-2008 default profile, the media profile AES67 and RAVENNA gear runs, and
  802.1AS.
- `Profiles/StreamDescription.h` — the six values a compatibility profile
  validates, and the reason this package needs no notion of SDP.
- `Profiles/ProfileLog.h` — where the log lines go, which is nowhere unless the
  consumer says otherwise.

## What it depends on

Nothing. Not a platform, not an operating system, not another package in this
repository. `scripts/gate.sh` checks both halves of that: no Apple framework,
no socket header, no Arduino, and no include reaching into a neighbour.

That is the whole point. `Profiles/PtpProfiles.h` is freestanding -- no
`std::string`, no allocation, and its lookup works in a constant expression --
so the Teensy firmware includes it as readily as the macOS driver does, and the
two agree by construction rather than by two tables that match today.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Or `scripts/gate.sh`, which does that and the self-containment checks.

## Consuming it

CMake, as the core does:

```cmake
add_subdirectory(../aes67-profiles aes67-profiles)
target_link_libraries(mine PRIVATE aes67_profiles)
```

PlatformIO or Arduino, as the Teensy firmware does -- an include path and
nothing to link, since the part a firmware wants is header-only:

```ini
build_flags = -I${PROJECT_DIR}/../../aes67-profiles
```

```cpp
#include "Profiles/PtpProfiles.h"

const AES67::PtpProfile* profile = AES67::ptpProfileByName("aes67");
ptp.applyProfile({profile->settings.domainNumber,
                  profile->settings.majorSdoId,
                  profile->settings.logSyncInterval,
                  profile->settings.logAnnounceInterval,
                  profile->settings.logMinDelayReqInterval});
```

A consumer that wants the compatibility profiles' `validate()` fills in a
`StreamDescription` from whatever it holds; the macOS core does it from an
`SDPSession` in one function, `NetworkEngine/ProfileAdapter.h`.
