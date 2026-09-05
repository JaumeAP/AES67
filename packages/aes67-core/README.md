# aes67_core

The parts of an AES67 implementation that are not about any operating system:
SDP parsing and generation, the RTP wire header, a lock-free jitter buffer and
packet pool, the media-clock PLL, the resampling chain, channel mapping,
compatibility profiles and stream configuration. Also the pieces that describe
rather than do: the single-producer ring buffer and the real-time view over it,
the PTP time types and the clock-source interface a platform implements, and
the PTP peer, RTCP receiver and Dolby model tables.

C++17, no dependencies, no platform headers. That is checked rather than
claimed — `scripts/check-platform-free.sh` fails on an Apple framework or a
socket header anywhere in the library, following includes rather than trusting
the `.cpp` files.

## Why it exists

This code was written inside `aes67_macos_driver` and lived there. That made
the macOS driver the base every other implementation had to consume, which is
backwards: the ESP32-P4 firmware, a Linux daemon and a macOS driver are three
peers, and none of them should have to take another's platform along to reuse a
jitter buffer.

Splitting it out makes them peers. What stays in `aes67_macos_driver` is what is
genuinely macOS: the `AudioServerPlugIn`, libASPL, CoreAudio clock sources, the
Accelerate codec.

## Building and checking

```bash
git clone --recurse-submodules https://github.com/JaumeAP/aes67-core.git
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

`scripts/gate.sh` runs it, and `.githooks/pre-push` runs that — opt in per
clone with `git config core.hooksPath .githooks`, since hook configuration is
local and does not travel with a repository.

The gate has two speeds. By default it builds, tests and checks the platform
contract, which takes under a second. The static analysis costs minutes and is
left out: it finds latent defects rather than what broke between two commits.
`--analyse`, or `AES67_ANALYSE=1`, runs it. The hook asks before a push to the
default branch — the result other people pull — and skips the question when
there is no terminal to ask on, rather than hanging the push.

Three checks, and they answer different questions:

| Check | Question |
|---|---|
| `ctest --test-dir build --output-on-failure` | does it still do what it did? 19 suites, one of them (`InteropSDP`) against a working implementation rather than against the RFC |
| `scripts/check-platform-free.sh` | is it still portable? fails on an Apple framework or a socket header anywhere, following includes |
| `scripts/check-tidy.sh` | is it defective in a way nobody ran into yet? clang-tidy, defect checks only, with `.clang-tidy` deciding which of them fail rather than warn |

`check-tidy.sh` skips itself with a message when no clang-tidy is present — it is
not part of the Command Line Tools. The script says how to get one.

`external/doctest` is a submodule and only the tests need it; a consumer that
builds the library alone can pass `-DAES67_CORE_TESTS=OFF`.

## One seam you have to fill

`NetworkEngine/StreamConfig.cpp` calls four functions declared in
`NetworkEngine/NetworkUtils.h` — `isIPv4Address`, `getInterfaceIP`,
`resolveInterfaceToIP` and `getActiveInterfacesWithIPs` — and this library does
not implement them. It cannot:
the implementation opens sockets and enumerates interfaces, which is exactly
what a platform-free library has no business doing.

So the header is here and the implementation is yours. `aes67_macos_driver`
provides it in `aes67_net`; a firmware consumer provides one over lwIP. Linking
`aes67_core` without supplying those three symbols fails at the final link,
which is the right place for it to fail.

`Tests/support/NetworkUtilsStub.cpp` is a worked example: deterministic answers,
no sockets, no looking at the machine. A consumer needs the real thing, but the
shape is the same.

This is a seam, not an oversight. It was found by linking, not by reading: the
platform-header check passes on `StreamConfig.cpp` because the header it
includes is clean, and only a real link showed that the code behind it is not.

## Consuming it

As a CMake subproject, a submodule, or by listing the sources directly — the
library has no build-system opinion beyond a C++17 compiler. PlatformIO
consumers can point `lib_extra_dirs` at the checkout.

## Licence

GPL-3.0, inherited from `aes67_macos_driver`, where this code was written. That
is not a preference: 19 of the 23 translation units carry commits from a second
author, so relicensing is not a decision one person can make.

For a consumer, that means the usual GPL bargain: link it and distribute the
result, and the result is GPL-3.0 with sources. Used in-house it obliges
nothing.

## History

The commits here are the real ones — 87 of them, filtered from
`aes67_macos_driver` so that `git log` and `git blame` still answer questions
about this code. Anything older than the split lives in that repository.
