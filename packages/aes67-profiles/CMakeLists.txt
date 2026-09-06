cmake_minimum_required(VERSION 3.20)
project(aes67_profiles LANGUAGES CXX)

# What every implementation in this repository agrees on, and nothing else: the
# compatibility profiles, the per-model Dolby data and the PTP profiles. No
# platform, no sockets, no operating system, and no dependency on any other
# package -- including the core, which is what the SDP-free StreamDescription
# in Profiles/ is for.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(aes67_profiles
    Profiles/CompatibilityProfile.cpp
)

target_include_directories(aes67_profiles PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(aes67_profiles PRIVATE -Wall -Wextra -Wpedantic)

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
