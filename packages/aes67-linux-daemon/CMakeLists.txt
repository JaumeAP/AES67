cmake_minimum_required(VERSION 3.20)
project(aes67_linux_daemon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# This package owns no audio code. What it owns is the wiring around
# bondagit/aes67-linux-daemon, which is vendored under external/ as a
# submodule: the daemon that runs in user space, and the Merging RAVENNA/AES67
# ALSA kernel module it drives through netlink, which the daemon carries as a
# submodule of its own.
#
# Nothing here patches upstream. A change in behaviour belongs in the upstream
# project, not in a copy kept next to it.

set(AES67_LINUX_DAEMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/aes67-linux-daemon"
    CACHE PATH "Directory holding the aes67-linux-daemon submodule")

if(NOT EXISTS "${AES67_LINUX_DAEMON_DIR}/daemon/CMakeLists.txt")
    message(WARNING "${AES67_LINUX_DAEMON_DIR} is empty - run: git submodule update --init --recursive")
    return()
endif()

# aes67-profile-conf: the compatibility profiles, written into the daemon's
# configuration file. It reads the profiles package and a base daemon.conf and
# touches nothing else, so it builds and is tested wherever this repository is
# read, Linux or not -- which is the half of this package a Mac can verify.
set(AES67_PROFILES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../aes67-profiles"
    CACHE PATH "Directory holding the profiles package")
if(NOT TARGET aes67_profiles)
    add_subdirectory(${AES67_PROFILES_DIR} ${CMAKE_CURRENT_BINARY_DIR}/aes67-profiles)
endif()

# The core, for its SDP writer: SdpCompare puts the daemon's SDP and this
# project's side by side, and the second half of that comparison is
# SDPParser::generate. The core is platform-free, so this costs the package
# nothing on a system that is not Linux.
set(AES67_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../aes67-core"
    CACHE PATH "Directory holding the core package")
if(NOT TARGET aes67_core)
    set(AES67_CORE_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${AES67_CORE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/aes67-core)
endif()

add_library(aes67_profile_conf Tools/ProfileConf.cpp Tools/ConfCheck.cpp
                            Tools/SourceGen.cpp Tools/DaemonSdp.cpp
                            Tools/SdpCompare.cpp)
target_include_directories(aes67_profile_conf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aes67_profile_conf PUBLIC aes67_profiles aes67_core)
target_compile_options(aes67_profile_conf PRIVATE -Wall -Wextra -Wpedantic)

add_executable(aes67-profile-conf Tools/ProfileConfMain.cpp)
target_link_libraries(aes67-profile-conf PRIVATE aes67_profile_conf)
target_compile_definitions(aes67-profile-conf PRIVATE
    AES67_DEFAULT_DAEMON_CONF="${AES67_LINUX_DAEMON_DIR}/daemon/daemon.conf")
target_compile_options(aes67-profile-conf PRIVATE -Wall -Wextra -Wpedantic)

option(AES67_LINUX_DAEMON_TESTS "Build the test suite" ON)
if(AES67_LINUX_DAEMON_TESTS)
    enable_testing()

    # doctest lives once, at the root of the monorepo. This branch is what
    # makes the package still configure on its own, outside the tree.
    if(NOT DEFINED AES67_DOCTEST_DIR)
        set(AES67_DOCTEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/doctest/doctest")
    endif()
    if(NOT TARGET doctest_headers)
        add_library(doctest_headers INTERFACE)
        target_include_directories(doctest_headers INTERFACE ${AES67_DOCTEST_DIR})
    endif()

    foreach(suite ProfileConf ConfCheck SourceGen DaemonSdp)
        add_executable(Test${suite} Tests/Test${suite}.cpp)
        target_link_libraries(Test${suite} PRIVATE aes67_profile_conf doctest_headers)
        add_test(NAME ${suite} COMMAND Test${suite})
        set_tests_properties(${suite} PROPERTIES LABELS "unit" TIMEOUT 60)
    endforeach()
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The daemon talks to the kernel module over netlink and the module is a
    # Linux kernel module. Configuring either anywhere else would mean a build
    # that succeeds without having compiled what matters.
    message(STATUS "aes67-linux-daemon: not Linux, the daemon is not built")
    return()
endif()

# The upstream daemon reads these as cache entries; build.sh passes the same
# ones on the command line.
# The module is its own package, packages/ravenna-alsa-lkm, and this is the
# checkout the daemon is built against: one pin, one copy, read from where it
# lives rather than copied here.
set(AES67_LINUX_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ravenna-alsa-lkm"
    CACHE PATH "Directory holding the kernel module package")
set(RAVENNA_ALSA_LKM_DIR "${AES67_LINUX_MODULE_DIR}/external/ravenna-alsa-lkm"
    CACHE PATH "Directory holding the RAVENNA ALSA kernel module sources")
set(CPP_HTTPLIB_DIR "${AES67_LINUX_DAEMON_DIR}/3rdparty/cpp-httplib"
    CACHE PATH "Directory holding cpp-httplib")
set(ENABLE_TESTS ON CACHE BOOL "Build the daemon's own tests")
set(WITH_AVAHI ON CACHE BOOL "mDNS support via Avahi")
set(WITH_SYSTEMD ON CACHE BOOL "systemd notify and watchdog support")
set(WITH_STREAMER ON CACHE BOOL "Streamer support")
set(WITH_NMOS ON CACHE BOOL "NMOS IS-04 Node API and registration")
set(FAKE_DRIVER OFF CACHE BOOL "Use the fake driver instead of RAVENNA")

add_subdirectory("${AES67_LINUX_DAEMON_DIR}/daemon"
                 "${CMAKE_CURRENT_BINARY_DIR}/aes67-daemon")
