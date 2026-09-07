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

# AES67 Interop Simulation: dry-run against the RAVENNA aes67-linux-daemon
add_executable(AES67InteropSim
    AES67InteropSim.cpp
    ../NetworkEngine/RTP/PCMCodec.cpp
)
target_include_directories(AES67InteropSim PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(AES67InteropSim PRIVATE aes67_net)

target_compile_features(AES67InteropSim PRIVATE cxx_std_17)
target_link_libraries(AES67InteropSim PRIVATE "-framework Accelerate")

# Dante Interop Simulation: dry-run against Dante Controller 4.18's own SAP
# and SDP handling, in both directions, under every profile.
add_executable(DanteInteropSim DanteInteropSim.cpp)
target_include_directories(DanteInteropSim PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(DanteInteropSim PRIVATE aes67_net)
target_compile_features(DanteInteropSim PRIVATE cxx_std_17)

# HAL Validate: Core Audio conformance check of the installed AudioServerPlugIn.
# Talks to coreaudiod through Apple's HAL client API only -- it links against
# no driver code, so it validates what is installed, not this build tree.
if(APPLE)
    add_executable(HALValidate HALValidate.cpp HALValidateAudioAccess.mm)
    target_compile_features(HALValidate PRIVATE cxx_std_17)
    target_link_libraries(HALValidate PRIVATE
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework AVFoundation"
    )
endif()

message(STATUS "Tools configured:")
message(STATUS "  - AES67TestSender")
message(STATUS "  - AES67TestReceiver")
message(STATUS "  - HALValidate")
message(STATUS "")
message(STATUS "Run test sender:")
message(STATUS "  ./Tools/AES67TestSender --ip 239.1.1.1 --port 5004 --channels 8 --freq 1000")
message(STATUS "Run test receiver:")
message(STATUS "  ./Tools/AES67TestReceiver --ip 239.1.1.2 --port 5004 --duration 10")
