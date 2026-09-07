cmake_minimum_required(VERSION 3.20)
project(aes67_ravenna LANGUAGES CXX)

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
    Ravenna/ConnectionApi.cpp
    Ravenna/DnsSd.cpp
    Ravenna/HttpServer.cpp
    Ravenna/Json.cpp
    Ravenna/ReceiverRouting.cpp
    Ravenna/MdnsResponder.cpp
    Ravenna/RtspMessages.cpp
    Ravenna/RtspServer.cpp
    Ravenna/SessionCatalogue.cpp
)
target_include_directories(aes67_ravenna PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aes67_ravenna PUBLIC aes67_core)
target_compile_options(aes67_ravenna PRIVATE -Wall -Wextra -Wpedantic)

# One session, advertised, so the package can be pointed at real gear rather
# than only at its own tests.
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
        foreach(suite RtspMessages DnsSd SessionCatalogue Json ConnectionApi ReceiverRouting)
            add_executable(Test${suite} Tests/Test${suite}.cpp)
            target_link_libraries(Test${suite} PRIVATE aes67_ravenna doctest_headers)
            add_test(NAME ${suite} COMMAND Test${suite})
            set_tests_properties(${suite} PROPERTIES LABELS "unit" TIMEOUT 60)
        endforeach()
    else()
        message(WARNING "${AES67_DOCTEST_DIR} is empty - run: git submodule update --init --recursive")
    endif()
endif()
