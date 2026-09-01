# doctest, from the submodule this repository already carries. The core defines
# the same target name, but its tests are off here (see the root CMakeLists),
# so nothing collides.
add_library(doctest_headers INTERFACE)
target_include_directories(doctest_headers INTERFACE
    ${CMAKE_SOURCE_DIR}/external/doctest/doctest)
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/doctest/doctest/doctest.h")
    message(WARNING "external/doctest is empty - run: git submodule update --init --recursive")
endif()



# Test: Ring Buffer
add_executable(TestRingBuffer
    TestRingBuffer.cpp
)

target_include_directories(TestRingBuffer PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(TestRingBuffer PRIVATE aes67_core aes67_net)

target_link_libraries(TestRingBuffer PRIVATE doctest_headers)
target_compile_features(TestRingBuffer PRIVATE cxx_std_17)

add_executable(TestRingBufferTiming
    TestRingBufferTiming.cpp
)

target_include_directories(TestRingBufferTiming PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(TestRingBufferTiming PRIVATE aes67_core aes67_net)

target_link_libraries(TestRingBufferTiming PRIVATE doctest_headers)
target_compile_features(TestRingBufferTiming PRIVATE cxx_std_17)

# Test: RTP Receiver
add_executable(TestRTPReceiver
    TestRTPReceiver.cpp
    ../NetworkEngine/RTP/RTPReceiver.cpp
    ../NetworkEngine/RTP/PCMCodec.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../Driver/AudioThreadPriority.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
)

target_include_directories(TestRTPReceiver PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(TestRTPReceiver PRIVATE aes67_core aes67_net)

target_link_libraries(TestRTPReceiver PRIVATE doctest_headers)
target_compile_features(TestRTPReceiver PRIVATE cxx_std_17)

target_link_libraries(TestRTPReceiver PRIVATE "-framework Accelerate")

# Test: RTP Transmitter
add_executable(TestRTPTransmitter
    TestRTPTransmitter.cpp
    ../NetworkEngine/RTP/RTPTransmitter.cpp
    ../NetworkEngine/RTP/PCMCodec.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../Driver/AudioThreadPriority.cpp
)

target_include_directories(TestRTPTransmitter PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(TestRTPTransmitter PRIVATE aes67_core aes67_net)

target_link_libraries(TestRTPTransmitter PRIVATE doctest_headers)
target_compile_features(TestRTPTransmitter PRIVATE cxx_std_17)

target_link_libraries(TestRTPTransmitter PRIVATE "-framework Accelerate")

# Test: PTP Clock
add_executable(TestPTPClock
    TestPTPClock.cpp
    ../NetworkEngine/PTP/PTPClock.cpp
    ../NetworkEngine/PTP/PTPDInterface.cpp
    ../NetworkEngine/PTP/PTPSlave.cpp
    ../NetworkEngine/PTP/PTPMaster.cpp
    ../NetworkEngine/PTP/PTPArbitrator.cpp
    ../NetworkEngine/PTP/CoreAudioClockSource.cpp
    ../NetworkEngine/PTP/AudioClockDeviceList.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Driver/AudioThreadPriority.cpp
)

target_include_directories(TestPTPClock PRIVATE
    ${CMAKE_SOURCE_DIR}
)
# AudioClockDeviceList/CoreAudioClockSource (pulled in via PTPArbitrator)
# call CoreAudio/CoreFoundation directly.
target_link_libraries(TestPTPClock PRIVATE
    "-framework CoreAudio"
    "-framework CoreFoundation"
)

target_link_libraries(TestPTPClock PRIVATE aes67_core aes67_net)

target_link_libraries(TestPTPClock PRIVATE doctest_headers)
target_compile_features(TestPTPClock PRIVATE cxx_std_17)

# Test: PTP Master / BMCA — offline only, see TestPTPMaster.cpp's own header
# comment for why it doesn't touch sockets.
add_executable(TestPTPMaster
    TestPTPMaster.cpp
)

target_include_directories(TestPTPMaster PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(TestPTPMaster PRIVATE aes67_core aes67_net)

target_link_libraries(TestPTPMaster PRIVATE doctest_headers)
target_compile_features(TestPTPMaster PRIVATE cxx_std_17)

# Test: RTSP DESCRIBE endpoint — real loopback TCP, ephemeral port
# (2026-08-31).
add_executable(TestRTSPServer
    TestRTSPServer.cpp
)

target_include_directories(TestRTSPServer PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(TestRTSPServer PRIVATE aes67_core aes67_net)

target_link_libraries(TestRTSPServer PRIVATE doctest_headers)
target_compile_features(TestRTSPServer PRIVATE cxx_std_17)

# Test: mDNS browser — talks to the real system responder; skips itself
# where mDNS is unreachable (2026-08-31).
add_executable(TestMDNSBrowser
    TestMDNSBrowser.cpp
)

target_include_directories(TestMDNSBrowser PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(TestMDNSBrowser PRIVATE aes67_core aes67_net)

target_link_libraries(TestMDNSBrowser PRIVATE doctest_headers)
target_compile_features(TestMDNSBrowser PRIVATE cxx_std_17)

# Test: PTP loopback — real master and slave over lo0 multicast on
# unprivileged high ports; the only test that runs the full §11.3
# delay exchange end to end (2026-08-31).
add_executable(TestPTPLoopback
    TestPTPLoopback.cpp
    ../Driver/AudioThreadPriority.cpp
)

target_include_directories(TestPTPLoopback PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(TestPTPLoopback PRIVATE aes67_core aes67_net)

target_link_libraries(TestPTPLoopback PRIVATE doctest_headers)
target_compile_features(TestPTPLoopback PRIVATE cxx_std_17)

add_executable(TestPTPService
    TestPTPService.cpp
    ../NetworkEngine/PTP/PTPDInterface.cpp
    ../NetworkEngine/PTP/PTPArbitrator.cpp
    ../NetworkEngine/PTP/CoreAudioClockSource.cpp
    ../NetworkEngine/PTP/AudioClockDeviceList.cpp
    ../Driver/AudioThreadPriority.cpp
)

target_include_directories(TestPTPService PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(TestPTPService PRIVATE aes67_core aes67_net)
target_link_libraries(TestPTPService PRIVATE
    "-framework CoreAudio"
    "-framework CoreFoundation"
    "-framework AudioToolbox"
)

target_link_libraries(TestPTPService PRIVATE doctest_headers)
target_compile_features(TestPTPService PRIVATE cxx_std_17)

add_executable(TestStreamManager
    TestStreamManager.cpp
)

target_include_directories(TestStreamManager PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(TestStreamManager PRIVATE aes67_core aes67_net)

target_link_libraries(TestStreamManager PRIVATE doctest_headers)
target_compile_features(TestStreamManager PRIVATE cxx_std_17)

add_executable(TestMultiStream
    TestMultiStream.cpp
    ../NetworkEngine/PTP/PTPClock.cpp
    ../NetworkEngine/PTP/PTPDInterface.cpp
    ../NetworkEngine/PTP/PTPSlave.cpp
    ../NetworkEngine/PTP/PTPMaster.cpp
    ../NetworkEngine/PTP/PTPArbitrator.cpp
    ../NetworkEngine/PTP/CoreAudioClockSource.cpp
    ../NetworkEngine/PTP/AudioClockDeviceList.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Driver/AudioThreadPriority.cpp
)

target_include_directories(TestMultiStream PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(TestMultiStream PRIVATE
    "-framework CoreAudio"
    "-framework CoreFoundation"
)

target_link_libraries(TestMultiStream PRIVATE aes67_core aes67_net)

target_link_libraries(TestMultiStream PRIVATE doctest_headers)
target_compile_features(TestMultiStream PRIVATE cxx_std_17)

# Benchmark: I/O Handler Performance (requires libASPL)
if(ASPL_FOUND)
    add_executable(BenchmarkIOHandler
        BenchmarkIOHandler.cpp
        ../Driver/AES67IOHandler.cpp
        ${ASPL_SOURCES}
    )

    target_include_directories(BenchmarkIOHandler PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${ASPL_INCLUDE_DIR}
    )

    target_link_libraries(BenchmarkIOHandler PRIVATE aes67_core aes67_net)

    target_link_libraries(BenchmarkIOHandler PRIVATE doctest_headers)
target_compile_features(BenchmarkIOHandler PRIVATE cxx_std_17)

    # Link libASPL for benchmark (system install case; the submodule case
    # compiles ASPL_SOURCES directly into the target above instead)
    if(ASPL_LIBRARY)
        target_link_libraries(BenchmarkIOHandler PRIVATE ${ASPL_LIBRARY})
    endif()

    # aspl::Convert and aspl::Driver call into CoreFoundation/CoreAudio
    # directly (CFURL, CFUUID...); the AES67Driver target links these, this
    # one didn't.
    target_link_libraries(BenchmarkIOHandler PRIVATE
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework AudioToolbox"
    )
endif()

# Integration Test: Full Audio Path
add_executable(TestIntegrationAudioPath
    TestIntegrationAudioPath.cpp
    ../NetworkEngine/RTP/RTPReceiver.cpp
    ../NetworkEngine/RTP/RTPTransmitter.cpp
    ../NetworkEngine/RTP/PCMCodec.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../Driver/AudioThreadPriority.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
)

target_include_directories(TestIntegrationAudioPath PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(TestIntegrationAudioPath PRIVATE aes67_core aes67_net)

target_link_libraries(TestIntegrationAudioPath PRIVATE doctest_headers)
target_compile_features(TestIntegrationAudioPath PRIVATE cxx_std_17)

target_link_libraries(TestIntegrationAudioPath PRIVATE "-framework Accelerate")

# Test: PCM Codec (float<->L16/L24, interleave) — pins the DSP behaviour
add_executable(TestPCMCodec
    TestPCMCodec.cpp
    ../NetworkEngine/RTP/PCMCodec.cpp
)
target_include_directories(TestPCMCodec PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(TestPCMCodec PRIVATE aes67_core aes67_net)

target_link_libraries(TestPCMCodec PRIVATE doctest_headers)
target_compile_features(TestPCMCodec PRIVATE cxx_std_17)
target_link_libraries(TestPCMCodec PRIVATE "-framework Accelerate")

# Test: PTP peer table (passive Dolby-via-PTP discovery core) — header-only,
# socket-free, time-injected. See TestPTPPeerTable.cpp.
add_executable(TestPTPPeerTable
    TestPTPPeerTable.cpp
)
target_include_directories(TestPTPPeerTable PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(TestPTPPeerTable PRIVATE aes67_core aes67_net)

target_link_libraries(TestPTPPeerTable PRIVATE doctest_headers)
target_compile_features(TestPTPPeerTable PRIVATE cxx_std_17)

# Test: Dolby model catalog (model -> channels/direction + channel resolver)
add_executable(TestDolbyModelCatalog
    TestDolbyModelCatalog.cpp
)
target_include_directories(TestDolbyModelCatalog PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(TestDolbyModelCatalog PRIVATE aes67_core aes67_net)

target_link_libraries(TestDolbyModelCatalog PRIVATE doctest_headers)
target_compile_features(TestDolbyModelCatalog PRIVATE cxx_std_17)

add_executable(TestRTCPReceiverTable
    TestRTCPReceiverTable.cpp
)
target_include_directories(TestRTCPReceiverTable PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(TestRTCPReceiverTable PRIVATE aes67_core aes67_net)

target_link_libraries(TestRTCPReceiverTable PRIVATE doctest_headers)
target_compile_features(TestRTCPReceiverTable PRIVATE cxx_std_17)

# Add tests to CTest
enable_testing()
add_test(NAME RingBuffer COMMAND TestRingBuffer)
add_test(NAME RingBufferTiming COMMAND TestRingBufferTiming)
add_test(NAME RTPReceiver COMMAND TestRTPReceiver)
add_test(NAME RTPTransmitter COMMAND TestRTPTransmitter)
add_test(NAME PTPClock COMMAND TestPTPClock)
add_test(NAME PTPMaster COMMAND TestPTPMaster)
add_test(NAME PTPLoopback COMMAND TestPTPLoopback)
add_test(NAME PTPService COMMAND TestPTPService)
add_test(NAME MDNSBrowser COMMAND TestMDNSBrowser)
add_test(NAME RTSPServer COMMAND TestRTSPServer)
add_test(NAME StreamManager COMMAND TestStreamManager)
add_test(NAME PCMCodec COMMAND TestPCMCodec)
add_test(NAME PTPPeerTable COMMAND TestPTPPeerTable)
add_test(NAME DolbyModelCatalog COMMAND TestDolbyModelCatalog)
add_test(NAME RTCPReceiverTable COMMAND TestRTCPReceiverTable)
add_test(NAME MultiStream COMMAND TestMultiStream)
add_test(NAME IntegrationAudioPath COMMAND TestIntegrationAudioPath)

# Labels drive what the local gate runs. scripts/ci-local.sh excludes the
# `timing` label rather than naming suites in a regex, so a new test that
# depends on wall-clock behaviour is excluded the
# moment it is labelled, without anyone remembering to edit the gate.
#
# `network` is separate from `timing` on purpose: the sanitizer runs want the
# concurrent and wall-clock suites -- that is where a race would show -- but
# still cannot run anything that needs real multicast sockets on a developer
# machine.
#
# Every test also gets a TIMEOUT. The gate runs from a pre-push hook, so a
# hung test blocks the push forever with no output; the slowest suite today
# is IntegrationAudioPath at ~2.2s, so 60s is generous by more than an order
# of magnitude and still bounded.
set_tests_properties(RingBuffer PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(RingBufferTiming PROPERTIES LABELS "timing" TIMEOUT 60)
set_tests_properties(RTPReceiver PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(RTPTransmitter PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(PTPClock PROPERTIES LABELS "unit;timing" TIMEOUT 60)
set_tests_properties(PTPMaster PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(StreamManager PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(PCMCodec PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(PTPPeerTable PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(DolbyModelCatalog PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(RTCPReceiverTable PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(MultiStream PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(IntegrationAudioPath PROPERTIES LABELS "integration;timing;network" TIMEOUT 60)
set_tests_properties(PTPLoopback PROPERTIES LABELS "integration;timing;network" TIMEOUT 90)
# Wall-clock waits (staleness has to actually elapse), no network.
set_tests_properties(PTPService PROPERTIES LABELS "integration;timing" TIMEOUT 60)
set_tests_properties(MDNSBrowser PROPERTIES LABELS "integration;network" TIMEOUT 60)
set_tests_properties(RTSPServer PROPERTIES LABELS "integration;network" TIMEOUT 60)

message(STATUS "Tests configured:")
message(STATUS "  - TestSDPParser")
message(STATUS "  - TestChannelMapper")
message(STATUS "  - TestRingBuffer")
message(STATUS "  - TestRTPReceiver")
message(STATUS "  - TestRTPTransmitter")
message(STATUS "  - TestPTPClock")
message(STATUS "  - TestPTPMaster")
message(STATUS "  - TestCompatibilityProfile")
message(STATUS "  - TestStreamManager")
message(STATUS "  - TestResampling (NEW)")
message(STATUS "  - TestStreamConfig (NEW)")
message(STATUS "  - TestMultiStream (Integration)")
message(STATUS "  - TestIntegrationAudioPath (Integration)")
message(STATUS "")
message(STATUS "Benchmarks configured:")
message(STATUS "  - BenchmarkIOHandler")
message(STATUS "")
message(STATUS "Run tests with: ctest or make test")
message(STATUS "Run benchmarks manually: ./BenchmarkIOHandler")
