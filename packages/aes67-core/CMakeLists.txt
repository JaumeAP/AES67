cmake_minimum_required(VERSION 3.20)
project(aes67_core LANGUAGES CXX)

# Platform-free by construction, and checked: see scripts/check-platform-free.sh
# and the README. Nothing here may include an Apple framework, a socket header
# or anything else that assumes an operating system -- that is the whole reason
# this library was split out of aes67-macos-driver rather than being consumed
# from inside it, where "the base" would have meant "the macOS one".
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(aes67_core
    Shared/Types.cpp
    Shared/Config.cpp
    Shared/NonBlockingLogger.cpp
    Shared/ErrorRecoveryManager.cpp
    Driver/SDPParser.cpp
    NetworkEngine/StreamChannelMapper.cpp
    NetworkEngine/StreamConfig.cpp
    NetworkEngine/DeviceChannelSettings.cpp
    NetworkEngine/AmplifierUnitSettings.cpp
    NetworkEngine/CompatibilityProfile.cpp
    NetworkEngine/NetworkErrorHandler.cpp
    NetworkEngine/DoPDecoder.cpp
    NetworkEngine/RTP/LockFreeCircularJitterBuffer.cpp
    NetworkEngine/RTP/LockFreePacketPool.cpp
    NetworkEngine/RTP/BufferStatusMonitor.cpp
    NetworkEngine/NMOSSettings.cpp
    NetworkEngine/PTP/PTPMasterSettings.cpp
    NetworkEngine/PTP/PhaseLockedLoop.cpp
    NetworkEngine/PTP/CustomSys.cpp
    NetworkEngine/Resampling/Resampler.cpp
    NetworkEngine/Resampling/PIController.cpp
    NetworkEngine/Resampling/SmoothedPIController.cpp
    NetworkEngine/Resampling/ClockAdjustmentController.cpp
    NetworkEngine/Resampling/SampleRateAdapter.cpp
)

target_include_directories(aes67_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(aes67_core PRIVATE -Wall -Wextra -Wpedantic
    -Wno-unused-parameter -Wno-missing-field-initializers)

option(AES67_CORE_TESTS "Build the test suites" ON)
if(AES67_CORE_TESTS)
    enable_testing()

    # doctest lives once, at the root of the monorepo, and the root
    # CMakeLists defines the target. This branch is what makes the package
    # still configure on its own, outside the tree.
    if(NOT DEFINED AES67_DOCTEST_DIR)
        set(AES67_DOCTEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/doctest/doctest")
    endif()
    if(NOT EXISTS "${AES67_DOCTEST_DIR}/doctest.h")
        message(WARNING "${AES67_DOCTEST_DIR} is empty - run: git submodule update --init --recursive")
    endif()
    if(NOT TARGET doctest_headers)
        add_library(doctest_headers INTERFACE)
        target_include_directories(doctest_headers INTERFACE ${AES67_DOCTEST_DIR})
    endif()

    # doctest suites: written here, or migrated as their code moved.
    # StreamConfig is the one suite that reaches the NetworkUtils seam -- see
    # the README -- so it, and only it, links the test double that fills it.
    set(AES67_SEAM_STUB Tests/support/NetworkUtilsStub.cpp)

    foreach(suite SDPParser RTPHeader PhaseLockedLoop DoPDecoder AmplifierUnit ChannelMapper CompatibilityProfile DeviceChannelSettings NetworkErrorHandler NMOSSettings PTPMasterSettings Resampling StreamConfig InteropSDP RingBuffer RingBufferTiming PTPPeerTable DolbyModelCatalog RTCPReceiverTable)
        set(extra_sources "")
        if(suite STREQUAL "StreamConfig")
            set(extra_sources ${AES67_SEAM_STUB})
        endif()
        add_executable(Test${suite} Tests/Test${suite}.cpp ${extra_sources})
        target_link_libraries(Test${suite} PRIVATE aes67_core doctest_headers)
        add_test(NAME ${suite} COMMAND Test${suite})
        set_tests_properties(${suite} PROPERTIES LABELS "unit" TIMEOUT 60)
    endforeach()

    # The interop suite reads its fixtures from disk rather than embedding
    # them: they are other people's output, and a file that can be diffed
    # against the daemon's is worth more than a string literal.
    target_compile_definitions(TestInteropSDP PRIVATE
        AES67_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures")
    set_tests_properties(InteropSDP PROPERTIES LABELS "unit;interop")

    # The ring buffer has two suites: what it does, and what it does under two
    # threads and a clock. The second is labelled for what it is, so a consumer
    # that wants only the deterministic ones can exclude it.
    set_tests_properties(RingBufferTiming PROPERTIES LABELS "unit;timing")
endif()
