# CMakeLists.txt
# Monorepo root
#
# The packages under packages/ came from separate repositories and each keeps
# its own build files. This file only makes the macOS driver reachable from
# the repository root, so `cmake -S . -B build` and `ctest --test-dir build`
# still work the way they did before the move. The firmware and fork packages
# (AES67-master-box-, t41-ptp, QNEthernet) build with their own toolchains and
# are deliberately not wired in here; packages/aes67-core is pulled in by the
# driver itself.

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

add_subdirectory(packages/aes67_macos_driver)
