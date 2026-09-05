# Examples CMakeLists.txt
# AES67 macOS Driver - Build #4
# Example programs compilation
#
# These used to list core translation units by hand. They live in
# aes67-core now, so the examples link the library instead -- and this file
# was where the move showed up last, because the gate builds with
# -DBUILD_EXAMPLES=OFF and never configured it.

cmake_minimum_required(VERSION 3.15)

# Example: Simple SDP Parser
add_executable(SimpleSDPParse
    SimpleSDPParse.cpp
)

target_include_directories(SimpleSDPParse PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(SimpleSDPParse PRIVATE aes67_core)

target_compile_features(SimpleSDPParse PRIVATE cxx_std_17)

# Example: Channel Mapper Demo
add_executable(ChannelMapperDemo
    ChannelMapperDemo.cpp
)

target_include_directories(ChannelMapperDemo PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(ChannelMapperDemo PRIVATE aes67_core)

target_compile_features(ChannelMapperDemo PRIVATE cxx_std_17)

message(STATUS "Examples configured:")
message(STATUS "  - SimpleSDPParse")
message(STATUS "  - ChannelMapperDemo")
message(STATUS "")
message(STATUS "Run examples:")
message(STATUS "  ./SimpleSDPParse Docs/Examples/riedel_artist_8ch.sdp")
message(STATUS "  ./ChannelMapperDemo")
