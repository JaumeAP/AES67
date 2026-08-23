# Tests CMakeLists.txt
# AES67 macOS Driver - Build #4
# Test suite compilation

cmake_minimum_required(VERSION 3.15)

# Test: SDP Parser
add_executable(TestSDPParser
    TestSDPParser.cpp
    ../Driver/SDPParser.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestSDPParser PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestSDPParser PRIVATE cxx_std_17)

# Test: Channel Mapper
add_executable(TestChannelMapper
    TestChannelMapper.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestChannelMapper PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestChannelMapper PRIVATE cxx_std_17)

# Test: Ring Buffer
add_executable(TestRingBuffer
    TestRingBuffer.cpp
)

target_include_directories(TestRingBuffer PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestRingBuffer PRIVATE cxx_std_17)

# Test: RTP Receiver
add_executable(TestRTPReceiver
    TestRTPReceiver.cpp
    ../NetworkEngine/RTP/RTPReceiver.cpp
    ../NetworkEngine/RTP/PCMCodec.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../NetworkEngine/NetworkUtils.cpp
    ../NetworkEngine/RTP/LockFreeCircularJitterBuffer.cpp
    ../NetworkEngine/RTP/LockFreePacketPool.cpp
    ../Driver/SDPParser.cpp
    ../Driver/AudioThreadPriority.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestRTPReceiver PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestRTPReceiver PRIVATE cxx_std_17)

target_link_libraries(TestRTPReceiver PRIVATE "-framework Accelerate")

# Test: RTP Transmitter
add_executable(TestRTPTransmitter
    TestRTPTransmitter.cpp
    ../NetworkEngine/RTP/RTPTransmitter.cpp
    ../NetworkEngine/RTP/PCMCodec.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../NetworkEngine/NetworkUtils.cpp
    ../Driver/SDPParser.cpp
    ../Driver/AudioThreadPriority.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestRTPTransmitter PRIVATE
    ${CMAKE_SOURCE_DIR}
)

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
    ../NetworkEngine/PTP/PTPMasterSettings.cpp
    ../NetworkEngine/PTP/PhaseLockedLoop.cpp
    ../NetworkEngine/PTP/CustomSys.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Driver/SDPParser.cpp
    ../Driver/AudioThreadPriority.cpp
    ../Shared/Types.cpp
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

target_compile_features(TestPTPClock PRIVATE cxx_std_17)

# Test: PTP Master / BMCA — offline only, see TestPTPMaster.cpp's own header
# comment for why it doesn't touch sockets.
add_executable(TestPTPMaster
    TestPTPMaster.cpp
)

target_include_directories(TestPTPMaster PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestPTPMaster PRIVATE cxx_std_17)

# Test: Compatibility profiles — pure constraint validation, no sockets.
add_executable(TestCompatibilityProfile
    TestCompatibilityProfile.cpp
    ../NetworkEngine/CompatibilityProfile.cpp
    ../Driver/SDPParser.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestCompatibilityProfile PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestCompatibilityProfile PRIVATE cxx_std_17)

# Test: Stream Manager
add_executable(TestStreamManager
    TestStreamManager.cpp
    ../Driver/SDPParser.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestStreamManager PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestStreamManager PRIVATE cxx_std_17)

# Test: Resampling Subsystem
add_executable(TestResampling
    TestResampling.cpp
    ../NetworkEngine/Resampling/PIController.cpp
    ../NetworkEngine/Resampling/SmoothedPIController.cpp
    ../NetworkEngine/Resampling/Resampler.cpp
    ../NetworkEngine/Resampling/SampleRateAdapter.cpp
)

target_include_directories(TestResampling PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestResampling PRIVATE cxx_std_17)

# Test: Stream Config
add_executable(TestStreamConfig
    TestStreamConfig.cpp
    ../NetworkEngine/StreamConfig.cpp
    ../NetworkEngine/NetworkUtils.cpp
    ../Driver/SDPParser.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestStreamConfig PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestStreamConfig PRIVATE cxx_std_17)

# Integration Test: Multi-Stream Scenarios
add_executable(TestMultiStream
    TestMultiStream.cpp
    ../Driver/SDPParser.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../NetworkEngine/PTP/PTPClock.cpp
    ../NetworkEngine/PTP/PTPDInterface.cpp
    ../NetworkEngine/PTP/PTPSlave.cpp
    ../NetworkEngine/PTP/PTPMaster.cpp
    ../NetworkEngine/PTP/PTPArbitrator.cpp
    ../NetworkEngine/PTP/CoreAudioClockSource.cpp
    ../NetworkEngine/PTP/AudioClockDeviceList.cpp
    ../NetworkEngine/PTP/PTPMasterSettings.cpp
    ../NetworkEngine/PTP/PhaseLockedLoop.cpp
    ../NetworkEngine/PTP/CustomSys.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Driver/AudioThreadPriority.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestMultiStream PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(TestMultiStream PRIVATE
    "-framework CoreAudio"
    "-framework CoreFoundation"
)

target_compile_features(TestMultiStream PRIVATE cxx_std_17)

# Benchmark: I/O Handler Performance (requires libASPL)
if(ASPL_FOUND)
    add_executable(BenchmarkIOHandler
        BenchmarkIOHandler.cpp
        ../Driver/AES67IOHandler.cpp
        ../Shared/Types.cpp
        ${ASPL_SOURCES}
    )

    target_include_directories(BenchmarkIOHandler PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${ASPL_INCLUDE_DIR}
    )

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
    ../NetworkEngine/NetworkUtils.cpp
    ../NetworkEngine/RTP/LockFreeCircularJitterBuffer.cpp
    ../NetworkEngine/RTP/LockFreePacketPool.cpp
    ../Driver/SDPParser.cpp
    ../Driver/AudioThreadPriority.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestIntegrationAudioPath PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestIntegrationAudioPath PRIVATE cxx_std_17)

target_link_libraries(TestIntegrationAudioPath PRIVATE "-framework Accelerate")

# Test: PCM Codec (float<->L16/L24, interleave) — pins the DSP behaviour
add_executable(TestPCMCodec
    TestPCMCodec.cpp
    ../NetworkEngine/RTP/PCMCodec.cpp
)
target_include_directories(TestPCMCodec PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(TestPCMCodec PRIVATE cxx_std_17)
target_link_libraries(TestPCMCodec PRIVATE "-framework Accelerate")

# Add tests to CTest
enable_testing()
add_test(NAME SDPParser COMMAND TestSDPParser)
add_test(NAME ChannelMapper COMMAND TestChannelMapper)
add_test(NAME RingBuffer COMMAND TestRingBuffer)
add_test(NAME RTPReceiver COMMAND TestRTPReceiver)
add_test(NAME RTPTransmitter COMMAND TestRTPTransmitter)
add_test(NAME PTPClock COMMAND TestPTPClock)
add_test(NAME PTPMaster COMMAND TestPTPMaster)
add_test(NAME CompatibilityProfile COMMAND TestCompatibilityProfile)
add_test(NAME StreamManager COMMAND TestStreamManager)
add_test(NAME Resampling COMMAND TestResampling)
add_test(NAME PCMCodec COMMAND TestPCMCodec)
add_test(NAME StreamConfig COMMAND TestStreamConfig)
add_test(NAME MultiStream COMMAND TestMultiStream)
add_test(NAME IntegrationAudioPath COMMAND TestIntegrationAudioPath)

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
