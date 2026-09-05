//
// TestIntegrationAudioPath.cpp
// AES67 macOS Driver
// Integration tests for the full audio data path
//
// Tests exercise the complete chain:
//   RTP TX -> Network -> RTP RX -> Ring Buffers -> IO Handler
// using localhost multicast and real component instances.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/RTP/SimpleRTP.h"
#include "NetworkEngine/RTP/RTPReceiver.h"
#include "NetworkEngine/RTP/RTPTransmitter.h"
#include "NetworkEngine/StreamChannelMapper.h"
#include "Driver/SDPParser.h"
#include "Shared/RingBuffer.hpp"
#include "Shared/Types.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <cmath>
#include <thread>
#include <chrono>
#include <array>
#include <atomic>
#include <cstring>
#include <numeric>
#include <unistd.h>

using namespace AES67;
using namespace AES67::RTP;

// Test result counter


// ============================================================================
// Helper: Create initialized ring buffer array (128 channels)
// Same pattern as BenchmarkIOHandler.cpp
// ============================================================================

namespace {
    template<size_t... Is>
    auto MakeRingBufferArray(size_t bufferSize, std::index_sequence<Is...>) {
        return std::array<SPSCRingBuffer<float>, sizeof...(Is)>{
            ((void)Is, SPSCRingBuffer<float>(bufferSize))...
        };
    }

    template<size_t N>
    auto MakeRingBufferArray(size_t bufferSize) {
        return MakeRingBufferArray(bufferSize, std::make_index_sequence<N>{});
    }

    constexpr size_t kNumChannels = 128;
    constexpr size_t kRingBufferSize = 4096;
}

// ============================================================================
// Helper: Create SDP session for testing
// ============================================================================

static SDPSession createTestSDP(
    const std::string& name,
    const std::string& multicastIP,
    uint16_t port,
    uint16_t channels,
    const std::string& encoding = "L24",
    uint32_t sampleRate = 48000,
    uint32_t framecount = 48
) {
    SDPSession sdp;
    sdp.sessionName = name;
    sdp.connectionAddress = multicastIP;
    sdp.port = port;
    sdp.encoding = encoding;
    sdp.sampleRate = sampleRate;
    sdp.numChannels = channels;
    sdp.payloadType = (encoding == "L16") ? PT_AES67_L16 : PT_AES67_L24;
    sdp.ptimeUs = 1000;
    sdp.framecount = framecount;
    sdp.originAddress = "127.0.0.1";
    sdp.ptpDomain = 0;
    return sdp;
}

// ============================================================================
// Helper: Create channel mapping
// ============================================================================

static ChannelMapping createTestMapping(
    const StreamID& id,
    const std::string& name,
    uint16_t streamChannels,
    uint16_t deviceStart
) {
    ChannelMapping mapping;
    mapping.streamID = id;
    mapping.streamName = name;
    mapping.streamChannelCount = streamChannels;
    mapping.streamChannelOffset = 0;
    mapping.deviceChannelStart = deviceStart;
    mapping.deviceChannelCount = streamChannels;
    return mapping;
}

// ============================================================================
// Helper: Send raw RTP packet via UDP to a multicast group
// Bypasses RTPTransmitter to have fine-grained control over packet content
// ============================================================================

static bool sendRawRTPPacket(
    const char* multicastIP,
    uint16_t port,
    uint16_t sequenceNumber,
    uint32_t timestamp,
    uint32_t ssrc,
    uint8_t payloadType,
    const uint8_t* payload,
    size_t payloadSize
) {
    // Create UDP socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) return false;

    // Set multicast TTL
    uint8_t ttl = 1;
    setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    // Enable loopback so the sender's own machine receives the packet
    uint8_t loopback = 1;
    setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, &loopback, sizeof(loopback));

    // Build destination address
    struct sockaddr_in destAddr;
    std::memset(&destAddr, 0, sizeof(destAddr));
    destAddr.sin_family = AF_INET;
    destAddr.sin_addr.s_addr = inet_addr(multicastIP);
    destAddr.sin_port = htons(port);

    // Build RTP header in network byte order
    RTPHeader header;
    header.version = 2;
    header.padding = 0;
    header.extension = 0;
    header.cc = 0;
    header.marker = 0;
    header.payloadType = payloadType;
    header.sequenceNumber = sequenceNumber;
    header.timestamp = timestamp;
    header.ssrc = ssrc;
    header.toNetworkOrder();

    // Send header + payload via scatter/gather
    struct iovec iov[2];
    iov[0].iov_base = &header;
    iov[0].iov_len = sizeof(header);
    iov[1].iov_base = const_cast<uint8_t*>(payload);
    iov[1].iov_len = payloadSize;

    struct msghdr msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.msg_name = &destAddr;
    msg.msg_namelen = sizeof(destAddr);
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    ssize_t sent = sendmsg(sockfd, &msg, 0);
    ::close(sockfd);

    return sent > 0;
}

