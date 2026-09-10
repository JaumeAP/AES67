# aes67-macos-manager

The SwiftUI manager app for the AES67 macOS driver: what a person runs.

It is one half of a delivery whose other half is `packages/aes67-macos-driver`.
The driver is an AudioServerPlugIn that `coreaudiod` loads and a PTP daemon
that `launchd` runs; neither can put itself on the system. This app carries
both as resources and installs them, through one administrator prompt, when the
Install button is pressed. What a user gets is a disk image with one app in it.

## Why it is its own package

Two programs, two toolchains, two ways to fail. The driver is Objective-C++
under CMake and needs libASPL; the app is Swift under a raw `swiftc` call in
`build.sh` and needs a Swift toolchain. A machine with the Command Line Tools
and no Swift builds the driver and not this; a change to a SwiftUI view has no
business re-running the driver's forty-seven suites. Inside one package that
was a `BUILD_MANAGER_APP` flag; as two packages it is nothing.

## What it needs from the driver

Artifacts, not sources: `AES67Driver.driver`, `aes67ptpd` and the daemon's
LaunchDaemon plist, out of the driver package's build directory.

- Standalone, `build.sh` looks in `../aes67-macos-driver/build`, which is what
  that package's gate fills. Missing artifacts are a warning, not an error:
  the app still builds and says what it could not embed.
- From the monorepo root, CMake passes its own binary directory for the driver
  (`AES67_MACOS_DRIVER_BUILD_DIR`) and makes the app wait for the driver and
  daemon targets, so a root build embeds what the root build built.

`DriverManager.cpp` is a small C++ shim over the Core Audio HAL APIs, and
`Models/DriverManager.swift` wraps it; that is how the app talks to an
installed driver. It does not include the driver's headers.

## Building

```bash
./build.sh              # the app bundle, AES67Manager.app, beside the sources
./build.sh --force      # skipping the up-to-date check
./run-tests.sh          # the host tests
./build-dmg.sh          # the disk image, into the driver's build/dmg
```

`Package.swift` exists for editor support only; the build is `swiftc`, not
SwiftPM. `#Preview` blocks live in `Views/Previews/`, which `build.sh` leaves
out on purpose: the macro they need ships with full Xcode and not with the
Command Line Tools. Keep new previews there.

## What is checked

`scripts/gate.sh` runs the host tests -- plain Swift over plain values, no
SwiftUI, no Core Audio, no driver -- and then builds the app. The tree gate
runs it after the driver's, so the bundle it produces has the driver inside.
Its functional behaviour against a live driver is not verified by anything
here: that takes an installed driver and a Mac in a known state.
