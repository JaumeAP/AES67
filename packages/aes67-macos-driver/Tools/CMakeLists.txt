# Tools CMakeLists.txt
# AES67 macOS Driver - Test and debugging tools

cmake_minimum_required(VERSION 3.15)

# AES67 Test Sender: generates sine wave RTP multicast stream with SAP
add_executable(AES67TestSender
    AES67TestSender.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../NetworkEngine/RTP/PCMCodec.cpp
    ../NetworkEngine/NetworkUtils.cpp
)

target_include_directories(AES67TestSender PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(AES67TestSender PRIVATE aes67_net)

target_compile_features(AES67TestSender PRIVATE cxx_std_17)
target_link_libraries(AES67TestSender PRIVATE "-framework Accelerate")

# AES67 Test Receiver: joins multicast group and reports received RTP packets
add_executable(AES67TestReceiver
    AES67TestReceiver.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../NetworkEngine/RTP/PCMCodec.cpp
    ../NetworkEngine/NetworkUtils.cpp
)

target_include_directories(AES67TestReceiver PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(AES67TestReceiver PRIVATE aes67_net)

target_compile_features(AES67TestReceiver PRIVATE cxx_std_17)
target_link_libraries(AES67TestReceiver PRIVATE "-framework Accelerate")

# AES67 Interop Simulation: dry-run against the RAVENNA aes67-linux-daemon.
# Tests/support/DaemonSap.cpp is the daemon's own send and receive path,
# mirrored from its source; this tool reports on that oracle rather than
# carrying a second copy of the daemon's rules, which is why it compiles a
# Tests file and needs Tests/ on its include path for "support/DaemonSap.h".
add_executable(AES67InteropSim
    AES67InteropSim.cpp
    ../Tests/support/DaemonSap.cpp
)
target_include_directories(AES67InteropSim PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../Tests
)
target_link_libraries(AES67InteropSim PRIVATE aes67_net)

target_compile_features(AES67InteropSim PRIVATE cxx_std_17)

# Dante Interop Simulation: dry-run against Dante Controller 4.18's own SAP
# and SDP handling, in both directions, under every profile.
add_executable(DanteInteropSim DanteInteropSim.cpp)
target_include_directories(DanteInteropSim PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(DanteInteropSim PRIVATE aes67_net)
target_compile_features(DanteInteropSim PRIVATE cxx_std_17)

# The impairment relay moved to Eines/packages/aoip-stress-lab on 2026-09-16,
# as its `impair` subcommand: it forwards datagrams without reading them, so
# nothing in it was ever about this driver, and the lab already owns the
# vendor-neutral half of this work. Its policy went with it, and so did the
# suite that pinned it.

# AES67 SAP Monitor: hears SAP announcements and says what they describe, so
# that what the announcers emit can be checked from outside them.
add_executable(AES67SAPMonitor AES67SAPMonitor.cpp)
target_include_directories(AES67SAPMonitor PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(AES67SAPMonitor PRIVATE aes67_net)
target_compile_features(AES67SAPMonitor PRIVATE cxx_std_17)

# AES67 Live Daemon Interop: the same SAP/RTSP surface AES67InteropSim dry-
# runs, driven against a REAL aes67-linux-daemon process instead -- built and
# started by a Linux CI job (this driver's own AudioServerPlugIn is macOS-
# only, but aes67_net is not, and the daemon does not build on macOS at all).
# No Apple framework, same as AES67SAPMonitor above.
add_executable(AES67LiveDaemonInterop AES67LiveDaemonInterop.cpp)
target_include_directories(AES67LiveDaemonInterop PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(AES67LiveDaemonInterop PRIVATE aes67_net_portable)
target_compile_features(AES67LiveDaemonInterop PRIVATE cxx_std_17)

# AES67 PTP Stress Run: this driver's real PTPMaster and PTPSlave against
# each other over a live interface for a sustained period, sampling lock
# state/offset/path delay every second -- TestPTPLoopback only proves the
# exchange completes and checks the state once, at the end of ~8 seconds.
# Needs AudioThreadPriority (PTPMaster's transmit/receive threads call it),
# which lives in Driver/ and is not part of aes67_net.
add_executable(AES67PTPStressRun
    AES67PTPStressRun.cpp
    ../Driver/AudioThreadPriority.cpp
)
target_include_directories(AES67PTPStressRun PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(AES67PTPStressRun PRIVATE aes67_net)
target_compile_features(AES67PTPStressRun PRIVATE cxx_std_17)

# HALValidate and QuickCapture moved to
# Eines/packages/macos-audio-development/tools on 2026-09-16. Neither links any
# driver code -- they talk to whatever coreaudiod has loaded, through Apple's
# own client API -- so they check an installed plug-in rather than this tree,
# and that is not a thing this repository should own.

message(STATUS "Tools configured:")
message(STATUS "  - AES67TestSender")
message(STATUS "  - AES67TestReceiver")
message(STATUS "  - AES67SAPMonitor")
message(STATUS "  - AES67LiveDaemonInterop")
message(STATUS "  - AES67PTPStressRun")
message(STATUS "")
message(STATUS "Run test sender:")
message(STATUS "  ./Tools/AES67TestSender --ip 239.1.1.1 --port 5004 --channels 8 --freq 1000")
message(STATUS "Run test receiver:")
message(STATUS "  ./Tools/AES67TestReceiver --ip 239.1.1.2 --port 5004 --duration 10")
