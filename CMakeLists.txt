# CMakeLists.txt
# AES67 macOS Driver - Build #4
# Cross-platform build configuration

cmake_minimum_required(VERSION 3.20)

# CMAKE_OSX_ARCHITECTURES and CMAKE_OSX_DEPLOYMENT_TARGET only take effect if
# set before the first project()/enable_language() call — that's when CMake
# runs compiler/ABI detection for this platform. Set after, as these used to
# be, they're silently ignored: every target built arm64-only (the host's
# native architecture) targeting whatever macOS the build machine runs, not
# the "12.0" this project intends. CMAKE_HOST_SYSTEM_NAME rather than APPLE:
# the APPLE variable itself isn't set until after that same first project()
# call.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures")
endif()

# Read version from VERSION.txt
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" VERSION_STRING)
string(STRIP "${VERSION_STRING}" VERSION_STRING)
string(REGEX MATCH "([0-9]+)\\.([0-9]+)\\.([0-9]+)-build\\.([0-9]+)" _ "${VERSION_STRING}")
set(VERSION_MAJOR ${CMAKE_MATCH_1})
set(VERSION_MINOR ${CMAKE_MATCH_2})
set(VERSION_PATCH ${CMAKE_MATCH_3})
set(VERSION_BUILD ${CMAKE_MATCH_4})

project(AES67Driver
    VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}
    LANGUAGES CXX OBJCXX
    DESCRIPTION "AES67/RAVENNA/Dante Audio Driver for macOS"
)

message(STATUS "AES67 Driver Build #${VERSION_BUILD}")

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sanitizer build (opt-in, verification only — NOT for the shipping driver).
# Configure a separate build dir with the sanitizers you want, then build the
# test targets and run ctest:
#
#   cmake -S . -B build-san -DAES67_SANITIZE="address,undefined"
#   cmake --build build-san --target TestSDPParser TestRTCPReceiverTable ...
#   ctest --test-dir build-san -E IntegrationAudioPath
#
# Use "thread" instead for data-race checking. macOS ASan has no leak detector
# (detect_leaks is unsupported); use the system `leaks --atExit -- <binary>`
# tool on a normal build for leaks. Applies to every C++ target; the Swift
# ManagerApp (built by its own build.sh) is unaffected.
set(AES67_SANITIZE "" CACHE STRING
    "Sanitizers to enable, e.g. address,undefined or thread (empty = none)")
if(AES67_SANITIZE)
    message(STATUS "Sanitizers enabled: ${AES67_SANITIZE}")
    add_compile_options(-fsanitize=${AES67_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${AES67_SANITIZE})
endif()

# Source-based coverage, opt-in and in its own build dir:
#   cmake -S . -B build-cov -DAES67_COVERAGE=ON && cmake --build build-cov
#   scripts/coverage.sh
# Kept out of the default build because instrumented binaries are slower and
# emit a .profraw per run, which would pollute a normal build tree.
option(AES67_COVERAGE "Instrument for llvm-cov source-based coverage" OFF)
if(AES67_COVERAGE)
    message(STATUS "Coverage instrumentation enabled")
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping -g -O0)
    add_link_options(-fprofile-instr-generate -fcoverage-mapping)
endif()

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# macOS specific settings (architectures/deployment target are set above,
# before project() — see the comment there)
if(APPLE)
    # Enable Hardened Runtime for notarization
    set(CMAKE_XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME YES)

    # Code signing (set to "-" for ad-hoc signing during development)
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-" CACHE STRING "Code signing identity")
    set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "" CACHE STRING "Development team ID")
endif()

# Compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
        -Wno-missing-field-initializers
    )
    # Debug flags
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
    # Release flags
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()

# ============================================================================
# Dependencies
# ============================================================================

