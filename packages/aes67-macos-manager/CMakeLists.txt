cmake_minimum_required(VERSION 3.20)
project(aes67_macos_manager LANGUAGES NONE)

# The SwiftUI manager app: the thing a person double-clicks. It installs the
# driver, the PTP daemon and the daemon's LaunchDaemon plist, which it carries
# as resources, so it is how the rest of the macOS half reaches the system.
#
# It was a directory inside packages/aes67-macos-driver until it was not. The
# two are one delivery and two programs: an AudioServerPlugIn that coreaudiod
# loads, and an app a person runs. They are built by different toolchains
# (Objective-C++ under CMake; raw swiftc under build.sh), they fail for
# different reasons, and a machine without a Swift toolchain can still build
# the driver -- which is what BUILD_MANAGER_APP=OFF used to exist for and
# what separate packages say without a flag.
#
# LANGUAGES NONE, because CMake compiles nothing here: build.sh does, and this
# file only makes it a target, wires its test into CTest and names where the
# driver's artifacts come from.

# The driver's binary directory: where AES67Driver.driver and aes67ptpd land.
# The default is that package's own build/ next door, which is what its gate
# fills. The monorepo's root CMakeLists passes its own binary directory for the
# driver instead, so a root build embeds what the root build built.
set(AES67_MACOS_DRIVER_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../aes67-macos-driver/build"
    CACHE PATH "Directory holding the built driver bundle and PTP daemon")

if(NOT APPLE)
    message(STATUS "aes67-macos-manager: not macOS, nothing to build")
    return()
endif()

add_custom_target(ManagerApp ALL
    COMMAND ${CMAKE_COMMAND} -E env AES67_BUILD_DIR=${AES67_MACOS_DRIVER_BUILD_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/build.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building AES67 Manager App (SwiftUI)"
    VERBATIM
)

# Inside the monorepo the driver is a target of the same build, and the app
# has to wait for it or it embeds yesterday's bundle. Standalone there is no
# such target, and build.sh says what it could not find.
if(TARGET AES67Driver)
    add_dependencies(ManagerApp AES67Driver)
endif()
if(TARGET aes67ptpd)
    add_dependencies(ManagerApp aes67ptpd)
endif()

option(BUILD_TESTS "Register the app's host tests with CTest" ON)
if(BUILD_TESTS)
    enable_testing()
    # Plain Swift over plain values: no SwiftUI, no Core Audio, no driver. See
    # run-tests.sh.
    add_test(NAME ManagerAppUnit
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run-tests.sh)
    set_tests_properties(ManagerAppUnit PROPERTIES
        LABELS "unit"
        TIMEOUT 120
        ENVIRONMENT "AES67_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}")
endif()

# Disk image, explicit only: not part of ALL. This is the whole delivery -- the
# app installs the driver, the daemon and the LaunchDaemon itself, so there is
# no package to build and no installer to run.
add_custom_target(dmg
    COMMAND ${CMAKE_COMMAND} -E env AES67_BUILD_DIR=${AES67_MACOS_DRIVER_BUILD_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/build-dmg.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building AES67Manager.dmg"
    DEPENDS ManagerApp
    VERBATIM
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/AES67Manager.app
    DESTINATION "/Applications"
    USE_SOURCE_PERMISSIONS
)
