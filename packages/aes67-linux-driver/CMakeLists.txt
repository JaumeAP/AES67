cmake_minimum_required(VERSION 3.20)
project(aes67_linux_driver LANGUAGES NONE)

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

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The daemon talks to the kernel module over netlink and the module is a
    # Linux kernel module. Configuring either anywhere else would mean a build
    # that succeeds without having compiled what matters.
    message(STATUS "aes67-linux-driver: not Linux, nothing to build")
    return()
endif()

enable_language(CXX)

# The upstream daemon reads these as cache entries; build.sh passes the same
# ones on the command line.
set(RAVENNA_ALSA_LKM_DIR "${AES67_LINUX_DAEMON_DIR}/3rdparty/ravenna-alsa-lkm"
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

# The kernel module is built by the kernel's own build system, not by CMake.
# It is a target here so that one command builds both halves, and it is not
# part of ALL: building a module wants the running kernel's headers, which a
# container or a cross build may not have.
add_custom_target(ravenna-alsa-lkm
    COMMAND ${CMAKE_COMMAND} -E echo "Building the RAVENNA ALSA kernel module"
    COMMAND make -C "${RAVENNA_ALSA_LKM_DIR}/driver"
    WORKING_DIRECTORY "${RAVENNA_ALSA_LKM_DIR}/driver"
    COMMENT "RAVENNA ALSA kernel module"
    VERBATIM)
