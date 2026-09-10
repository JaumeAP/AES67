# CMakeLists.txt
# Monorepo root
#
# The packages under packages/ came from separate repositories and each keeps
# its own build files. This file makes them reachable from the repository
# root, so `cmake -S . -B build` and `ctest --test-dir build` cover the tree
# rather than one package of it. The driver pulls in aes67-core, aes67-profiles
# and aes67-ravenna itself; the two below are the ones nothing else reaches.
#
# t41-ptp is cross-compiled for a Teensy with the Arduino toolchain and is
# deliberately not wired in here -- it has scripts/gate.sh instead.
#
# Every package verifies itself: scripts/gate.sh runs each package's own gate,
# and .githooks/pre-push runs that.
#
# The grandmaster firmware was a package here too. It is standalone again, in
# JaumeAP/AES67-master-box-, and consumes t41-ptp as a submodule of its own.

cmake_minimum_required(VERSION 3.20)

# Same reason as in the driver's own CMakeLists.txt: these only take effect if
# they are set before the first project() call of the whole tree, which is now
# this one.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    # Same number as the driver package and the Manager app: 13.0.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures")
endif()

project(AES67Monorepo LANGUAGES NONE)

enable_testing()

# One doctest for the whole tree. It used to sit inside the driver package,
# which left the core with an external/doctest of its own that the move never
# populated: its nineteen suites could not be built by anybody, here or
# standalone. A single copy at the root is what both packages now include, and
# the target is defined once, here, because both used to define it under the
# same name.
set(AES67_DOCTEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/doctest/doctest"
    CACHE PATH "Directory holding doctest.h")
if(NOT EXISTS "${AES67_DOCTEST_DIR}/doctest.h")
    message(WARNING "${AES67_DOCTEST_DIR} is empty - run: git submodule update --init --recursive")
endif()
if(NOT TARGET doctest_headers)
    add_library(doctest_headers INTERFACE)
    target_include_directories(doctest_headers INTERFACE ${AES67_DOCTEST_DIR})
endif()

add_subdirectory(packages/aes67-macos-driver)

# The app embeds what the driver's build produced, so it is told where that
# is: this build's binary directory for the driver, not the driver package's
# own build/, which a root build never fills.
set(AES67_MACOS_DRIVER_BUILD_DIR "${CMAKE_BINARY_DIR}/packages/aes67-macos-driver"
    CACHE PATH "Directory holding the built driver bundle and PTP daemon" FORCE)
add_subdirectory(packages/aes67-macos-manager)

# After the driver, because it defines aes67_core and aes67_profiles and both
# of these guard on the targets already existing. On a Mac this gives the PTP
# wire library and its three suites and no daemon, which is what that package
# configures to when it is not Linux; the kernel module's package configures to
# nothing at all off Linux and says so.
add_subdirectory(packages/aes67-linux-ptpd)
add_subdirectory(packages/ravenna-alsa-lkm)
