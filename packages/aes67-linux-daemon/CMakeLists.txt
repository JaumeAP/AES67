cmake_minimum_required(VERSION 3.20)
project(aes67_linux_daemon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# What this package is, now that the daemon's own source is not vendored here:
# the tools that turn this repository's compatibility profiles into the AES67
# Linux daemon's configuration, and that read one back and say what is wrong
# with it. They need the profiles, the core's SDP writer and a text file --
# never the daemon itself, which is why they build and are tested wherever
# this repository is read.
#
# The daemon is bondagit/aes67-linux-daemon, built from its own checkout; the
# kernel module it drives is packages/ravenna-alsa-lkm. Where the tools mirror
# the daemon's own code -- Tools/DaemonSdp, from session_manager.cpp -- the
# mirror names the file and the lines it came from, because there is no copy
# in this tree to diff against.

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
    # The base a rewrite starts from: upstream's own daemon.conf, kept here as
    # a file rather than reached for in a checkout of the daemon this package
    # no longer carries. It is upstream's, verbatim, and NOTICE says so.
    AES67_DEFAULT_DAEMON_CONF="${CMAKE_CURRENT_SOURCE_DIR}/Tools/daemon.conf.upstream")
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

