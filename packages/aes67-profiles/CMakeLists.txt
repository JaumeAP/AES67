cmake_minimum_required(VERSION 3.20)
project(aes67_profiles LANGUAGES CXX)

# Instrumentation, for the analysis half of the gate. Both are off by default
# and both are applied once for the whole tree: a package built as another's
# subdirectory inherits the flags from the directory that set them, and this
# property is what stops it adding them a second time.
#
#   -DAES67_SANITIZE=address,undefined   ASan and UBSan, in build-san
#   -DAES67_COVERAGE=ON                  gcov counters, in build-cov
set(AES67_SANITIZE "" CACHE STRING "Sanitizers to build with, e.g. address,undefined")
option(AES67_COVERAGE "Build with coverage counters" OFF)
get_property(_aes67_instrumented GLOBAL PROPERTY AES67_INSTRUMENTATION_APPLIED)
if(NOT _aes67_instrumented)
    set_property(GLOBAL PROPERTY AES67_INSTRUMENTATION_APPLIED ON)
    if(AES67_SANITIZE)
        message(STATUS "Sanitizers enabled: ${AES67_SANITIZE}")
        add_compile_options(-fsanitize=${AES67_SANITIZE} -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=${AES67_SANITIZE})
    endif()
    if(AES67_COVERAGE)
        message(STATUS "Coverage counters enabled")
        add_compile_options(--coverage -O0 -g)
        add_link_options(--coverage)
    endif()
endif()

# What every implementation in this repository agrees on, and nothing else: the
# compatibility profiles, the per-model Dolby data and the PTP profiles. No
# platform, no sockets, no operating system, and no dependency on any other
# package -- including the core, which is what the SDP-free StreamDescription
# in Profiles/ is for.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(aes67_profiles
    Profiles/CompatibilityProfile.cpp
    Profiles/ConfigPaths.cpp
)

target_include_directories(aes67_profiles PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(aes67_profiles PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

# Tests, on the same terms as the other packages: doctest from the root of the
# monorepo, one binary per suite, registered with CTest.
option(BUILD_TESTS "Build the profile tests" ON)

if(BUILD_TESTS)
    if(NOT DEFINED AES67_DOCTEST_DIR)
        set(AES67_DOCTEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/doctest/doctest"
            CACHE PATH "Directory holding doctest.h")
    endif()
    if(EXISTS "${AES67_DOCTEST_DIR}/doctest.h")
        if(NOT TARGET doctest_headers)
            add_library(doctest_headers INTERFACE)
            target_include_directories(doctest_headers INTERFACE ${AES67_DOCTEST_DIR})
        endif()

        enable_testing()
        foreach(suite CompatibilityProfile DolbyModelCatalog PtpProfiles)
            add_executable(Test${suite} Tests/Test${suite}.cpp)
            target_link_libraries(Test${suite} PRIVATE aes67_profiles doctest_headers)
            add_test(NAME ${suite} COMMAND Test${suite})
            set_tests_properties(${suite} PROPERTIES LABELS "unit" TIMEOUT 60)
        endforeach()
    else()
        message(WARNING "${AES67_DOCTEST_DIR} is empty - run: git submodule update --init --recursive")
    endif()
endif()