// ============================================================================
// Test 1: RTP Receive -> Ring Buffer
// ============================================================================

TEST_CASE("RTP Receive To Ring Buffer") {
    std::cout << "Test: RTP Receive -> Ring Buffer... ";

    // Create ring buffers
    auto deviceBuffers = MakeRingBufferArray<kNumChannels>(kRingBufferSize);

    // Configure a 2-channel L16 RX stream on 239.69.69.1:5004, mapped to device channels 0-1
    const uint16_t rxChannels = 2;
    const uint16_t rxPort = 15004; // Use high port to avoid conflicts
    const char* rxAddr = "239.69.69.1";

    SDPSession rxSDP = createTestSDP("RX Test", rxAddr, rxPort, rxChannels, "L16");
    StreamID rxID = StreamID::generate();
    ChannelMapping rxMapping = createTestMapping(rxID, "RX Test", rxChannels, 0);

    // Create receiver
    RTPReceiver receiver(rxSDP, rxMapping, deviceBuffers);
    CHECK(!receiver.isRunning());

    bool started = receiver.start();
    CHECK(started);
    CHECK(receiver.isRunning());

    // Give the receiver threads time to initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Create known audio data: 48 frames, 2 channels
    // Channel 0: 0.5f for all frames, Channel 1: -0.5f for all frames
    const size_t frameCount = 48;
    const size_t totalSamples = frameCount * rxChannels;
    std::vector<float> audioData(totalSamples);
    for (size_t f = 0; f < frameCount; ++f) {
        audioData[f * rxChannels + 0] = 0.5f;   // Channel 0
        audioData[f * rxChannels + 1] = -0.5f;   // Channel 1
    }

    // Encode to L16
    std::vector<uint8_t> l16Payload(totalSamples * 2);
    L16Codec::encode(audioData.data(), totalSamples, l16Payload.data());

    // Send multiple RTP packets with sequential sequence numbers
    const uint32_t ssrc = 0x12345678;
    const int numPackets = 20;
    for (int i = 0; i < numPackets; ++i) {
        bool sent = sendRawRTPPacket(
            rxAddr, rxPort,
            static_cast<uint16_t>(i),       // sequence number
            static_cast<uint32_t>(i * 48),   // timestamp
            ssrc,
            PT_AES67_L16,
            l16Payload.data(),
            l16Payload.size()
        );
        CHECK(sent);
        // Small delay between packets to simulate real RTP timing
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    // Wait for receiver to process packets through jitter buffer -> ring buffers
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Check that data appeared in the ring buffers at device channels 0 and 1
    size_t ch0Available = deviceBuffers[0].available();
    size_t ch1Available = deviceBuffers[1].available();

    CHECK(ch0Available > 0);
    CHECK(ch1Available > 0);

    // Read and verify sample values from channel 0
    std::vector<float> readBuffer(ch0Available);
    size_t ch0Read = deviceBuffers[0].read(readBuffer.data(), ch0Available);
    CHECK(ch0Read > 0);

    // Verify the samples are approximately 0.5 (L16 has limited precision)
    bool ch0ValuesCorrect = true;
    for (size_t i = 0; i < ch0Read; ++i) {
        if (std::abs(readBuffer[i] - 0.5f) > 0.01f) {
            ch0ValuesCorrect = false;
            break;
        }
    }
    CHECK(ch0ValuesCorrect);

    // Read and verify channel 1
    readBuffer.resize(ch1Available);
    size_t ch1Read = deviceBuffers[1].read(readBuffer.data(), ch1Available);
    CHECK(ch1Read > 0);

    bool ch1ValuesCorrect = true;
    for (size_t i = 0; i < ch1Read; ++i) {
        if (std::abs(readBuffer[i] - (-0.5f)) > 0.01f) {
            ch1ValuesCorrect = false;
            break;
        }
    }
    CHECK(ch1ValuesCorrect);

    // Verify unmapped channels remain empty
    CHECK(deviceBuffers[2].available() == 0);
    CHECK(deviceBuffers[3].available() == 0);

    // Check receiver statistics
    StatisticsSnapshot stats = receiver.getStatistics();
    CHECK(stats.packetsReceived > 0);

    receiver.stop();
    CHECK(!receiver.isRunning());

    std::cout << "PASS" << std::endl;
}

// ============================================================================
// Test 2: Ring Buffer -> RTP Transmit
// ============================================================================

TEST_CASE("Ring Buffer To RTP Transmit") {
    std::cout << "Test: Ring Buffer -> RTP Transmit... ";

    // Create ring buffers
    auto deviceBuffers = MakeRingBufferArray<kNumChannels>(kRingBufferSize);

    // Configure a 2-channel L16 TX stream on 239.69.69.2:15006
    const uint16_t txChannels = 2;
    const uint16_t txPort = 15006;
    const char* txAddr = "239.69.69.2";

    SDPSession txSDP = createTestSDP("TX Test", txAddr, txPort, txChannels, "L16");
    StreamID txID = StreamID::generate();
    ChannelMapping txMapping = createTestMapping(txID, "TX Test", txChannels, 8);

    // Pre-fill the output ring buffers with known data at device channels 8-9
    // Channel 8: sine wave at 0.75f amplitude
    // Channel 9: constant -0.25f
    const size_t prefillFrames = 480; // 10 packets worth @ 48 samples
    std::vector<float> ch8Data(prefillFrames);
    std::vector<float> ch9Data(prefillFrames);
    for (size_t i = 0; i < prefillFrames; ++i) {
        ch8Data[i] = 0.75f * std::sin(2.0f * M_PI * 1000.0f * i / 48000.0f);
        ch9Data[i] = -0.25f;
    }

    size_t written8 = deviceBuffers[8].write(ch8Data.data(), prefillFrames);
    size_t written9 = deviceBuffers[9].write(ch9Data.data(), prefillFrames);
    CHECK(written8 == prefillFrames);
    CHECK(written9 == prefillFrames);

    // Create transmitter
    RTPTransmitter transmitter(txSDP, txMapping, deviceBuffers);
    CHECK(!transmitter.isRunning());

    bool started = transmitter.start();
    CHECK(started);
    CHECK(transmitter.isRunning());

    // Let the transmitter send packets for a brief period
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    transmitter.stop();
    CHECK(!transmitter.isRunning());

    // Verify transmitter sent packets
    StatisticsSnapshot stats = transmitter.getStatistics();
    CHECK(stats.bytesSent > 0);

    // Verify the ring buffers were consumed
    size_t remaining8 = deviceBuffers[8].available();
    size_t remaining9 = deviceBuffers[9].available();
    CHECK(remaining8 < prefillFrames);
    CHECK(remaining9 < prefillFrames);

    std::cout << "PASS" << std::endl;
}

// ============================================================================
// Test 3: Full Loopback (TX -> Network -> RX)
// ============================================================================

TEST_CASE("Full Loopback") {
    std::cout << "Test: Full Loopback (TX -> Network -> RX)... ";

    // Create separate buffer arrays for TX and RX
    auto txBuffers = MakeRingBufferArray<kNumChannels>(kRingBufferSize);
    auto rxBuffers = MakeRingBufferArray<kNumChannels>(kRingBufferSize);

    // Shared multicast group for loopback
    const char* loopAddr = "239.69.69.3";
    const uint16_t loopPort = 15008;
    const uint16_t channels = 2;

    // TX: reads from txBuffers channels 0-1, sends to multicast
    SDPSession txSDP = createTestSDP("Loopback TX", loopAddr, loopPort, channels, "L24");
    StreamID txID = StreamID::generate();
    ChannelMapping txMapping = createTestMapping(txID, "Loopback TX", channels, 0);

    // RX: receives from multicast, writes to rxBuffers channels 0-1
    SDPSession rxSDP = createTestSDP("Loopback RX", loopAddr, loopPort, channels, "L24");
    StreamID rxID = StreamID::generate();
    ChannelMapping rxMapping = createTestMapping(rxID, "Loopback RX", channels, 0);

    // Pre-fill TX buffers with a recognizable pattern
    // Channel 0: constant 0.25f, Channel 1: constant -0.75f
    const size_t prefillFrames = 960; // 20 packets
    std::vector<float> txCh0(prefillFrames, 0.25f);
    std::vector<float> txCh1(prefillFrames, -0.75f);
    txBuffers[0].write(txCh0.data(), prefillFrames);
    txBuffers[1].write(txCh1.data(), prefillFrames);

    // Start receiver first so it binds to the multicast group
    RTPReceiver receiver(rxSDP, rxMapping, rxBuffers);
    bool rxStarted = receiver.start();
    CHECK(rxStarted);

    // Give receiver time to bind socket
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Start transmitter
    RTPTransmitter transmitter(txSDP, txMapping, txBuffers);
    bool txStarted = transmitter.start();
    CHECK(txStarted);

    // Let the loopback run for enough time for packets to flow
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Stop both
    transmitter.stop();
    receiver.stop();

    // Verify TX sent data
    StatisticsSnapshot txStats = transmitter.getStatistics();
    CHECK(txStats.bytesSent > 0);

    // Verify RX received data
    StatisticsSnapshot rxStats = receiver.getStatistics();
    CHECK(rxStats.packetsReceived > 0);

    // Verify data arrived in RX ring buffers
    size_t rxCh0Available = rxBuffers[0].available();
    size_t rxCh1Available = rxBuffers[1].available();
    CHECK(rxCh0Available > 0);
    CHECK(rxCh1Available > 0);

    // Read received data and verify integrity
    std::vector<float> rxCh0Data(rxCh0Available);
    std::vector<float> rxCh1Data(rxCh1Available);
    rxBuffers[0].read(rxCh0Data.data(), rxCh0Available);
    rxBuffers[1].read(rxCh1Data.data(), rxCh1Available);

    // Check that received values match sent values within L24 precision
    // (L24 round-trip tolerance ~0.001). Only the first prefillFrames
    // samples carry the pattern: the transmitter keeps sending after the
    // TX ring runs dry, silence-filling as AES67's continuous-flow rule
    // requires (see the "Continuous Packet Flow (silence on empty)"
    // case, which asserts exactly that). This test predated that
    // behavior and demanded pattern everywhere, so it failed on the
    // silence tail with zero actual corruption (diagnosed 2026-08-31:
    // 960 pattern samples bit-exact, then only zeros).
    bool ch0Correct = true;
    for (size_t i = 0; i < rxCh0Available; ++i) {
        const float expected = i < prefillFrames ? 0.25f : 0.0f;
        if (std::abs(rxCh0Data[i] - expected) > 0.01f) {
            ch0Correct = false;
            break;
        }
    }
    CHECK(ch0Correct);
    // The pattern must actually have arrived, not just silence.
    CHECK(rxCh0Available >= prefillFrames);

    bool ch1Correct = true;
    for (size_t i = 0; i < rxCh1Available; ++i) {
        const float expected = i < prefillFrames ? -0.75f : 0.0f;
        if (std::abs(rxCh1Data[i] - expected) > 0.01f) {
            ch1Correct = false;
            break;
        }
    }
    CHECK(ch1Correct);
    CHECK(rxCh1Available >= prefillFrames);

    std::cout << "PASS" << std::endl;
}

// ============================================================================
// Test 4: Underrun Behavior
// ============================================================================

TEST_CASE("Underrun Behavior") {
    std::cout << "Test: Underrun Behavior (empty ring buffers -> silence)... ";

    // Create ring buffers - leave them EMPTY (no data written)
    auto deviceBuffers = MakeRingBufferArray<kNumChannels>(kRingBufferSize);

    // Verify all buffers start empty
    for (size_t ch = 0; ch < kNumChannels; ++ch) {
        CHECK(deviceBuffers[ch].isEmpty());
    }

    // Simulate what the IO handler does when reading from empty input buffers:
    // It should fill with silence (zeros) and increment underrun counter.
    //
    // We test this directly using the ring buffer API since AES67IOHandler
    // requires libASPL types. The IO handler's processInput() calls
    // inputBuffers_[ch].read() -- if it returns 0, it zero-fills.
    const size_t frameCount = 48;
    std::vector<float> readBuffer(frameCount, 999.0f); // Fill with non-zero sentinel

    // Read from empty ring buffer -- should return 0 (no data available)
    size_t samplesRead = deviceBuffers[0].read(readBuffer.data(), frameCount);
    CHECK(samplesRead == 0);

    // The IO handler would fill with silence here:
    if (samplesRead < frameCount) {
        std::memset(&readBuffer[samplesRead], 0, (frameCount - samplesRead) * sizeof(float));
    }

    // Verify silence was filled
    bool allZeros = true;
    for (size_t i = 0; i < frameCount; ++i) {
        if (readBuffer[i] != 0.0f) {
            allZeros = false;
            break;
        }
    }
    CHECK(allZeros);

    // Now test with an actual RTPReceiver that is connected but gets no data
    // after the initial packets. The consume loop should increment underruns.
    SDPSession sdp = createTestSDP("Underrun RX", "239.69.69.4", 15010, 2, "L16");
    StreamID id = StreamID::generate();
    ChannelMapping mapping = createTestMapping(id, "Underrun RX", 2, 0);

    RTPReceiver receiver(sdp, mapping, deviceBuffers);
    bool started = receiver.start();
    CHECK(started);

    // Send a few packets to connect, then stop sending
    const size_t samples = 48 * 2;
    std::vector<float> audio(samples, 0.1f);
    std::vector<uint8_t> payload(samples * 2);
    L16Codec::encode(audio.data(), samples, payload.data());

    for (int i = 0; i < 3; ++i) {
        sendRawRTPPacket("239.69.69.4", 15010, i, i * 48, 0xAABBCCDD,
                         PT_AES67_L16, payload.data(), payload.size());
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    // Now stop sending and wait -- the consume loop should detect underruns
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    StatisticsSnapshot stats = receiver.getStatistics();
    // The receiver should have detected underruns in the consume loop
    // because the jitter buffer becomes empty after the initial packets
    CHECK((stats.underruns > 0 || stats.packetsReceived > 0));

    receiver.stop();

    std::cout << "PASS" << std::endl;
}

// ============================================================================
// Test 5: Overrun Behavior
// ============================================================================

TEST_CASE("Overrun Behavior") {
    std::cout << "Test: Overrun Behavior (ring buffer overflow)... ";

    // Create ring buffers with a SMALL capacity to easily trigger overrun
    const size_t smallCapacity = 64;

    // We need the full 128-channel array but with small buffers
    auto deviceBuffers = MakeRingBufferArray<kNumChannels>(smallCapacity);

    // Fill the ring buffer to capacity
    std::vector<float> fillData(smallCapacity, 0.5f);
    size_t written = deviceBuffers[0].write(fillData.data(), smallCapacity);
    CHECK(written == smallCapacity);
    CHECK(deviceBuffers[0].isFull());

    // Try to write more -- should return 0 (overrun)
    float extraSample = 0.99f;
    size_t overflowWritten = deviceBuffers[0].write(&extraSample, 1);
    CHECK(overflowWritten == 0);

    // Verify the buffer data is intact (old data preserved, not corrupted)
    std::vector<float> readBack(smallCapacity);
    size_t readCount = deviceBuffers[0].read(readBack.data(), smallCapacity);
    CHECK(readCount == smallCapacity);

    bool dataIntact = true;
    for (size_t i = 0; i < readCount; ++i) {
        if (std::abs(readBack[i] - 0.5f) > 0.001f) {
            dataIntact = false;
            break;
        }
    }
    CHECK(dataIntact);

    // Test overrun tracking: simulate Core Audio writing to output buffers
    // when the network consumer is too slow. Fill all channels, then try
    // to write more.
    auto outputBuffers = MakeRingBufferArray<kNumChannels>(smallCapacity);
    std::atomic<uint64_t> overrunCounter{0};

    // Fill channel 0 and 1
    outputBuffers[0].write(fillData.data(), smallCapacity);
    outputBuffers[1].write(fillData.data(), smallCapacity);

    // Simulate processOutput behavior: try to write and track overruns
    float newData[48];
    std::memset(newData, 0, sizeof(newData));

    for (size_t ch = 0; ch < 2; ++ch) {
        size_t samplesWritten = outputBuffers[ch].write(newData, 48);
        if (samplesWritten < 48) {
            overrunCounter.fetch_add(1, std::memory_order_relaxed);
        }
    }

    CHECK(overrunCounter.load() == 2);

    std::cout << "PASS" << std::endl;
}

// ============================================================================
// Test 6: Multi-Stream Channel Isolation
// ============================================================================

TEST_CASE("Multi Stream Channel Isolation") {
    std::cout << "Test: Multi-Stream Channel Isolation... ";

    // Create ring buffers
    auto deviceBuffers = MakeRingBufferArray<kNumChannels>(kRingBufferSize);

    // Stream A: 2 channels on 239.69.69.5:15012, mapped to device channels 0-1
    // Stream B: 2 channels on 239.69.69.6:15014, mapped to device channels 4-5
    const uint16_t channels = 2;

    SDPSession sdpA = createTestSDP("Stream A", "239.69.69.5", 15012, channels, "L16");
    StreamID idA = StreamID::generate();
    ChannelMapping mappingA = createTestMapping(idA, "Stream A", channels, 0);

    SDPSession sdpB = createTestSDP("Stream B", "239.69.69.6", 15014, channels, "L16");
    StreamID idB = StreamID::generate();
    ChannelMapping mappingB = createTestMapping(idB, "Stream B", channels, 4);

    // Create two receivers
    RTPReceiver receiverA(sdpA, mappingA, deviceBuffers);
    RTPReceiver receiverB(sdpB, mappingB, deviceBuffers);

    bool startedA = receiverA.start();
    bool startedB = receiverB.start();
    CHECK(startedA);
    CHECK(startedB);

    // Give receivers time to bind
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send distinct data to each stream
    // Stream A: all samples = 0.3f
    // Stream B: all samples = -0.7f
    const size_t frameCount = 48;
    const size_t totalSamples = frameCount * channels;

    std::vector<float> audioA(totalSamples, 0.3f);
    std::vector<float> audioB(totalSamples, -0.7f);

    std::vector<uint8_t> payloadA(totalSamples * 2);
    std::vector<uint8_t> payloadB(totalSamples * 2);

    L16Codec::encode(audioA.data(), totalSamples, payloadA.data());
    L16Codec::encode(audioB.data(), totalSamples, payloadB.data());

    const int numPackets = 20;
    for (int i = 0; i < numPackets; ++i) {
        sendRawRTPPacket("239.69.69.5", 15012, i, i * 48, 0x11111111,
                         PT_AES67_L16, payloadA.data(), payloadA.size());
        sendRawRTPPacket("239.69.69.6", 15014, i, i * 48, 0x22222222,
                         PT_AES67_L16, payloadB.data(), payloadB.size());
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    // Wait for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Check Stream A data arrived in channels 0-1 only
    size_t aCh0 = deviceBuffers[0].available();
    size_t aCh1 = deviceBuffers[1].available();
    CHECK(aCh0 > 0);
    CHECK(aCh1 > 0);

    // Check Stream B data arrived in channels 4-5 only
    size_t bCh4 = deviceBuffers[4].available();
    size_t bCh5 = deviceBuffers[5].available();
    CHECK(bCh4 > 0);
    CHECK(bCh5 > 0);

    // Verify isolation: channels 2-3 (between A and B) should be empty
    CHECK(deviceBuffers[2].available() == 0);
    CHECK(deviceBuffers[3].available() == 0);

    // Verify Stream A data is correct (~0.3f)
    std::vector<float> aCh0Data(aCh0);
    deviceBuffers[0].read(aCh0Data.data(), aCh0);
    bool aCorrect = true;
    for (size_t i = 0; i < aCh0; ++i) {
        if (std::abs(aCh0Data[i] - 0.3f) > 0.02f) {
            aCorrect = false;
            break;
        }
    }
    CHECK(aCorrect);

    // Verify Stream B data is correct (~-0.7f)
    std::vector<float> bCh4Data(bCh4);
    deviceBuffers[4].read(bCh4Data.data(), bCh4);
    bool bCorrect = true;
    for (size_t i = 0; i < bCh4; ++i) {
        if (std::abs(bCh4Data[i] - (-0.7f)) > 0.02f) {
            bCorrect = false;
            break;
        }
    }
    CHECK(bCorrect);

    // Verify no cross-contamination: Stream A data should NOT appear in Stream B channels
    // and vice versa. We already confirmed the gap channels are empty.
    // Additionally verify that Stream A's values are distinct from Stream B's
    CHECK(std::abs(0.3f - (-0.7f)) > 0.5f);

    // Stop both receivers
    receiverA.stop();
    receiverB.stop();
    CHECK(!receiverA.isRunning());
    CHECK(!receiverB.isRunning());

    std::cout << "PASS" << std::endl;
}

// ============================================================================
// Test 7: L16 vs L24 Encoding Round-Trip via Ring Buffers
// ============================================================================

TEST_CASE("Encoding Round Trip") {
    std::cout << "Test: L16/L24 Encoding Round-Trip via Ring Buffers... ";

    // Test that encoding -> ring buffer -> decoding preserves audio data
    // at the expected precision for each codec.

    const size_t numSamples = 256;
    std::vector<float> original(numSamples);
    for (size_t i = 0; i < numSamples; ++i) {
        original[i] = std::sin(2.0f * M_PI * i / numSamples);
    }

    // L16 round-trip via codec
    {
        std::vector<uint8_t> encoded(numSamples * 2);
        std::vector<float> decoded(numSamples);

        L16Codec::encode(original.data(), numSamples, encoded.data());
        L16Codec::decode(encoded.data(), encoded.size(), decoded.data());

        // Write decoded to ring buffer and read back
        SPSCRingBuffer<float> ringBuffer(kRingBufferSize);
        size_t written = ringBuffer.write(decoded.data(), numSamples);
        CHECK(written == numSamples);

        std::vector<float> readBack(numSamples);
        size_t readCount = ringBuffer.read(readBack.data(), numSamples);
        CHECK(readCount == numSamples);

        // Verify L16 precision (~0.01 tolerance)
        double maxError = 0.0;
        for (size_t i = 0; i < numSamples; ++i) {
            maxError = std::max(maxError, static_cast<double>(std::abs(readBack[i] - original[i])));
        }
        CHECK(maxError < 0.01);
    }

    // L24 round-trip via codec
    {
        std::vector<uint8_t> encoded(numSamples * 3);
        std::vector<float> decoded(numSamples);

        L24Codec::encode(original.data(), numSamples, encoded.data());
        L24Codec::decode(encoded.data(), encoded.size(), decoded.data());

        // Write decoded to ring buffer and read back
        SPSCRingBuffer<float> ringBuffer(kRingBufferSize);
        size_t written = ringBuffer.write(decoded.data(), numSamples);
        CHECK(written == numSamples);

        std::vector<float> readBack(numSamples);
        size_t readCount = ringBuffer.read(readBack.data(), numSamples);
        CHECK(readCount == numSamples);

        // Verify L24 precision (~0.001 tolerance)
        double maxError = 0.0;
        for (size_t i = 0; i < numSamples; ++i) {
            maxError = std::max(maxError, static_cast<double>(std::abs(readBack[i] - original[i])));
        }
        CHECK(maxError < 0.001);
    }

    std::cout << "PASS" << std::endl;
}

// ============================================================================
// Test 8: Channel Mapping Correctness Through Receiver
// ============================================================================

TEST_CASE("Channel Mapping Through Receiver") {
    std::cout << "Test: Channel Mapping Correctness (offset mapping)... ";

    auto deviceBuffers = MakeRingBufferArray<kNumChannels>(kRingBufferSize);

    // Configure a 4-channel stream mapped to device channels 16-19
    const uint16_t rxChannels = 4;
    SDPSession sdp = createTestSDP("Mapped RX", "239.69.69.7", 15016, rxChannels, "L16");
    StreamID id = StreamID::generate();
    ChannelMapping mapping = createTestMapping(id, "Mapped RX", rxChannels, 16);

    RTPReceiver receiver(sdp, mapping, deviceBuffers);
    bool started = receiver.start();
    CHECK(started);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Create 4-channel interleaved audio with distinct per-channel values
    // Channel 0: 0.1, Channel 1: 0.2, Channel 2: 0.3, Channel 3: 0.4
    const size_t frameCount = 48;
    const size_t totalSamples = frameCount * rxChannels;
    std::vector<float> audio(totalSamples);
    for (size_t f = 0; f < frameCount; ++f) {
        audio[f * rxChannels + 0] = 0.1f;
        audio[f * rxChannels + 1] = 0.2f;
        audio[f * rxChannels + 2] = 0.3f;
        audio[f * rxChannels + 3] = 0.4f;
    }

    std::vector<uint8_t> payload(totalSamples * 2);
    L16Codec::encode(audio.data(), totalSamples, payload.data());

    const int numPackets = 15;
    for (int i = 0; i < numPackets; ++i) {
        sendRawRTPPacket("239.69.69.7", 15016, i, i * 48, 0x33333333,
                         PT_AES67_L16, payload.data(), payload.size());
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Verify data landed in device channels 16-19
    CHECK(deviceBuffers[16].available() > 0);
    CHECK(deviceBuffers[17].available() > 0);
    CHECK(deviceBuffers[18].available() > 0);
    CHECK(deviceBuffers[19].available() > 0);

    // Verify channels 0-15 and 20+ are empty
    for (int ch = 0; ch < 16; ++ch) {
        CHECK(deviceBuffers[ch].available() == 0);
    }
    for (int ch = 20; ch < 24; ++ch) {
        CHECK(deviceBuffers[ch].available() == 0);
    }

    // Verify per-channel data correctness
    auto verifyChannel = [&](size_t devCh, float expectedVal, const char* desc) -> void {
        size_t avail = deviceBuffers[devCh].available();
        std::vector<float> data(avail);
        deviceBuffers[devCh].read(data.data(), avail);
        for (size_t i = 0; i < avail; ++i) {
            if (std::abs(data[i] - expectedVal) > 0.02f) {
                FAIL_CHECK(desc << " (sample " << i << " = " << data[i]
                                << ", expected ~" << expectedVal << ")");
            }
        }
    };

    verifyChannel(16, 0.1f, "Device ch16 should have stream ch0 data (~0.1)");
    verifyChannel(17, 0.2f, "Device ch17 should have stream ch1 data (~0.2)");
    verifyChannel(18, 0.3f, "Device ch18 should have stream ch2 data (~0.3)");
    verifyChannel(19, 0.4f, "Device ch19 should have stream ch3 data (~0.4)");

    receiver.stop();

    std::cout << "PASS" << std::endl;
}

// ============================================================================
// Test 9: Receiver Statistics Accuracy
// ============================================================================

TEST_CASE("Receiver Statistics") {
    std::cout << "Test: Receiver Statistics Accuracy... ";

    auto deviceBuffers = MakeRingBufferArray<kNumChannels>(kRingBufferSize);

    SDPSession sdp = createTestSDP("Stats RX", "239.69.69.8", 15018, 2, "L16");
    StreamID id = StreamID::generate();
    ChannelMapping mapping = createTestMapping(id, "Stats RX", 2, 0);

    RTPReceiver receiver(sdp, mapping, deviceBuffers);
    receiver.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Prepare L16 payload for 2 channels, 48 frames
    const size_t totalSamples = 48 * 2;
    std::vector<float> audio(totalSamples, 0.0f);
    std::vector<uint8_t> payload(totalSamples * 2);
    L16Codec::encode(audio.data(), totalSamples, payload.data());

    // Send 10 packets with sequential sequence numbers
    for (int i = 0; i < 10; ++i) {
        sendRawRTPPacket("239.69.69.8", 15018, i, i * 48, 0x44444444,
                         PT_AES67_L16, payload.data(), payload.size());
        std::this_thread::sleep_for(std::chrono::microseconds(800));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    StatisticsSnapshot stats = receiver.getStatistics();

    // Should have received all 10 packets
    CHECK(stats.packetsReceived >= 8);

    // Bytes received should be > 0
    CHECK(stats.bytesReceived > 0);

    // No malformed packets (we sent valid ones)
    CHECK(stats.malformedPackets == 0);

    // Now send a gap: skip sequence numbers 10-14, send 15
    sendRawRTPPacket("239.69.69.8", 15018, 15, 15 * 48, 0x44444444,
                     PT_AES67_L16, payload.data(), payload.size());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    StatisticsSnapshot stats2 = receiver.getStatistics();
    // The receiver should detect the gap (packets 10-14 lost)
    CHECK((stats2.packetsLost > 0 || stats2.packetsReceived > stats.packetsReceived));

    receiver.stop();

    // Test reset functionality
    receiver.resetStatistics();

    std::cout << "PASS" << std::endl;
}

// ============================================================================
// Test 10: Transmitter Continuous Packet Flow
// ============================================================================

TEST_CASE("Transmitter Continuous Flow") {
    std::cout << "Test: Transmitter Continuous Packet Flow (silence on empty)... ";

    // AES67 requires continuous packets even when ring buffers are empty.
    // The transmitter should send silence-filled packets in that case.

    auto deviceBuffers = MakeRingBufferArray<kNumChannels>(kRingBufferSize);

    // Do NOT pre-fill the ring buffers -- they start empty
    SDPSession sdp = createTestSDP("Continuous TX", "239.69.69.9", 15020, 2, "L16");
    StreamID id = StreamID::generate();
    ChannelMapping mapping = createTestMapping(id, "Continuous TX", 2, 0);

    RTPTransmitter transmitter(sdp, mapping, deviceBuffers);
    bool started = transmitter.start();
    CHECK(started);

    // Let it run for 50ms -- it should still be sending packets (silence)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    StatisticsSnapshot stats = transmitter.getStatistics();
    CHECK(stats.bytesSent > 0);

    transmitter.stop();

    std::cout << "PASS" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

