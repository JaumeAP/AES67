cmake_minimum_required(VERSION 3.20)
# C as well as C++: the RAVENNA module's own PTP.c is compiled into one test,
# so that what it decides is exercised rather than mirrored.
project(aes67_linux_ptpd LANGUAGES C CXX)

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
target_compile_options(aes67_ptp_wire PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # ptp-servo.cpp is the Teensy library's servo, compiled in rather than
    # rewritten: it is platform-free on purpose, and a second PI loop against
    # a one-per-second reference is a second thing to get wrong.
    add_executable(aes67-ptpd
        src/main.cpp
        src/Options.cpp
        src/ExternalReference.cpp
        src/Grandmaster.cpp
        src/PhcClock.cpp
        src/PtpSockets.cpp
        ${AES67_T41_PTP_DIR}/ptp/ptp-servo.cpp
    )
    target_link_libraries(aes67-ptpd PRIVATE aes67_ptp_wire)
    target_compile_options(aes67-ptpd PRIVATE -Wall -Wextra -Wpedantic)

    install(TARGETS aes67-ptpd RUNTIME DESTINATION bin)
    install(FILES systemd/aes67-ptpd.service
            DESTINATION lib/systemd/system)
    install(FILES README.md
            DESTINATION share/doc/aes67-ptpd)

    # A .deb, so the thing that reaches the Pi is one file and dpkg owns what
    # it puts where. The architecture is whatever this is being built on:
    # cpack asks dpkg, and a package built on an arm64 runner says arm64.
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGE_NAME "aes67-ptpd")
    set(CPACK_PACKAGE_VENDOR "JaumeAP")
    set(CPACK_PACKAGE_VERSION "0.1.0")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "PTP grandmaster announcing the NIC's own hardware clock")
    set(CPACK_PACKAGE_CONTACT "https://github.com/JaumeAP/AES67")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
    set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    include(CPack)
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
        # Options is in the list although the daemon it configures is
        # Linux-only: src/Options.cpp was split out of main.cpp precisely so
        # that the half which is only numbers builds and runs anywhere, the
        # same reason PtpWire.cpp is its own library.
        foreach(suite PtpWire ReferenceMath Options)
            add_executable(Test${suite} Tests/Test${suite}.cpp)
            target_link_libraries(Test${suite} PRIVATE aes67_ptp_wire doctest_headers)
            add_test(NAME ${suite} COMMAND Test${suite})
            set_tests_properties(${suite} PROPERTIES LABELS "unit" TIMEOUT 60)
        endforeach()
        target_sources(TestOptions PRIVATE src/Options.cpp)

        # What this daemon sends, against the rules of a real slave: the
        # RAVENNA ALSA kernel module, which arrives inside the
        # external/aes67-linux-daemon submodule -- a recursive checkout is
        # what puts it on disk.
        #
        # The module's own PTP receive path, compiled into the test rather
        # than mirrored in C++ beside it: Tests/support/lkm holds the handful
        # of kernel names PTP.c uses, and RavennaSlave.cpp is the shim that
        # frames a payload the way a netfilter hook delivers one. An upstream
        # change now breaks this test instead of quietly outdating a comment.
        set(lkm_driver ${CMAKE_CURRENT_SOURCE_DIR}/../../external/aes67-linux-daemon/3rdparty/ravenna-alsa-lkm/driver)

        # The module is C and uses bool, true and false throughout. In a kernel
        # build those arrive with <linux/types.h>, which MTAL_stdint.h includes
        # only under MTAL_KERNEL; out of the kernel it takes <stdint.h>, which
        # promises none of them. PTP.h is also the first thing PTP.c includes,
        # so nothing the stub directory holds is reached in time to help.
        #
        # Apple's clang has them already through another header and compiled it
        # regardless, which is why this stood until a Linux runner said so. The
        # force-include says what is happening; a stub that quietly supplied
        # them would not.
        set(lkm_ptp_options "-include" "stdbool.h")

        # Only when the file is there. It used to be, for anybody who ran
        # `git submodule update --init`, because the module was this
        # repository's own submodule; reached through the daemon it needs
        # --recursive, and a source listed in add_executable that is not on
        # disk stops the configure of the whole tree rather than this suite.
        if(EXISTS ${lkm_driver}/PTP.c)
        add_executable(TestRavennaSlaveInterop
            Tests/TestRavennaSlaveInterop.cpp
            Tests/support/RavennaSlave.cpp
            ${lkm_driver}/PTP.c
        )
        target_include_directories(TestRavennaSlaveInterop PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests/support/lkm
            ${lkm_driver})
        if(AES67_SANITIZE)
            # The module's own byte swap shifts a signed int32 left by 8, which
            # is undefined once the top bits are set: PTP.c:474, on the
            # Follow_Up timestamp, and every timestamp of the last few decades
            # has them. That is upstream's code and upstream's defect. This
            # test compiles it to find out what it DECIDES about a packet, and
            # a sanitizer abort inside the vendor's arithmetic ends the test
            # without having said anything about this daemon.
            #
            # Reported, with the two-line fix, as
            # https://github.com/bondagit/ravenna-alsa-lkm/issues/40. When it
            # lands and the submodule moves, this goes: the fix was tried here
            # first, and the suite passes under both sanitizers without it.
            #
            # And the same file reads a clock identity out of a packet through
            # a uint64_t*: PTP.c:297, and five more like it. A PTP payload
            # starts 42 bytes into a frame, so those eight bytes are never
            # eight-byte aligned -- in an skb either, which is where the module
            # really reads them. GCC's UBSan on Linux says so; clang's on macOS
            # did not, which is why this one waited for a runner.
            #
            # Narrow on purpose: these two checks, on this one file. Everything
            # else in it stays instrumented, and so does every line of ours.
            list(APPEND lkm_ptp_options "-fno-sanitize=shift" "-fno-sanitize=alignment")
        endif()
        set_source_files_properties(${lkm_driver}/PTP.c PROPERTIES
            COMPILE_OPTIONS "${lkm_ptp_options}")
        target_link_libraries(TestRavennaSlaveInterop PRIVATE aes67_ptp_wire doctest_headers)
        add_test(NAME RavennaSlaveInterop COMMAND TestRavennaSlaveInterop)
        set_tests_properties(RavennaSlaveInterop PROPERTIES LABELS "unit" TIMEOUT 60)

        # The same module, fed what the daemon actually put on a socket rather
        # than what PtpWire built in memory. Only where the daemon exists to
        # be started, which is Linux: it is the thing under test.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_executable(TestGrandmasterOnTheWire
                Tests/TestGrandmasterOnTheWire.cpp
                Tests/support/RavennaSlave.cpp
                ${lkm_driver}/PTP.c
            )
            target_include_directories(TestGrandmasterOnTheWire PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/Tests
                ${CMAKE_CURRENT_SOURCE_DIR}/Tests/support/lkm
                ${lkm_driver})
            target_link_libraries(TestGrandmasterOnTheWire
                PRIVATE aes67_ptp_wire doctest_headers)
            add_dependencies(TestGrandmasterOnTheWire aes67-ptpd)
            add_test(NAME GrandmasterOnTheWire COMMAND TestGrandmasterOnTheWire)
            # network, so that the sanitizer and coverage runs -- which exclude
            # that label -- do not start a second daemon on the same group
            # while another build of this suite is doing the same.
            set_tests_properties(GrandmasterOnTheWire PROPERTIES
                LABELS "network" TIMEOUT 120)
        endif()
        else()
            message(STATUS
                "ravenna-alsa-lkm not checked out: RavennaSlaveInterop skipped "
                "(git submodule update --init --recursive)")
        endif()

    # The parsers, fed what nobody wrote a test for: Tests/fuzz/FuzzParsers.cpp.
    # Not a CTest test -- the gate runs it under AES67_ANALYSE=1, in the
    # sanitizer build, with an iteration count. -DAES67_LIBFUZZER=ON builds the
    # coverage-guided form instead, for a clang that ships the runtime.
    option(AES67_LIBFUZZER "Build the fuzz harness against libFuzzer" OFF)
    add_executable(FuzzPtpWire Tests/fuzz/FuzzParsers.cpp)
    target_link_libraries(FuzzPtpWire PRIVATE aes67_ptp_wire)
    if(AES67_LIBFUZZER)
        target_compile_definitions(FuzzPtpWire PRIVATE FUZZ_LIBFUZZER=1)
        target_compile_options(FuzzPtpWire PRIVATE -fsanitize=fuzzer)
        target_link_options(FuzzPtpWire PRIVATE -fsanitize=fuzzer)
    endif()

    else()
        message(WARNING "${AES67_DOCTEST_DIR} is empty - run: git submodule update --init --recursive")
    endif()
endif()