# Check if ASPL submodule exists and has the required headers. Headers live
# under include/aspl/, not aspl/ directly — the submodule ships a normal
# include/ layout, it isn't flattened at the repo root.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/libASPL/include/aspl/Device.hpp")
    set(ASPL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/libASPL/include")
    message(STATUS "Using ASPL from submodule: ${ASPL_INCLUDE_DIR}")
    set(ASPL_FOUND TRUE)
    set(ASPL_LIBRARY "")
    # Not header-only: it ships 23 .cpp files under src/ that any target using
    # aspl::Device/aspl::Plugin (virtual classes) must compile in directly, or
    # linking fails on missing vtables. Globbed into ASPL_SOURCES rather than
    # add_subdirectory() — the submodule's own CMakeLists.txt runs
    # `git describe` for its version tag and pulls in its own tests/docs
    # options, which don't apply here.
    file(GLOB ASPL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/external/libASPL/src/*.cpp")
else()
    # Find libASPL (header-only library) in system locations
    find_path(ASPL_INCLUDE_DIR aspl/Plugin.hpp
        PATHS
            /usr/local/include
            /opt/homebrew/include
    )

    # Also find libASPL static library
    find_library(ASPL_LIBRARY ASPL
        PATHS
            /usr/local/lib
            /opt/homebrew/lib
    )

    if(NOT ASPL_INCLUDE_DIR)
        message(WARNING "libASPL not found — driver and examples will be skipped")
        set(ASPL_FOUND FALSE)
        set(ASPL_INCLUDE_DIR "")
    else()
        message(STATUS "Found libASPL: ${ASPL_INCLUDE_DIR}")
        set(ASPL_FOUND TRUE)
    endif()
endif()

# ============================================================================
# Shared Library
# ============================================================================

# Common source files for both driver and tests
set(SHARED_SOURCES
    Shared/ResourceCleanupManager.cpp
)

# Driver source files
set(DRIVER_SOURCES
    Driver/AES67Device.cpp
    Driver/AES67IOHandler.cpp
    Driver/AudioThreadPriority.cpp
)

# Network Engine source files
set(NETWORK_SOURCES
    NetworkEngine/StreamManager.cpp
    NetworkEngine/RTP/SimpleRTP.cpp
    NetworkEngine/RTP/RTPReceiver.cpp
    NetworkEngine/RTP/RTPTransmitter.cpp
    NetworkEngine/RTP/PCMCodec.cpp
    NetworkEngine/PTP/PTPClock.cpp
    NetworkEngine/PTP/PTPDInterface.cpp
    NetworkEngine/PTP/PTPSlave.cpp
    NetworkEngine/PTP/PTPMaster.cpp
    NetworkEngine/PTP/PTPArbitrator.cpp
    NetworkEngine/PTP/CoreAudioClockSource.cpp
    NetworkEngine/PTP/PTPPeerObserver.cpp
    NetworkEngine/PTP/AudioClockDeviceList.cpp
    NetworkEngine/NetworkUtils.cpp
    NetworkEngine/NetworkInterfaceDetection.cpp
    NetworkEngine/Discovery/SAPListener.cpp
    NetworkEngine/Discovery/MDNSBrowser.cpp
    NetworkEngine/Discovery/RTSPServer.cpp
    NetworkEngine/Discovery/SAPAnnouncer.cpp
    NetworkEngine/Discovery/RTCPMonitor.cpp
    NetworkEngine/Discovery/RTSPClient.cpp
)

# ============================================================================
# The platform-free core, from its own repository
# ============================================================================
#
# What used to be AES67_CORE_SOURCES here now lives in JaumeAP/aes67-core and
# arrives as a submodule. The reason for moving it out rather than exposing it
# from inside: while the core lived here, every other implementation that wanted
# a jitter buffer or an SDP parser had to consume a macOS driver. The firmware,
# a Linux daemon and this driver are peers.
#
# What stays here is what is genuinely macOS -- the AudioServerPlugIn, libASPL,
# the CoreAudio clock sources, the Accelerate codec -- plus the socket layer in
# AES67_NET_SOURCES, which is portable in practice but is not the core's
# business.
# Its tests stay off here: they run in that repository, against that
# repository's gate. Building them from this side would duplicate the targets
# and, worse, suggest this project is where they are maintained.
set(AES67_CORE_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/aes67-core EXCLUDE_FROM_ALL)

# Includes are written relative to the repository root now, not to the file
# doing the including. That became necessary when the core moved out: a header
# that says "Shared/Types.h" resolves the same whether Shared/ is here or in
# external/aes67-core, while "../Shared/Types.h" only resolved while the two
# sat side by side.
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

set(AES67_NET_SOURCES
    Shared/ResourceCleanupManager.cpp
    NetworkEngine/NetworkUtils.cpp
    NetworkEngine/NetworkInterfaceDetection.cpp
    NetworkEngine/StreamManager.cpp
    NetworkEngine/RTP/SimpleRTP.cpp
    NetworkEngine/RTP/RTPReceiver.cpp
    NetworkEngine/RTP/RTPTransmitter.cpp
    NetworkEngine/PTP/PTPSlave.cpp
    NetworkEngine/PTP/PTPMaster.cpp
    NetworkEngine/PTP/PTPPeerObserver.cpp
    NetworkEngine/Discovery/SAPListener.cpp
    NetworkEngine/Discovery/MDNSBrowser.cpp
    NetworkEngine/Discovery/RTSPServer.cpp
    NetworkEngine/Discovery/SAPAnnouncer.cpp
    NetworkEngine/Discovery/RTCPMonitor.cpp
    NetworkEngine/Discovery/RTSPClient.cpp
)

add_library(aes67_net STATIC ${AES67_NET_SOURCES})
target_link_libraries(aes67_net PUBLIC aes67_core)

# Combine all C++ sources
set(ALL_CPP_SOURCES
    ${SHARED_SOURCES}
    ${DRIVER_SOURCES}
    ${NETWORK_SOURCES}
)
# The core's translation units are no longer in these lists; targets link the
# aes67_core library from the submodule instead.

# ============================================================================
# AudioServerPlugIn Bundle (macOS only)
# ============================================================================

if(APPLE AND ASPL_FOUND)
    # Create driver bundle
    add_library(AES67Driver MODULE
        ${ALL_CPP_SOURCES}
        ${ASPL_SOURCES}
        Driver/PlugInMain.cpp  # AudioServerPlugIn entry point
    )

    target_include_directories(AES67Driver PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${ASPL_INCLUDE_DIR}
    )

    target_link_libraries(AES67Driver PRIVATE
        aes67_core
        ${ASPL_LIBRARY}
        # ptpd_obj disabled - using stub PTP implementation
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework AudioToolbox"
        "-framework Accelerate"
    )

    # Bundle properties
    set_target_properties(AES67Driver PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "driver"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Driver/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "AES67"
        MACOSX_BUNDLE_BUNDLE_VERSION "${VERSION_BUILD}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.aes67driver.audiodevice"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.aes67driver.audiodevice"
        XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET "13.0"
        OSX_DEPLOYMENT_TARGET "13.0"
    )

    # Install to standard location
    install(TARGETS AES67Driver
        LIBRARY DESTINATION "/Library/Audio/Plug-Ins/HAL"
        BUNDLE DESTINATION "/Library/Audio/Plug-Ins/HAL"
    )
endif()

# ============================================================================
# Manager App (SwiftUI - macOS only)
# ============================================================================

# Declared here rather than beside BUILD_TESTS/BUILD_EXAMPLES/BUILD_TOOLS
# further down: an option has to exist before the block that reads it.
# The app builds through its own build.sh (raw swiftc, not SwiftPM), so it
# needs a full Xcode toolchain that a plain command-line build may not have.
option(BUILD_MANAGER_APP "Build the SwiftUI Manager app" ON)

if(APPLE AND ASPL_FOUND AND BUILD_MANAGER_APP)
    add_custom_target(ManagerApp ALL
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/ManagerApp/build.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ManagerApp
        COMMENT "Building AES67 Manager App (SwiftUI)"
        VERBATIM
    )

    # Installer target (explicit only — not part of ALL)
    add_custom_target(installer
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/Installer/build-installer.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Installer
        COMMENT "Building .pkg installer"
        DEPENDS AES67Driver ManagerApp
        VERBATIM
    )

    # Install the Manager app to /Applications
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ManagerApp/AES67Manager.app
        DESTINATION "/Applications"
        USE_SOURCE_PERMISSIONS
    )
endif()

# ============================================================================
# Tests
# ============================================================================

option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()

# ============================================================================
# Examples
# ============================================================================

option(BUILD_EXAMPLES "Build example programs" ON)

if(BUILD_EXAMPLES)
    add_subdirectory(Examples)
endif()

# ============================================================================
# Tools
# ============================================================================

option(BUILD_TOOLS "Build test and debugging tools" ON)

if(BUILD_TOOLS)
    add_subdirectory(Tools)
endif()

# ============================================================================
# Documentation
# ============================================================================

find_package(Doxygen QUIET)

if(DOXYGEN_FOUND)
    set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/Docs/Doxyfile.in)
    set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
    configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)

    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )
else()
    message(STATUS "Doxygen not found — 'make docs' target will not be available")
endif()

# ============================================================================
# Installation
# ============================================================================

# Install README and documentation
install(FILES
    README.md
    VERSION.txt
    DESTINATION share/doc/AES67Driver
)

# Install example SDP files
install(DIRECTORY Docs/Examples/
    DESTINATION share/doc/AES67Driver/examples
    FILES_MATCHING PATTERN "*.sdp"
)

# ============================================================================
# Package
# ============================================================================

set(CPACK_PACKAGE_NAME "AES67Driver")
set(CPACK_PACKAGE_VENDOR "AES67 Driver")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "AES67 Audio Driver for macOS")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}.${VERSION_BUILD}")
set(CPACK_PACKAGE_VERSION_MAJOR ${VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${VERSION_PATCH})

if(APPLE)
    set(CPACK_GENERATOR "productbuild")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/")
endif()

include(CPack)

# ============================================================================
# Summary
# ============================================================================

message(STATUS "")
message(STATUS "AES67 Driver Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION} (Build #${VERSION_BUILD})")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
if(APPLE)
    message(STATUS "  macOS deployment target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
    message(STATUS "  Architectures: ${CMAKE_OSX_ARCHITECTURES}")
endif()
message(STATUS "  RTP: SimpleRTP (minimal implementation)")
message(STATUS "  libASPL: ${ASPL_INCLUDE_DIR}")
message(STATUS "  PTP: IEEE 1588 slave-only (PTPSlave.cpp, stub fallback available)")
message(STATUS "  Manager App: ManagerApp target (SwiftUI)")
message(STATUS "  Installer: make installer")
message(STATUS "  Tests: ${BUILD_TESTS}")
message(STATUS "  Examples: ${BUILD_EXAMPLES}")
message(STATUS "")
