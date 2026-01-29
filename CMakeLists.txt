# CMakeLists.txt
# AES67 macOS Driver - Build #4
# Cross-platform build configuration

cmake_minimum_required(VERSION 3.20)

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

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# macOS specific settings
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures")

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

# Find Boost for lockfree containers
find_package(Boost REQUIRED COMPONENTS system thread)
if(Boost_FOUND)
    message(STATUS "Found Boost: ${Boost_INCLUDE_DIRS}")
else()
    message(FATAL_ERROR "Boost library not found. Please install Boost.")
endif()

# ============================================================================
# Dependencies
# ============================================================================

# Check if ASPL submodule exists
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/libASPL/aspl/Plugin.hpp")
    set(ASPL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/libASPL")
    message(STATUS "Using ASPL from submodule: ${ASPL_INCLUDE_DIR}")

    # Add ASPL as a subdirectory if it has its own CMakeLists.txt
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/libASPL/CMakeLists.txt")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/libASPL" ASPL)
        set(ASPL_LIBRARY ASPL::ASPL)
    else()
        # If ASPL doesn't have CMakeLists.txt, treat as header-only
        set(ASPL_LIBRARY "")
    endif()
else()
    # Find libASPL (header-only library) in system locations
    find_path(ASPL_INCLUDE_DIR aspl/Plugin.hpp
        PATHS
            /usr/local/include
            /opt/homebrew/include
            ${CMAKE_SOURCE_DIR}/external/libASPL/include
    )

    # Also find libASPL static library
    find_library(ASPL_LIBRARY ASPL
        PATHS
            /usr/local/lib
            /opt/homebrew/lib
    )

    if(NOT ASPL_INCLUDE_DIR)
        message(FATAL_ERROR "libASPL not found. Please install from https://github.com/gavv/libASPL or add as submodule in external/libASPL/")
    endif()

    message(STATUS "Found libASPL: ${ASPL_INCLUDE_DIR}")
endif()

# Add ptpd source files directly to the project
set(PTPD_SOURCES
    NetworkEngine/PTP/vendor/ptpd/src/ptpd.c
    NetworkEngine/PTP/vendor/ptpd/src/protocol.c
    NetworkEngine/PTP/vendor/ptpd/src/display.c
    NetworkEngine/PTP/vendor/ptpd/src/management.c
    NetworkEngine/PTP/vendor/ptpd/src/ptp_timers.c
    NetworkEngine/PTP/vendor/ptpd/src/timingdomain.c
    NetworkEngine/PTP/vendor/ptpd/src/signaling.c
    NetworkEngine/PTP/vendor/ptpd/src/arith.c
    NetworkEngine/PTP/vendor/ptpd/src/bmc.c
    # Platform-specific implementations
    NetworkEngine/PTP/vendor/ptpd/src/dep/sys.c
    NetworkEngine/PTP/vendor/ptpd/src/dep/msg.c
    NetworkEngine/PTP/vendor/ptpd/src/dep/net.c
    NetworkEngine/PTP/vendor/ptpd/src/dep/eventtimer.c
    NetworkEngine/PTP/vendor/ptpd/src/dep/configdefaults.c
    NetworkEngine/PTP/vendor/ptpd/src/dep/daemonconfig.c
    NetworkEngine/PTP/vendor/ptpd/src/dep/startup.c
    NetworkEngine/PTP/vendor/ptpd/src/dep/statistics.c
    NetworkEngine/PTP/vendor/ptpd/src/dep/alarms.c
    NetworkEngine/PTP/vendor/ptpd/src/dep/outlierfilter.c
    NetworkEngine/PTP/vendor/ptpd/src/dep/servo.c
    NetworkEngine/PTP/vendor/ptpd/src/dep/snmp.c
)

# Create a custom target for ptpd to ensure proper compilation flags
add_library(ptpd_obj OBJECT ${PTPD_SOURCES})
target_include_directories(ptpd_obj PRIVATE
    NetworkEngine/PTP/vendor/ptpd/src
    NetworkEngine/PTP/vendor/ptpd/src/def
    NetworkEngine/PTP/vendor/ptpd/src/dep
)
target_compile_definitions(ptpd_obj PRIVATE
    # Define flags to customize ptpd behavior for embedding in coreaudiod
    PTPD_STANDALONE=0  # Indicate that we're embedding ptpd
    PTPD_NO_DAEMON=1   # Don't fork or become a daemon
    PTPD_THREADED=1    # Enable threaded operation
    PTPD_NO_COMMAND=1  # Don't include command-line interface
    # Define platform-specific flags if needed
    _GNU_SOURCE
    # Disable features that might interfere with coreaudiod process
    DISABLE_LOGGING=1  # Minimize logging in audio process
)

