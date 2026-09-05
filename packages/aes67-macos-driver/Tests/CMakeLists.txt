# doctest lives once, at the root of the monorepo, and the root CMakeLists
# defines the target -- the core needs the same one, and two packages
# defining a target under the same name is an error. This branch is what
# makes the package still configure on its own, outside the tree.
if(NOT DEFINED AES67_DOCTEST_DIR)
    set(AES67_DOCTEST_DIR "${PROJECT_SOURCE_DIR}/../../external/doctest/doctest")
endif()
if(NOT EXISTS "${AES67_DOCTEST_DIR}/doctest.h")
    message(WARNING "${AES67_DOCTEST_DIR} is empty - run: git submodule update --init --recursive")
endif()
if(NOT TARGET doctest_headers)
    add_library(doctest_headers INTERFACE)
    target_include_directories(doctest_headers INTERFACE ${AES67_DOCTEST_DIR})
endif()



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
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestRTPReceiver PRIVATE aes67_net)

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
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestRTPTransmitter PRIVATE aes67_net)

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
    ${PROJECT_SOURCE_DIR}
)
# AudioClockDeviceList/CoreAudioClockSource (pulled in via PTPArbitrator)
# call CoreAudio/CoreFoundation directly.
target_link_libraries(TestPTPClock PRIVATE
    "-framework CoreAudio"
    "-framework CoreFoundation"
)

target_link_libraries(TestPTPClock PRIVATE aes67_net)

target_link_libraries(TestPTPClock PRIVATE doctest_headers)
target_compile_features(TestPTPClock PRIVATE cxx_std_17)

# Test: PTP Master / BMCA — offline only, see TestPTPMaster.cpp's own header
# comment for why it doesn't touch sockets.
add_executable(TestPTPMaster
    TestPTPMaster.cpp
)

target_include_directories(TestPTPMaster PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestPTPMaster PRIVATE aes67_net)

target_link_libraries(TestPTPMaster PRIVATE doctest_headers)
target_compile_features(TestPTPMaster PRIVATE cxx_std_17)

# Test: RTSP DESCRIBE endpoint — real loopback TCP, ephemeral port
# (2026-08-31).
add_executable(TestRTSPServer
    TestRTSPServer.cpp
)

target_include_directories(TestRTSPServer PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestRTSPServer PRIVATE aes67_net)

target_link_libraries(TestRTSPServer PRIVATE doctest_headers)
target_compile_features(TestRTSPServer PRIVATE cxx_std_17)

# Test: SAP announcement parsing, against datagrams built byte by byte.
add_executable(TestSAPListener
    TestSAPListener.cpp
)

target_include_directories(TestSAPListener PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestSAPListener PRIVATE aes67_net)

target_link_libraries(TestSAPListener PRIVATE doctest_headers)
target_compile_features(TestSAPListener PRIVATE cxx_std_17)

# Test: what the RTCP monitor records from a packet, without a socket.
add_executable(TestRTCPMonitor
    TestRTCPMonitor.cpp
)

target_include_directories(TestRTCPMonitor PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestRTCPMonitor PRIVATE aes67_net)

target_link_libraries(TestRTCPMonitor PRIVATE doctest_headers)
target_compile_features(TestRTCPMonitor PRIVATE cxx_std_17)

# Test: the HTTP client, against a server made of a loopback socket.
add_executable(TestHTTPClient
    TestHTTPClient.cpp
)

target_include_directories(TestHTTPClient PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestHTTPClient PRIVATE aes67_net)

target_link_libraries(TestHTTPClient PRIVATE doctest_headers)
target_compile_features(TestHTTPClient PRIVATE cxx_std_17)

# Test: RTSP URL and response parsing, without a server.
add_executable(TestRTSPClient
    TestRTSPClient.cpp
)

target_include_directories(TestRTSPClient PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestRTSPClient PRIVATE aes67_net)

target_link_libraries(TestRTSPClient PRIVATE doctest_headers)
target_compile_features(TestRTSPClient PRIVATE cxx_std_17)

# Test: SAP announcements built here, read back by the listener's parser.
add_executable(TestSAPAnnouncer
    TestSAPAnnouncer.cpp
)

target_include_directories(TestSAPAnnouncer PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestSAPAnnouncer PRIVATE aes67_net)

target_link_libraries(TestSAPAnnouncer PRIVATE doctest_headers)
target_compile_features(TestSAPAnnouncer PRIVATE cxx_std_17)

# Test: the socket-layer helpers this repository supplies to the core.
add_executable(TestNetworkUtils
    TestNetworkUtils.cpp
)

target_include_directories(TestNetworkUtils PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestNetworkUtils PRIVATE aes67_net)

target_link_libraries(TestNetworkUtils PRIVATE doctest_headers)
target_compile_features(TestNetworkUtils PRIVATE cxx_std_17)

