# CMakeLists.txt
# Monorepo root
#
# The packages under packages/ came from separate repositories and each keeps
# its own build files. This file only makes the macOS driver reachable from
# the repository root, so `cmake -S . -B build` and `ctest --test-dir build`
# still work the way they did before the move. t41-ptp is cross-compiled for a
# Teensy with the Arduino toolchain and is deliberately not wired in here;
# packages/aes67-core is pulled in by the driver itself.
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
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
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