# ============================================================================
# Shared Library
# ============================================================================

# Common source files for both driver and tests
set(SHARED_SOURCES
    Shared/Types.cpp
    Shared/Config.cpp
    Shared/NonBlockingLogger.cpp
    Shared/ResourceCleanupManager.cpp
    Shared/ErrorRecoveryManager.cpp
)

# Driver source files
set(DRIVER_SOURCES
    Driver/SDPParser.cpp
    Driver/AES67Device.cpp
    Driver/AES67IOHandler.cpp
    Driver/AudioThreadPriority.cpp
)

# Network Engine source files
set(NETWORK_SOURCES
    NetworkEngine/StreamChannelMapper.cpp
    NetworkEngine/StreamManager.cpp
    NetworkEngine/StreamConfig.cpp
    NetworkEngine/RTP/SimpleRTP.cpp
    NetworkEngine/RTP/RTPReceiver.cpp
    NetworkEngine/RTP/RTPTransmitter.cpp
    NetworkEngine/RTP/LockFreeCircularJitterBuffer.cpp
    NetworkEngine/RTP/LockFreePacketPool.cpp
    NetworkEngine/RTP/BufferStatusMonitor.cpp
    NetworkEngine/PTP/PTPClock.cpp
    NetworkEngine/PTP/PTPDInterface.cpp
    NetworkEngine/PTP/CustomSys.cpp
    NetworkEngine/PTP/PhaseLockedLoop.cpp
    NetworkEngine/Resampling/Resampler.cpp
    NetworkEngine/Resampling/ClockAdjustmentController.cpp
    NetworkEngine/Resampling/SmoothedPIController.cpp
    NetworkEngine/Resampling/SampleRateAdapter.cpp
    NetworkEngine/NetworkUtils.cpp
    NetworkEngine/NetworkErrorHandler.cpp
    NetworkEngine/NetworkInterfaceDetection.cpp
    NetworkEngine/DoPDecoder.cpp
    NetworkEngine/Discovery/SAPListener.cpp
    NetworkEngine/Discovery/RTSPClient.cpp
)

# Combine all C++ sources
set(ALL_CPP_SOURCES
    ${SHARED_SOURCES}
    ${DRIVER_SOURCES}
    ${NETWORK_SOURCES}
)

# ============================================================================
# AudioServerPlugIn Bundle (macOS only)
# ============================================================================

if(APPLE)
    # Create driver bundle
    add_library(AES67Driver MODULE
        ${ALL_CPP_SOURCES}
        Driver/PlugInMain.cpp  # AudioServerPlugIn entry point
    )

    target_include_directories(AES67Driver PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${ASPL_INCLUDE_DIR}
    )

    target_include_directories(AES67Driver PRIVATE
        ${Boost_INCLUDE_DIRS}
    )

    target_link_libraries(AES67Driver PRIVATE
        ${ASPL_LIBRARY}
        ptpd_obj  # Link with our embedded ptpd library
        ${Boost_LIBRARIES}
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework AudioToolbox"
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

if(APPLE)
    # Note: SwiftUI app will be built separately via Xcode
    # This CMake file focuses on the C++ driver component
    # See ManagerApp/AES67Manager.xcodeproj for Swift app
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
# Documentation
# ============================================================================

option(BUILD_DOCS "Build documentation" OFF)

if(BUILD_DOCS)
    find_package(Doxygen)
    if(DOXYGEN_FOUND)
        set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/Docs/Doxyfile.in)
        set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
        configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)

        add_custom_target(docs ALL
            COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating API documentation with Doxygen"
            VERBATIM
        )
    endif()
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
message(STATUS "  ptpd: Embedded (from source)")
message(STATUS "  Tests: ${BUILD_TESTS}")
message(STATUS "  Examples: ${BUILD_EXAMPLES}")
message(STATUS "")
