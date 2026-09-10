cmake_minimum_required(VERSION 3.20)
project(aes67_ravenna LANGUAGES CXX)

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

# RAVENNA's session layer: how a device finds what this machine offers, and
# how it asks what a session is.
#
# Not a second implementation of anything this repository already has. The SDP
# is aes67-core's -- SDPParser generates what RAVENNA wants, a=framecount
# included -- and the channel routing is aes67-core's StreamChannelMapper, the
# 128-channel matrix with its overlap prevention. What is here is the two
# things neither of those covers: DNS-SD over mDNS, and the RTSP DESCRIBE that
# hands the SDP over.
#
# POSIX sockets, so this builds and runs on macOS and on Linux alike. There is
# no third platform half.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(AES67_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../aes67-core"
    CACHE PATH "Directory holding the aes67-core package")

if(NOT TARGET aes67_core)
    add_subdirectory(${AES67_CORE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/aes67-core)
endif()

add_library(aes67_ravenna
    Ravenna/ChannelMappingApi.cpp
    Ravenna/ConnectionApi.cpp
    Ravenna/DnsSd.cpp
    Ravenna/HttpServer.cpp
    Ravenna/Json.cpp
    Ravenna/NodeApi.cpp
    Ravenna/ReceiverRouting.cpp
    Ravenna/MdnsResponder.cpp
    Ravenna/RtspMessages.cpp
    Ravenna/RtspServer.cpp
    Ravenna/SessionCatalogue.cpp
)
target_include_directories(aes67_ravenna PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aes67_ravenna PUBLIC aes67_core)
target_compile_options(aes67_ravenna PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

# The tool and the tests are what the package builds on its own. Another
# package that links the library (the macOS driver, for the mDNS responder)
# turns them off: it has a gate of its own and this package has one too.
option(AES67_RAVENNA_TOOLS_AND_TESTS "Build ravenna-announce and the test suites" ON)

if(AES67_RAVENNA_TOOLS_AND_TESTS)
    # One session, advertised, so the package can be pointed at real gear
    # rather than only at its own tests.
    add_executable(ravenna-announce Tools/ravenna-announce.cpp)
    target_link_libraries(ravenna-announce PRIVATE aes67_ravenna)
    target_compile_options(ravenna-announce PRIVATE -Wall -Wextra -Wpedantic)

    option(BUILD_TESTS "Build the tests" ON)

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
            # RavennaKitParity reads the checkout under external/ for its
            # vectors and links nothing from it: the comparison is against what
            # that project's own suite says the bytes mean, not against its
            # code, so this package keeps its dependency count at zero.
            foreach(suite RtspMessages DnsSd SessionCatalogue Json ConnectionApi ReceiverRouting
                     ChannelMappingApi NodeApi RavennaKitParity)
                add_executable(Test${suite} Tests/Test${suite}.cpp)
                target_link_libraries(Test${suite} PRIVATE aes67_ravenna doctest_headers)
                add_test(NAME ${suite} COMMAND Test${suite})
                set_tests_properties(${suite} PROPERTIES LABELS "unit" TIMEOUT 60)
            endforeach()

    # The parsers, fed what nobody wrote a test for: Tests/fuzz/FuzzParsers.cpp.
    # Not a CTest test -- the gate runs it under AES67_ANALYSE=1, in the
    # sanitizer build, with an iteration count. -DAES67_LIBFUZZER=ON builds the
    # coverage-guided form instead, for a clang that ships the runtime.
    option(AES67_LIBFUZZER "Build the fuzz harness against libFuzzer" OFF)
    add_executable(FuzzRavennaParsers Tests/fuzz/FuzzParsers.cpp)
    target_link_libraries(FuzzRavennaParsers PRIVATE aes67_ravenna)
    if(AES67_LIBFUZZER)
        target_compile_definitions(FuzzRavennaParsers PRIVATE FUZZ_LIBFUZZER=1)
        target_compile_options(FuzzRavennaParsers PRIVATE -fsanitize=fuzzer)
        target_link_options(FuzzRavennaParsers PRIVATE -fsanitize=fuzzer)
    endif()

        else()
            message(WARNING "${AES67_DOCTEST_DIR} is empty - run: git submodule update --init --recursive")
        endif()
    endif()
endif()
