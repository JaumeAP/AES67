cmake_minimum_required(VERSION 3.20)
project(ravenna_alsa_lkm LANGUAGES NONE)

# The RAVENNA/AES67 ALSA kernel module, from Merging Technologies through
# bondagit's fork, vendored under external/ as a submodule. This package owns
# no code: it owns the checkout, the one command that builds it, and the gate
# that says whether it is there.
#
# It is its own package because it is its own thing: a Linux kernel module,
# built by the kernel's build system against the running kernel's headers,
# while the daemon that drives it is a user-space process. They are two
# halves of one device and they fail in different ways.

set(AES67_LKM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ravenna-alsa-lkm"
    CACHE PATH "Directory holding the RAVENNA ALSA kernel module sources")

if(NOT EXISTS "${AES67_LKM_DIR}/driver/Makefile")
    message(WARNING "${AES67_LKM_DIR} is empty - run: git submodule update --init --recursive")
    return()
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "ravenna-alsa-lkm: not Linux, nothing to build")
    return()
endif()

# Not part of ALL: building a module wants the running kernel's headers, which
# a container or a cross build may not have, and a configure that succeeds is
# not a promise that a build will.
add_custom_target(ravenna-alsa-lkm
    COMMAND ${CMAKE_COMMAND} -E echo "Building the RAVENNA ALSA kernel module"
    COMMAND make -C "${AES67_LKM_DIR}/driver"
    WORKING_DIRECTORY "${AES67_LKM_DIR}/driver"
    COMMENT "RAVENNA ALSA kernel module"
    VERBATIM)
