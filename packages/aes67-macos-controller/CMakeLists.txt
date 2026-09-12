cmake_minimum_required(VERSION 3.20)
project(aes67_macos_controller LANGUAGES NONE)

# The controller: the application that routes other people's devices.
#
# A package of its own rather than a second window in the Manager, because who
# runs it is different. The Manager is about this machine -- it installs the
# driver and the PTP daemon, activates the device, sets the profile -- and an
# installer routing a plant should not have to install an audio driver to open
# a crosspoint matrix. Dante splits the same way, and for the same reason.
#
# What it shares with the Manager it compiles from that package rather than
# copying: the NMOS client, the session merge and the crosspoint matrix are
# one implementation used by two applications. What it adds is its own
# discovery, through the C bridge over aes67_net
# (NetworkEngine/Discovery/DiscoveryBridge.h) -- the same SAPListener and the
# same mDNS browse plus RTSP DESCRIBE the driver runs, in this process.
#
# LANGUAGES NONE: build.sh compiles it with raw swiftc, like the Manager, and
# this file only makes it a target and says where the driver's libraries are.

# The driver's binary directory: where libaes67_net.a and the libraries under
# it land. Default is that package's own build/, which its gate fills; the
# root CMakeLists passes its own binary directory instead.
set(AES67_MACOS_DRIVER_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../aes67-macos-driver/build"
    CACHE PATH "Directory holding the built driver libraries")

if(NOT APPLE)
    message(STATUS "aes67-macos-controller: not macOS, nothing to build")
    return()
endif()

add_custom_target(ControllerApp ALL
    COMMAND ${CMAKE_COMMAND} -E env AES67_DRIVER_BUILD_DIR=${AES67_MACOS_DRIVER_BUILD_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/build.sh --force
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building AES67 Controller (SwiftUI)"
    VERBATIM
)

# Inside the monorepo the libraries are targets of the same build, and the app
# has to wait for them or it links yesterday's. Standalone there is no such
# target, and build.sh says which library it could not find.
if(TARGET aes67_net)
    add_dependencies(ControllerApp aes67_net)
endif()

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/AES67Controller.app
    DESTINATION "/Applications"
    USE_SOURCE_PERMISSIONS
)
