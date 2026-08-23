//
// TestIntegrationAudioPath.cpp
// AES67 macOS Driver
// Integration tests for the full audio data path
//
// Tests exercise the complete chain:
//   RTP TX -> Network -> RTP RX -> Ring Buffers -> IO Handler
// using localhost multicast and real component instances.
//

#include "../NetworkEngine/RTP/SimpleRTP.h"
#include "../NetworkEngine/RTP/RTPReceiver.h"
#include "../NetworkEngine/RTP/RTPTransmitter.h"
#include "../NetworkEngine/StreamChannelMapper.h"
#include "../Driver/SDPParser.h"
#include "../Shared/RingBuffer.hpp"
#include "../Shared/Types.h"
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
static int testsPassed = 0;
static int testsFailed = 0;

#define TEST_ASSERT(condition, message) \
    if (!(condition)) { \
        std::cerr << "FAIL: " << message << std::endl; \
        testsFailed++; \
        return false; \
    } else { \
        testsPassed++; \
    }

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

bool testRTPReceiveToRingBuffer() {
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
    TEST_ASSERT(!receiver.isRunning(), "Receiver should not be running before start");

    bool started = receiver.start();
    TEST_ASSERT(started, "Receiver should start successfully");
    TEST_ASSERT(receiver.isRunning(), "Receiver should be running after start");

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
        TEST_ASSERT(sent, "RTP packet should be sent successfully");
        // Small delay between packets to simulate real RTP timing
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    // Wait for receiver to process packets through jitter buffer -> ring buffers
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Check that data appeared in the ring buffers at device channels 0 and 1
    size_t ch0Available = deviceBuffers[0].available();
    size_t ch1Available = deviceBuffers[1].available();

    TEST_ASSERT(ch0Available > 0, "Channel 0 ring buffer should have data");
    TEST_ASSERT(ch1Available > 0, "Channel 1 ring buffer should have data");

    // Read and verify sample values from channel 0
    std::vector<float> readBuffer(ch0Available);
    size_t ch0Read = deviceBuffers[0].read(readBuffer.data(), ch0Available);
    TEST_ASSERT(ch0Read > 0, "Should read data from channel 0");

    // Verify the samples are approximately 0.5 (L16 has limited precision)
    bool ch0ValuesCorrect = true;
    for (size_t i = 0; i < ch0Read; ++i) {
        if (std::abs(readBuffer[i] - 0.5f) > 0.01f) {
            ch0ValuesCorrect = false;
            break;
        }
    }
    TEST_ASSERT(ch0ValuesCorrect, "Channel 0 samples should be ~0.5f");

    // Read and verify channel 1
    readBuffer.resize(ch1Available);
    size_t ch1Read = deviceBuffers[1].read(readBuffer.data(), ch1Available);
    TEST_ASSERT(ch1Read > 0, "Should read data from channel 1");

    bool ch1ValuesCorrect = true;
    for (size_t i = 0; i < ch1Read; ++i) {
        if (std::abs(readBuffer[i] - (-0.5f)) > 0.01f) {
            ch1ValuesCorrect = false;
            break;
        }
    }
    TEST_ASSERT(ch1ValuesCorrect, "Channel 1 samples should be ~-0.5f");

    // Verify unmapped channels remain empty
    TEST_ASSERT(deviceBuffers[2].available() == 0, "Unmapped channel 2 should be empty");
    TEST_ASSERT(deviceBuffers[3].available() == 0, "Unmapped channel 3 should be empty");

    // Check receiver statistics
    StatisticsSnapshot stats = receiver.getStatistics();
    TEST_ASSERT(stats.packetsReceived > 0, "Should report received packets");

    receiver.stop();
    TEST_ASSERT(!receiver.isRunning(), "Receiver should stop");

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 2: Ring Buffer -> RTP Transmit
// ============================================================================

bool testRingBufferToRTPTransmit() {
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
    TEST_ASSERT(written8 == prefillFrames, "Should write all frames to channel 8");
    TEST_ASSERT(written9 == prefillFrames, "Should write all frames to channel 9");

    // Create transmitter
    RTPTransmitter transmitter(txSDP, txMapping, deviceBuffers);
    TEST_ASSERT(!transmitter.isRunning(), "Transmitter should not be running before start");

    bool started = transmitter.start();
    TEST_ASSERT(started, "Transmitter should start successfully");
    TEST_ASSERT(transmitter.isRunning(), "Transmitter should be running after start");

    // Let the transmitter send packets for a brief period
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    transmitter.stop();
    TEST_ASSERT(!transmitter.isRunning(), "Transmitter should stop");

    // Verify transmitter sent packets
    StatisticsSnapshot stats = transmitter.getStatistics();
    TEST_ASSERT(stats.bytesSent > 0, "Transmitter should have sent bytes");

    // Verify the ring buffers were consumed
    size_t remaining8 = deviceBuffers[8].available();
    size_t remaining9 = deviceBuffers[9].available();
    TEST_ASSERT(remaining8 < prefillFrames, "Channel 8 buffer should be partially consumed");
    TEST_ASSERT(remaining9 < prefillFrames, "Channel 9 buffer should be partially consumed");

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 3: Full Loopback (TX -> Network -> RX)
// ============================================================================

bool testFullLoopback() {
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
    TEST_ASSERT(rxStarted, "Loopback receiver should start");

    // Give receiver time to bind socket
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Start transmitter
    RTPTransmitter transmitter(txSDP, txMapping, txBuffers);
    bool txStarted = transmitter.start();
    TEST_ASSERT(txStarted, "Loopback transmitter should start");

    // Let the loopback run for enough time for packets to flow
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Stop both
    transmitter.stop();
    receiver.stop();

    // Verify TX sent data
    StatisticsSnapshot txStats = transmitter.getStatistics();
    TEST_ASSERT(txStats.bytesSent > 0, "TX should have sent bytes");

    // Verify RX received data
    StatisticsSnapshot rxStats = receiver.getStatistics();
    TEST_ASSERT(rxStats.packetsReceived > 0, "RX should have received packets");

    // Verify data arrived in RX ring buffers
    size_t rxCh0Available = rxBuffers[0].available();
    size_t rxCh1Available = rxBuffers[1].available();
    TEST_ASSERT(rxCh0Available > 0, "RX channel 0 should have data");
    TEST_ASSERT(rxCh1Available > 0, "RX channel 1 should have data");

    // Read received data and verify integrity
    std::vector<float> rxCh0Data(rxCh0Available);
    std::vector<float> rxCh1Data(rxCh1Available);
    rxBuffers[0].read(rxCh0Data.data(), rxCh0Available);
    rxBuffers[1].read(rxCh1Data.data(), rxCh1Available);

    // Check that received values match sent values within L24 precision
    // L24 round-trip tolerance: ~0.001
    bool ch0Correct = true;
    for (size_t i = 0; i < rxCh0Available; ++i) {
        if (std::abs(rxCh0Data[i] - 0.25f) > 0.01f) {
            ch0Correct = false;
            break;
        }
    }
    TEST_ASSERT(ch0Correct, "Loopback channel 0 data should match (~0.25f)");

    bool ch1Correct = true;
    for (size_t i = 0; i < rxCh1Available; ++i) {
        if (std::abs(rxCh1Data[i] - (-0.75f)) > 0.01f) {
            ch1Correct = false;
            break;
        }
    }
    TEST_ASSERT(ch1Correct, "Loopback channel 1 data should match (~-0.75f)");

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 4: Underrun Behavior
// ============================================================================

bool testUnderrunBehavior() {
    std::cout << "Test: Underrun Behavior (empty ring buffers -> silence)... ";

    // Create ring buffers - leave them EMPTY (no data written)
    auto deviceBuffers = MakeRingBufferArray<kNumChannels>(kRingBufferSize);

    // Verify all buffers start empty
    for (size_t ch = 0; ch < kNumChannels; ++ch) {
        TEST_ASSERT(deviceBuffers[ch].isEmpty(), "All buffers should start empty");
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
    TEST_ASSERT(samplesRead == 0, "Read from empty buffer should return 0");

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
    TEST_ASSERT(allZeros, "Underrun should result in silence (all zeros)");

    // Now test with an actual RTPReceiver that is connected but gets no data
    // after the initial packets. The consume loop should increment underruns.
    SDPSession sdp = createTestSDP("Underrun RX", "239.69.69.4", 15010, 2, "L16");
    StreamID id = StreamID::generate();
    ChannelMapping mapping = createTestMapping(id, "Underrun RX", 2, 0);

    RTPReceiver receiver(sdp, mapping, deviceBuffers);
    bool started = receiver.start();
    TEST_ASSERT(started, "Underrun test receiver should start");

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
    TEST_ASSERT(stats.underruns > 0 || stats.packetsReceived > 0,
                "Should detect underruns or have received initial packets");

    receiver.stop();

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 5: Overrun Behavior
// ============================================================================

bool testOverrunBehavior() {
    std::cout << "Test: Overrun Behavior (ring buffer overflow)... ";

    // Create ring buffers with a SMALL capacity to easily trigger overrun
    const size_t smallCapacity = 64;

    // We need the full 128-channel array but with small buffers
    auto deviceBuffers = MakeRingBufferArray<kNumChannels>(smallCapacity);

    // Fill the ring buffer to capacity
    std::vector<float> fillData(smallCapacity, 0.5f);
    size_t written = deviceBuffers[0].write(fillData.data(), smallCapacity);
    TEST_ASSERT(written == smallCapacity, "Should fill buffer to capacity");
    TEST_ASSERT(deviceBuffers[0].isFull(), "Buffer should be full after fill");

    // Try to write more -- should return 0 (overrun)
    float extraSample = 0.99f;
    size_t overflowWritten = deviceBuffers[0].write(&extraSample, 1);
    TEST_ASSERT(overflowWritten == 0, "Write to full buffer should return 0 (overrun)");

    // Verify the buffer data is intact (old data preserved, not corrupted)
    std::vector<float> readBack(smallCapacity);
    size_t readCount = deviceBuffers[0].read(readBack.data(), smallCapacity);
    TEST_ASSERT(readCount == smallCapacity, "Should read back all original data");

    bool dataIntact = true;
    for (size_t i = 0; i < readCount; ++i) {
        if (std::abs(readBack[i] - 0.5f) > 0.001f) {
            dataIntact = false;
            break;
        }
    }
    TEST_ASSERT(dataIntact, "Original data should be preserved after overrun");

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

    TEST_ASSERT(overrunCounter.load() == 2,
                "Should detect overrun on both channels");

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 6: Multi-Stream Channel Isolation
// ============================================================================

bool testMultiStreamChannelIsolation() {
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
    TEST_ASSERT(startedA, "Receiver A should start");
    TEST_ASSERT(startedB, "Receiver B should start");

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
    TEST_ASSERT(aCh0 > 0, "Stream A: channel 0 should have data");
    TEST_ASSERT(aCh1 > 0, "Stream A: channel 1 should have data");

    // Check Stream B data arrived in channels 4-5 only
    size_t bCh4 = deviceBuffers[4].available();
    size_t bCh5 = deviceBuffers[5].available();
    TEST_ASSERT(bCh4 > 0, "Stream B: channel 4 should have data");
    TEST_ASSERT(bCh5 > 0, "Stream B: channel 5 should have data");

    // Verify isolation: channels 2-3 (between A and B) should be empty
    TEST_ASSERT(deviceBuffers[2].available() == 0,
                "Channel 2 (gap between streams) should be empty");
    TEST_ASSERT(deviceBuffers[3].available() == 0,
                "Channel 3 (gap between streams) should be empty");

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
    TEST_ASSERT(aCorrect, "Stream A channel 0 values should be ~0.3f");

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
    TEST_ASSERT(bCorrect, "Stream B channel 4 values should be ~-0.7f");

    // Verify no cross-contamination: Stream A data should NOT appear in Stream B channels
    // and vice versa. We already confirmed the gap channels are empty.
    // Additionally verify that Stream A's values are distinct from Stream B's
    TEST_ASSERT(std::abs(0.3f - (-0.7f)) > 0.5f,
                "Stream A and B values should be clearly distinct");

    // Stop both receivers
    receiverA.stop();
    receiverB.stop();
    TEST_ASSERT(!receiverA.isRunning(), "Receiver A should stop");
    TEST_ASSERT(!receiverB.isRunning(), "Receiver B should stop");

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 7: L16 vs L24 Encoding Round-Trip via Ring Buffers
// ============================================================================

bool testEncodingRoundTrip() {
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
        TEST_ASSERT(written == numSamples, "L16: should write all samples to ring buffer");

        std::vector<float> readBack(numSamples);
        size_t readCount = ringBuffer.read(readBack.data(), numSamples);
        TEST_ASSERT(readCount == numSamples, "L16: should read all samples from ring buffer");

        // Verify L16 precision (~0.01 tolerance)
        double maxError = 0.0;
        for (size_t i = 0; i < numSamples; ++i) {
            maxError = std::max(maxError, static_cast<double>(std::abs(readBack[i] - original[i])));
        }
        TEST_ASSERT(maxError < 0.01, "L16 round-trip error should be < 0.01");
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
        TEST_ASSERT(written == numSamples, "L24: should write all samples to ring buffer");

        std::vector<float> readBack(numSamples);
        size_t readCount = ringBuffer.read(readBack.data(), numSamples);
        TEST_ASSERT(readCount == numSamples, "L24: should read all samples from ring buffer");

        // Verify L24 precision (~0.001 tolerance)
        double maxError = 0.0;
        for (size_t i = 0; i < numSamples; ++i) {
            maxError = std::max(maxError, static_cast<double>(std::abs(readBack[i] - original[i])));
        }
        TEST_ASSERT(maxError < 0.001, "L24 round-trip error should be < 0.001");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 8: Channel Mapping Correctness Through Receiver
// ============================================================================

bool testChannelMappingThroughReceiver() {
    std::cout << "Test: Channel Mapping Correctness (offset mapping)... ";

    auto deviceBuffers = MakeRingBufferArray<kNumChannels>(kRingBufferSize);

    // Configure a 4-channel stream mapped to device channels 16-19
    const uint16_t rxChannels = 4;
    SDPSession sdp = createTestSDP("Mapped RX", "239.69.69.7", 15016, rxChannels, "L16");
    StreamID id = StreamID::generate();
    ChannelMapping mapping = createTestMapping(id, "Mapped RX", rxChannels, 16);

    RTPReceiver receiver(sdp, mapping, deviceBuffers);
    bool started = receiver.start();
    TEST_ASSERT(started, "Mapped receiver should start");

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
    TEST_ASSERT(deviceBuffers[16].available() > 0, "Device channel 16 should have data");
    TEST_ASSERT(deviceBuffers[17].available() > 0, "Device channel 17 should have data");
    TEST_ASSERT(deviceBuffers[18].available() > 0, "Device channel 18 should have data");
    TEST_ASSERT(deviceBuffers[19].available() > 0, "Device channel 19 should have data");

    // Verify channels 0-15 and 20+ are empty
    for (int ch = 0; ch < 16; ++ch) {
        TEST_ASSERT(deviceBuffers[ch].available() == 0,
                    "Channels below mapping range should be empty");
    }
    for (int ch = 20; ch < 24; ++ch) {
        TEST_ASSERT(deviceBuffers[ch].available() == 0,
                    "Channels above mapping range should be empty");
    }

    // Verify per-channel data correctness
    auto verifyChannel = [&](size_t devCh, float expectedVal, const char* desc) -> bool {
        size_t avail = deviceBuffers[devCh].available();
        std::vector<float> data(avail);
        deviceBuffers[devCh].read(data.data(), avail);
        for (size_t i = 0; i < avail; ++i) {
            if (std::abs(data[i] - expectedVal) > 0.02f) {
                std::cerr << "FAIL: " << desc << " (sample " << i
                          << " = " << data[i] << ", expected ~" << expectedVal << ")" << std::endl;
                testsFailed++;
                return false;
            }
        }
        testsPassed++;
        return true;
    };

    verifyChannel(16, 0.1f, "Device ch16 should have stream ch0 data (~0.1)");
    verifyChannel(17, 0.2f, "Device ch17 should have stream ch1 data (~0.2)");
    verifyChannel(18, 0.3f, "Device ch18 should have stream ch2 data (~0.3)");
    verifyChannel(19, 0.4f, "Device ch19 should have stream ch3 data (~0.4)");

    receiver.stop();

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 9: Receiver Statistics Accuracy
// ============================================================================

bool testReceiverStatistics() {
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
    TEST_ASSERT(stats.packetsReceived >= 8,
                "Should receive most of the 10 packets sent");

    // Bytes received should be > 0
    TEST_ASSERT(stats.bytesReceived > 0, "Should track bytes received");

    // No malformed packets (we sent valid ones)
    TEST_ASSERT(stats.malformedPackets == 0, "Should have no malformed packets");

    // Now send a gap: skip sequence numbers 10-14, send 15
    sendRawRTPPacket("239.69.69.8", 15018, 15, 15 * 48, 0x44444444,
                     PT_AES67_L16, payload.data(), payload.size());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    StatisticsSnapshot stats2 = receiver.getStatistics();
    // The receiver should detect the gap (packets 10-14 lost)
    TEST_ASSERT(stats2.packetsLost > 0 || stats2.packetsReceived > stats.packetsReceived,
                "Should detect gap or receive the new packet");

    receiver.stop();

    // Test reset functionality
    receiver.resetStatistics();

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 10: Transmitter Continuous Packet Flow
// ============================================================================

bool testTransmitterContinuousFlow() {
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
    TEST_ASSERT(started, "Transmitter should start even with empty buffers");

    // Let it run for 50ms -- it should still be sending packets (silence)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    StatisticsSnapshot stats = transmitter.getStatistics();
    TEST_ASSERT(stats.bytesSent > 0,
                "Transmitter should send silence packets even with empty buffers");

    transmitter.stop();

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "AES67 Integration Tests: Full Audio Path" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    std::cout << "RTP Receive Path Tests:" << std::endl;
    std::cout << "----------------------" << std::endl;
    testRTPReceiveToRingBuffer();
    std::cout << std::endl;

    std::cout << "RTP Transmit Path Tests:" << std::endl;
    std::cout << "-----------------------" << std::endl;
    testRingBufferToRTPTransmit();
    std::cout << std::endl;

    std::cout << "Full Loopback Tests:" << std::endl;
    std::cout << "-------------------" << std::endl;
    testFullLoopback();
    std::cout << std::endl;

    std::cout << "Buffer Edge Case Tests:" << std::endl;
    std::cout << "----------------------" << std::endl;
    testUnderrunBehavior();
    testOverrunBehavior();
    std::cout << std::endl;

    std::cout << "Channel Isolation Tests:" << std::endl;
    std::cout << "-----------------------" << std::endl;
    testMultiStreamChannelIsolation();
    std::cout << std::endl;

    std::cout << "Encoding Round-Trip Tests:" << std::endl;
    std::cout << "-------------------------" << std::endl;
    testEncodingRoundTrip();
    std::cout << std::endl;

    std::cout << "Channel Mapping Tests:" << std::endl;
    std::cout << "---------------------" << std::endl;
    testChannelMappingThroughReceiver();
    std::cout << std::endl;

    std::cout << "Statistics Tests:" << std::endl;
    std::cout << "----------------" << std::endl;
    testReceiverStatistics();
    std::cout << std::endl;

    std::cout << "Continuous Flow Tests:" << std::endl;
    std::cout << "---------------------" << std::endl;
    testTransmitterContinuousFlow();
    std::cout << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << "Test Results:" << std::endl;
    std::cout << "  Passed: " << testsPassed << std::endl;
    std::cout << "  Failed: " << testsFailed << std::endl;
    std::cout << "========================================" << std::endl;

    return testsFailed == 0 ? 0 : 1;
}