# Test: passive PTP peer discovery, from messages handed in directly.
add_executable(TestPTPPeerObserver
    TestPTPPeerObserver.cpp
)

target_include_directories(TestPTPPeerObserver PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestPTPPeerObserver PRIVATE aes67_net)

target_link_libraries(TestPTPPeerObserver PRIVATE doctest_headers)
target_compile_features(TestPTPPeerObserver PRIVATE cxx_std_17)

# Test: interface selection, against whatever interfaces this machine has.
add_executable(TestNetworkInterfaceDetection
    TestNetworkInterfaceDetection.cpp
)

target_include_directories(TestNetworkInterfaceDetection PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestNetworkInterfaceDetection PRIVATE aes67_net)

target_link_libraries(TestNetworkInterfaceDetection PRIVATE doctest_headers)
target_compile_features(TestNetworkInterfaceDetection PRIVATE cxx_std_17)

# Test: teardown plumbing — the cleanup manager and the RAII guards.
add_executable(TestResourceCleanup
    TestResourceCleanup.cpp
)

target_include_directories(TestResourceCleanup PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestResourceCleanup PRIVATE aes67_net)

target_link_libraries(TestResourceCleanup PRIVATE doctest_headers)
target_compile_features(TestResourceCleanup PRIVATE cxx_std_17)

# Test: multicast memberships kept alive, against loopback sockets.
add_executable(TestMulticastRejoiner
    TestMulticastRejoiner.cpp
)

target_include_directories(TestMulticastRejoiner PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestMulticastRejoiner PRIVATE aes67_net)

target_link_libraries(TestMulticastRejoiner PRIVATE doctest_headers)
target_compile_features(TestMulticastRejoiner PRIVATE cxx_std_17)

# Test: the IS-05 Connection API, routed directly and over a socket.
add_executable(TestConnectionAPI
    TestConnectionAPI.cpp
)

target_include_directories(TestConnectionAPI PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestConnectionAPI PRIVATE aes67_net)

target_link_libraries(TestConnectionAPI PRIVATE doctest_headers)
target_compile_features(TestConnectionAPI PRIVATE cxx_std_17)

# Test: registering as an IS-04 node, against a registry made of a socket.
add_executable(TestNMOSRegistration
    TestNMOSRegistration.cpp
)

target_include_directories(TestNMOSRegistration PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestNMOSRegistration PRIVATE aes67_net)

target_link_libraries(TestNMOSRegistration PRIVATE doctest_headers)
target_compile_features(TestNMOSRegistration PRIVATE cxx_std_17)

# Test: this slave against the bytes the AES67-MasterBox puts on the wire,
# replayed through PTPSlave::deliverMessage. No sockets: see the file's head.
add_executable(TestPTPMasterBoxInterop
    TestPTPMasterBoxInterop.cpp
)

target_include_directories(TestPTPMasterBoxInterop PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestPTPMasterBoxInterop PRIVATE aes67_net)

target_link_libraries(TestPTPMasterBoxInterop PRIVATE doctest_headers)
target_compile_features(TestPTPMasterBoxInterop PRIVATE cxx_std_17)

# Test: the configured PTP dataset reaching the engines that send it.
add_executable(TestPTPSettingsMapping
    TestPTPSettingsMapping.cpp
)

target_include_directories(TestPTPSettingsMapping PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestPTPSettingsMapping PRIVATE aes67_net)

target_link_libraries(TestPTPSettingsMapping PRIVATE doctest_headers)
target_compile_features(TestPTPSettingsMapping PRIVATE cxx_std_17)

# Test: the DSCP marking, read back off a real socket.
add_executable(TestNetworkQoS
    TestNetworkQoS.cpp
)

target_include_directories(TestNetworkQoS PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestNetworkQoS PRIVATE aes67_net)

target_link_libraries(TestNetworkQoS PRIVATE doctest_headers)
target_compile_features(TestNetworkQoS PRIVATE cxx_std_17)

# Test: fetching a session description from a file, an HTTP server or an
# RTSP DESCRIBE, over real loopback sockets.
add_executable(TestSDPFetcher
    TestSDPFetcher.cpp
)

target_include_directories(TestSDPFetcher PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestSDPFetcher PRIVATE aes67_net)

target_link_libraries(TestSDPFetcher PRIVATE doctest_headers)
target_compile_features(TestSDPFetcher PRIVATE cxx_std_17)

# Test: mDNS browser — talks to the real system responder; skips itself
# where mDNS is unreachable (2026-08-31).
add_executable(TestMDNSBrowser
    TestMDNSBrowser.cpp
)

target_include_directories(TestMDNSBrowser PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestMDNSBrowser PRIVATE aes67_net)

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
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestPTPLoopback PRIVATE aes67_net)

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
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestPTPService PRIVATE aes67_net)
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
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestStreamManager PRIVATE aes67_net)

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
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestMultiStream PRIVATE
    "-framework CoreAudio"
    "-framework CoreFoundation"
)

