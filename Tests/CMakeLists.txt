


# Test: RTCP receiver-report detection core
add_executable(TestRTCPReceiverTable
    TestRTCPReceiverTable.cpp
)
target_include_directories(TestRTCPReceiverTable PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(TestRTCPReceiverTable PRIVATE aes67_core aes67_net)

target_compile_features(TestRTCPReceiverTable PRIVATE cxx_std_17)

# Add tests to CTest
enable_testing()
add_test(NAME RingBuffer COMMAND TestRingBuffer)
add_test(NAME RingBufferTiming COMMAND TestRingBufferTiming)
add_test(NAME RTPReceiver COMMAND TestRTPReceiver)
add_test(NAME RTPTransmitter COMMAND TestRTPTransmitter)
add_test(NAME PTPClock COMMAND TestPTPClock)
add_test(NAME PTPMaster COMMAND TestPTPMaster)
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
