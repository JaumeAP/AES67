cmake_minimum_required(VERSION 3.20)
project(aes67_linux_ptpd LANGUAGES CXX)

# A PTP grandmaster for Linux, and specifically for a board whose NIC keeps a
# PTP hardware clock -- a Raspberry Pi 5 is the one this was written for.
#
# Two halves, and the split is the point. PtpWire.cpp is the byte layout of the
# four messages and builds anywhere, which is what makes it testable on the
# machine the code is written on. Everything else is Linux: SO_TIMESTAMPING,
# SIOCSHWTSTAMP, /dev/ptpN. Configuring this package on a Mac gives you the
# wire library and its tests and no daemon, on purpose, rather than a
# configure error.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(AES67_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../aes67-core"
    CACHE PATH "Directory holding the aes67-core package")

if(NOT TARGET aes67_core)
    add_subdirectory(${AES67_CORE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/aes67-core)
endif()

# The message layouts. No sockets, no Linux, no clock: it is handed the
# numbers and returns bytes.
add_library(aes67_ptp_wire
    src/PtpWire.cpp
)
target_include_directories(aes67_ptp_wire PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(aes67_ptp_wire PUBLIC aes67_core)
target_compile_options(aes67_ptp_wire PRIVATE -Wall -Wextra -Wpedantic)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(aes67-ptpd
        src/main.cpp
        src/Grandmaster.cpp
        src/PhcClock.cpp
        src/PtpSockets.cpp
    )
    target_link_libraries(aes67-ptpd PRIVATE aes67_ptp_wire)
    target_compile_options(aes67-ptpd PRIVATE -Wall -Wextra -Wpedantic)

    install(TARGETS aes67-ptpd RUNTIME DESTINATION bin)
    install(FILES systemd/aes67-ptpd.service
            DESTINATION lib/systemd/system)
else()
    message(STATUS
        "aes67-linux-ptpd: not Linux, building the wire library and its tests only")
endif()

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
        add_executable(TestPtpWire Tests/TestPtpWire.cpp)
        target_link_libraries(TestPtpWire PRIVATE aes67_ptp_wire doctest_headers)
        add_test(NAME PtpWire COMMAND TestPtpWire)
        set_tests_properties(PtpWire PROPERTIES LABELS "unit" TIMEOUT 60)
    else()
        message(WARNING "${AES67_DOCTEST_DIR} is empty - run: git submodule update --init --recursive")
    endif()
endif()