target_link_libraries(TestMultiStream PRIVATE aes67_net)

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
        ${PROJECT_SOURCE_DIR}
    )
    # Same as the driver bundle: vendored headers as SYSTEM, vendored sources
    # compiled with warnings off, so what is left is ours.
    target_include_directories(BenchmarkIOHandler SYSTEM PRIVATE ${ASPL_INCLUDE_DIR})
    set_source_files_properties(${ASPL_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

    target_link_libraries(BenchmarkIOHandler PRIVATE aes67_net)

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
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(TestIntegrationAudioPath PRIVATE aes67_net)

target_link_libraries(TestIntegrationAudioPath PRIVATE doctest_headers)
target_compile_features(TestIntegrationAudioPath PRIVATE cxx_std_17)

target_link_libraries(TestIntegrationAudioPath PRIVATE "-framework Accelerate")

# Test: PCM Codec (float<->L16/L24, interleave) — pins the DSP behaviour
add_executable(TestPCMCodec
    TestPCMCodec.cpp
    ../NetworkEngine/RTP/PCMCodec.cpp
)
target_include_directories(TestPCMCodec PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(TestPCMCodec PRIVATE aes67_net)

target_link_libraries(TestPCMCodec PRIVATE doctest_headers)
target_compile_features(TestPCMCodec PRIVATE cxx_std_17)
target_link_libraries(TestPCMCodec PRIVATE "-framework Accelerate")


# Add tests to CTest
enable_testing()
add_test(NAME RTPReceiver COMMAND TestRTPReceiver)
add_test(NAME RTPTransmitter COMMAND TestRTPTransmitter)
add_test(NAME PTPClock COMMAND TestPTPClock)
add_test(NAME PTPMaster COMMAND TestPTPMaster)
add_test(NAME PTPLoopback COMMAND TestPTPLoopback)
add_test(NAME PTPService COMMAND TestPTPService)
add_test(NAME MDNSBrowser COMMAND TestMDNSBrowser)
add_test(NAME RTSPServer COMMAND TestRTSPServer)
add_test(NAME SDPFetcher COMMAND TestSDPFetcher)
add_test(NAME NetworkQoS COMMAND TestNetworkQoS)
add_test(NAME PTPSettingsMapping COMMAND TestPTPSettingsMapping)
add_test(NAME PTPMasterBoxInterop COMMAND TestPTPMasterBoxInterop)
add_test(NAME NMOSRegistration COMMAND TestNMOSRegistration)
add_test(NAME ConnectionAPI COMMAND TestConnectionAPI)
add_test(NAME SAPListener COMMAND TestSAPListener)
add_test(NAME RTCPMonitor COMMAND TestRTCPMonitor)
add_test(NAME HTTPClient COMMAND TestHTTPClient)
add_test(NAME RTSPClient COMMAND TestRTSPClient)
add_test(NAME SAPAnnouncer COMMAND TestSAPAnnouncer)
add_test(NAME NetworkUtils COMMAND TestNetworkUtils)
add_test(NAME PTPPeerObserver COMMAND TestPTPPeerObserver)
add_test(NAME NetworkInterfaceDetection COMMAND TestNetworkInterfaceDetection)
add_test(NAME ResourceCleanup COMMAND TestResourceCleanup)
add_test(NAME MulticastRejoiner COMMAND TestMulticastRejoiner)
add_test(NAME StreamManager COMMAND TestStreamManager)
add_test(NAME PCMCodec COMMAND TestPCMCodec)
add_test(NAME MultiStream COMMAND TestMultiStream)
add_test(NAME IntegrationAudioPath COMMAND TestIntegrationAudioPath)

# Labels drive what the local gate runs. scripts/gate.sh excludes the
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
set_tests_properties(RTPReceiver PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(RTPTransmitter PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(PTPClock PROPERTIES LABELS "unit;timing" TIMEOUT 60)
set_tests_properties(PTPMaster PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(StreamManager PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(PCMCodec PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(SDPFetcher PROPERTIES LABELS "unit;network" TIMEOUT 60)
set_tests_properties(NetworkQoS PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(PTPSettingsMapping PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(PTPMasterBoxInterop PROPERTIES LABELS "unit;interop" TIMEOUT 60)
set_tests_properties(NMOSRegistration PROPERTIES LABELS "unit;network" TIMEOUT 60)
set_tests_properties(ConnectionAPI PROPERTIES LABELS "unit;network" TIMEOUT 60)
set_tests_properties(SAPListener PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(RTCPMonitor PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(HTTPClient PROPERTIES LABELS "unit;network" TIMEOUT 60)
set_tests_properties(RTSPClient PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(SAPAnnouncer PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(NetworkUtils PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(PTPPeerObserver PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(NetworkInterfaceDetection PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(ResourceCleanup PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(MulticastRejoiner PROPERTIES LABELS "unit;network" TIMEOUT 60)
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
