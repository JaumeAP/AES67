//
// TestTypes.cpp
// Shared/Types.cpp: the identifiers, counters, validity rules and small
// utilities every other package in the tree builds on, and which nothing
// tested directly.
//
// These are characterisation tests: they pin what the code does today, so a
// later change to any of it has to be deliberate. Where the behaviour is
// arguably wrong -- a malformed UUID becoming the null one rather than being
// rejected, an IPv4 octet with leading zeros being accepted -- the test says
// so in its name rather than quietly asserting the wrong thing is right.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Shared/Types.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

using AES67::AudioEncoding;
using AES67::DeviceConfig;
using AES67::Error;
using AES67::ErrorCode;
using AES67::NetworkAddress;
using AES67::PTPConfig;
using AES67::SampleRate;
using AES67::Statistics;
using AES67::StatisticsSnapshot;
using AES67::StreamID;
using AES67::StreamInfo;

// ---------------------------------------------------------------------------
// StreamID
// ---------------------------------------------------------------------------

TEST_CASE("A default-constructed StreamID is the null one") {
    const StreamID id;
    CHECK(id.isNull());
    CHECK(id.toString() == "00000000-0000-0000-0000-000000000000");
    CHECK(id == StreamID::null());
}

TEST_CASE("A StreamID built from sixteen bytes prints them in UUID shape") {
    const uint8_t bytes[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                               0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
    const StreamID id(bytes);

    CHECK_FALSE(id.isNull());
    CHECK(id.toString() == "01234567-89ab-cdef-fedc-ba9876543210");
}

TEST_CASE("A StreamID round-trips through its own string form") {
    const uint8_t bytes[16] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x11, 0x22, 0x33,
                               0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};
    const StreamID original(bytes);
    const StreamID parsed(original.toString());

    CHECK(parsed == original);
    CHECK(parsed.toString() == original.toString());
}

TEST_CASE("A StreamID parses a UUID string with the dashes left out") {
    const StreamID dashed("01234567-89ab-cdef-fedc-ba9876543210");
    const StreamID bare("0123456789abcdeffedcba9876543210");

    CHECK(dashed == bare);
}

TEST_CASE("A UUID string of the wrong length parses to the null identifier") {
    // The constructor has no way to report a failure, so a malformed string
    // silently becomes null. A caller therefore has to check isNull() to tell
    // "this was not a UUID" from "this was the null UUID".
    CHECK(StreamID("").isNull());
    CHECK(StreamID("not-a-uuid").isNull());
    CHECK(StreamID("0123456789abcdef").isNull());
    CHECK(StreamID("0123456789abcdeffedcba98765432100000").isNull());
}

TEST_CASE("StreamID comparison is byte order on the raw UUID") {
    const uint8_t lowBytes[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    const uint8_t highBytes[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const StreamID low(lowBytes);
    const StreamID high(highBytes);

    CHECK(low < high);
    CHECK_FALSE(high < low);
    CHECK(low != high);
    CHECK_FALSE(low == high);
    CHECK(low == StreamID(lowBytes));
    CHECK_FALSE(low != StreamID(lowBytes));
}

TEST_CASE("A generated StreamID is a version 4 UUID and is not the null one") {
    const StreamID first = StreamID::generate();
    const StreamID second = StreamID::generate();

    CHECK_FALSE(first.isNull());
    CHECK_FALSE(second.isNull());
    CHECK(first != second);

    // Version nibble is the 13th hex digit, variant the 17th: RFC 4122 puts
    // '4' and one of 8/9/a/b there, and generate() sets both explicitly.
    const std::string text = first.toString();
    REQUIRE(text.size() == 36);
    CHECK(text[14] == '4');
    CHECK((text[19] == '8' || text[19] == '9' || text[19] == 'a' || text[19] == 'b'));
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

TEST_CASE("Reset returns every counter to zero") {
    Statistics stats;
    stats.packetsReceived.store(10);
    stats.packetsLost.store(2);
    stats.malformedPackets.store(3);
    stats.outOfOrderPackets.store(4);
    stats.underruns.store(5);
    stats.overruns.store(6);
    stats.jitterNs.store(7);
    stats.latencyNs.store(8);
    stats.bytesReceived.store(9);
    stats.bytesSent.store(11);
    stats.lastPacketTimeNs.store(12);

    stats.reset();

    const StatisticsSnapshot snap = stats.snapshot();
    CHECK(snap.packetsReceived == 0);
    CHECK(snap.packetsLost == 0);
    CHECK(snap.malformedPackets == 0);
    CHECK(snap.outOfOrderPackets == 0);
    CHECK(snap.underruns == 0);
    CHECK(snap.overruns == 0);
    CHECK(snap.jitterNs == 0);
    CHECK(snap.latencyNs == 0);
    CHECK(snap.bytesReceived == 0);
    CHECK(snap.bytesSent == 0);
    CHECK(snap.lastPacketTimeNs == 0);
}

TEST_CASE("Loss is a percentage of what was sent, not of what arrived") {
    Statistics stats;

    // Nothing received yet: no denominator, and the answer is zero rather
    // than a division by zero.
    CHECK(stats.getPacketLossPercent() == doctest::Approx(0.0));

    stats.packetsReceived.store(90);
    stats.packetsLost.store(10);
    // 10 lost out of the 100 that were sent.
    CHECK(stats.getPacketLossPercent() == doctest::Approx(10.0));

    stats.packetsReceived.store(0);
    stats.packetsLost.store(5);
    // Nothing received at all is reported as zero loss, not as total loss:
    // received == 0 short-circuits before the lost count is even read.
    CHECK(stats.getPacketLossPercent() == doctest::Approx(0.0));
}

TEST_CASE("A stream that never received a packet reports a negative age") {
    Statistics stats;
    CHECK(stats.timeSinceLastPacketMs() == -1);
}

TEST_CASE("The age of the last packet grows with the clock") {
    Statistics stats;
    const auto now = std::chrono::steady_clock::now();
    const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              now.time_since_epoch()).count();

    // Stamped a full second in the past, so the answer is about 1000 ms
    // however long this test itself takes.
    stats.lastPacketTimeNs.store(nowNs - 1000000000LL);
    const int64_t age = stats.timeSinceLastPacketMs();
    CHECK(age >= 1000);
    CHECK(age < 60000);
}

TEST_CASE("A snapshot carries every counter across") {
    Statistics stats;
    stats.packetsReceived.store(1);
    stats.packetsLost.store(2);
    stats.malformedPackets.store(3);
    stats.outOfOrderPackets.store(4);
    stats.underruns.store(5);
    stats.overruns.store(6);
    stats.jitterNs.store(7);
    stats.latencyNs.store(8);
    stats.bytesReceived.store(9);
    stats.bytesSent.store(10);
    stats.lastPacketTimeNs.store(11);

    const StatisticsSnapshot snap = stats.snapshot();
    CHECK(snap.packetsReceived == 1);
    CHECK(snap.packetsLost == 2);
    CHECK(snap.malformedPackets == 3);
    CHECK(snap.outOfOrderPackets == 4);
    CHECK(snap.underruns == 5);
    CHECK(snap.overruns == 6);
    CHECK(snap.jitterNs == 7);
    CHECK(snap.latencyNs == 8);
    CHECK(snap.bytesReceived == 9);
    CHECK(snap.bytesSent == 10);
    CHECK(snap.lastPacketTimeNs == 11);
}

TEST_CASE("A snapshot answers the same questions as the live counters") {
    Statistics stats;
    stats.packetsReceived.store(75);
    stats.packetsLost.store(25);

    const StatisticsSnapshot snap = stats.snapshot();
    CHECK(snap.getPacketLossPercent() == doctest::Approx(25.0));
    CHECK(snap.getPacketLossPercent() ==
          doctest::Approx(stats.getPacketLossPercent()));
    CHECK(snap.timeSinceLastPacketMs() == -1);
}

// ---------------------------------------------------------------------------
// NetworkAddress
// ---------------------------------------------------------------------------

TEST_CASE("An address needs both a host and a port to be valid") {
    NetworkAddress address{"239.69.0.1", 5004};
    CHECK(address.isValid());

    address.port = 0;
    CHECK_FALSE(address.isValid());

    address.port = 5004;
    address.ip.clear();
    CHECK_FALSE(address.isValid());
}

TEST_CASE("An address prints as host and port") {
    const NetworkAddress address{"192.168.1.10", 5004};
    CHECK(address.toString() == "192.168.1.10:5004");
}

TEST_CASE("Multicast is the whole 224-to-239 range, AES67 only the top of it") {
    CHECK(NetworkAddress{"224.0.0.1", 5004}.isMulticast());
    CHECK(NetworkAddress{"239.69.0.1", 5004}.isMulticast());
    CHECK_FALSE(NetworkAddress{"192.168.1.10", 5004}.isMulticast());
    CHECK_FALSE(NetworkAddress{"240.0.0.1", 5004}.isMulticast());

    // AES67 recommends 239.x.x.x, so the rest of the multicast range is
    // multicast without being AES67 multicast.
    CHECK(NetworkAddress{"239.69.0.1", 5004}.isAES67Multicast());
    CHECK_FALSE(NetworkAddress{"224.0.0.1", 5004}.isAES67Multicast());
    CHECK_FALSE(NetworkAddress{"192.168.1.10", 5004}.isAES67Multicast());
}

// ---------------------------------------------------------------------------
// PTPConfig, StreamInfo, DeviceConfig
// ---------------------------------------------------------------------------

TEST_CASE("A PTP domain outside 0 to 127 is not a valid configuration") {
    PTPConfig config;
    CHECK(config.isValid());          // domain 0 by default

    config.domain = 127;
    CHECK(config.isValid());

    config.domain = 128;
    CHECK_FALSE(config.isValid());

    config.domain = -1;               // the header's "no PTP" value
    CHECK_FALSE(config.isValid());
}

namespace {

// A stream that passes every clause of StreamInfo::isValid, so each case can
// break exactly one of them.
StreamInfo validStream() {
    StreamInfo stream;
    stream.id = StreamID::generate();
    stream.name = "Studio A";
    stream.multicast = NetworkAddress{"239.69.0.1", 5004};
    stream.encoding = AudioEncoding::L24;
    stream.sampleRate = 48000;
    stream.numChannels = 8;
    return stream;
}

} // namespace

TEST_CASE("A stream is valid only with an identifier, a name, a group and a format") {
    CHECK(validStream().isValid());

    SUBCASE("the null identifier disqualifies it") {
        StreamInfo stream = validStream();
        stream.id = StreamID::null();
        CHECK_FALSE(stream.isValid());
    }
    SUBCASE("an unnamed stream disqualifies it") {
        StreamInfo stream = validStream();
        stream.name.clear();
        CHECK_FALSE(stream.isValid());
    }
    SUBCASE("an unusable multicast address disqualifies it") {
        StreamInfo stream = validStream();
        stream.multicast.port = 0;
        CHECK_FALSE(stream.isValid());
    }
    SUBCASE("an unknown encoding disqualifies it") {
        StreamInfo stream = validStream();
        stream.encoding = AudioEncoding::Unknown;
        CHECK_FALSE(stream.isValid());
    }
    SUBCASE("a zero sample rate disqualifies it") {
        StreamInfo stream = validStream();
        stream.sampleRate = 0;
        CHECK_FALSE(stream.isValid());
    }
    SUBCASE("a channel-less stream disqualifies it") {
        StreamInfo stream = validStream();
        stream.numChannels = 0;
        CHECK_FALSE(stream.isValid());
    }
}

TEST_CASE("The default device configuration is valid, and each field can break it") {
    CHECK(DeviceConfig{}.isValid());

    SUBCASE("no sample rate") {
        DeviceConfig config;
        config.sampleRate = 0.0;
        CHECK_FALSE(config.isValid());
    }
    SUBCASE("no buffer") {
        DeviceConfig config;
        config.bufferSize = 0;
        CHECK_FALSE(config.isValid());
    }
    SUBCASE("no ring buffer") {
        DeviceConfig config;
        config.ringBufferSize = 0;
        CHECK_FALSE(config.isValid());
    }
    SUBCASE("no device name") {
        DeviceConfig config;
        config.deviceName.clear();
        CHECK_FALSE(config.isValid());
    }
    SUBCASE("no device UID") {
        DeviceConfig config;
        config.deviceUID.clear();
        CHECK_FALSE(config.isValid());
    }
}

// ---------------------------------------------------------------------------
// Error
// ---------------------------------------------------------------------------

TEST_CASE("An error prints its numeric code and its message") {
    const Error error(ErrorCode::Success, "nothing went wrong");
    CHECK(error.isSuccess());
    CHECK(error.toString() == "Error 0: nothing went wrong");
}

TEST_CASE("Context is appended in parentheses, and left out when there is none") {
    const Error without(ErrorCode::SDPParseError, "unparsable session");
    CHECK_FALSE(without.isSuccess());
    CHECK(without.toString().find('(') == std::string::npos);
    CHECK(without.toString().find(": unparsable session") != std::string::npos);

    const Error with(ErrorCode::SDPParseError, "unparsable session", "SDPParser");
    CHECK(with.toString() ==
          without.toString() + " (SDPParser)");
}

TEST_CASE("An error carries a message of its own only if it was given one") {
    const Error bare(ErrorCode::InternalError);
    CHECK(bare.message.empty());
    CHECK(bare.context.empty());
    CHECK(bare.toString().substr(0, 6) == "Error ");
}

// ---------------------------------------------------------------------------
// Utils
// ---------------------------------------------------------------------------

TEST_CASE("Sample rates convert both ways") {
    using AES67::Utils::hzToSampleRate;
    using AES67::Utils::sampleRateToHz;

    CHECK(sampleRateToHz(SampleRate::SR_44100) == 44100);
    CHECK(sampleRateToHz(SampleRate::SR_48000) == 48000);
    CHECK(sampleRateToHz(SampleRate::SR_88200) == 88200);
    CHECK(sampleRateToHz(SampleRate::SR_96000) == 96000);
    CHECK(sampleRateToHz(SampleRate::SR_176400) == 176400);
    CHECK(sampleRateToHz(SampleRate::SR_192000) == 192000);
    CHECK(sampleRateToHz(SampleRate::SR_352800) == 352800);
    CHECK(sampleRateToHz(SampleRate::SR_384000) == 384000);

    CHECK(hzToSampleRate(44100) == SampleRate::SR_44100);
    CHECK(hzToSampleRate(48000) == SampleRate::SR_48000);
    CHECK(hzToSampleRate(88200) == SampleRate::SR_88200);
    CHECK(hzToSampleRate(96000) == SampleRate::SR_96000);
    CHECK(hzToSampleRate(176400) == SampleRate::SR_176400);
    CHECK(hzToSampleRate(192000) == SampleRate::SR_192000);
    CHECK(hzToSampleRate(352800) == SampleRate::SR_352800);
    CHECK(hzToSampleRate(384000) == SampleRate::SR_384000);
}

TEST_CASE("A rate this does not know falls back to 48 kHz rather than failing") {
    using AES67::Utils::hzToSampleRate;

    // The function has no way to say "no", so an unknown rate silently
    // becomes the AES67 default. A caller that needs to reject one has to
    // compare the round trip.
    CHECK(hzToSampleRate(0) == SampleRate::SR_48000);
    CHECK(hzToSampleRate(32000) == SampleRate::SR_48000);
    CHECK(hzToSampleRate(768000) == SampleRate::SR_48000);
}

TEST_CASE("An IPv4 address is four dotted octets in range") {
    using AES67::Utils::isValidIPv4;

    CHECK(isValidIPv4("0.0.0.0"));
    CHECK(isValidIPv4("255.255.255.255"));
    CHECK(isValidIPv4("192.168.1.10"));

    CHECK_FALSE(isValidIPv4(""));
    CHECK_FALSE(isValidIPv4("192.168.1"));
    CHECK_FALSE(isValidIPv4("192.168.1.10.5"));
    CHECK_FALSE(isValidIPv4("192.168.1."));
    CHECK_FALSE(isValidIPv4("256.0.0.1"));
    CHECK_FALSE(isValidIPv4("1.2.3.999"));
    CHECK_FALSE(isValidIPv4("host.example.com"));
    CHECK_FALSE(isValidIPv4("192.168.1.-1"));
    CHECK_FALSE(isValidIPv4(" 192.168.1.10"));
    CHECK_FALSE(isValidIPv4("192.168.1.10 "));
    CHECK_FALSE(isValidIPv4("::1"));
}

TEST_CASE("Octets written with leading zeros are accepted") {
    using AES67::Utils::isValidIPv4;

    // inet_pton rejects these and some resolvers read them as octal, so this
    // is looser than the platform is. Pinned here because anything relying on
    // the check would change behaviour if it were tightened.
    CHECK(isValidIPv4("010.001.000.001"));
}

TEST_CASE("A malformed address is neither multicast nor AES67 multicast") {
    using AES67::Utils::isAES67MulticastIP;
    using AES67::Utils::isMulticastIP;

    CHECK_FALSE(isMulticastIP("not an address"));
    CHECK_FALSE(isMulticastIP(""));
    CHECK_FALSE(isAES67MulticastIP("not an address"));
    CHECK_FALSE(isAES67MulticastIP(""));
}

TEST_CASE("The multicast range is bounded at both ends") {
    using AES67::Utils::isMulticastIP;

    CHECK_FALSE(isMulticastIP("223.255.255.255"));
    CHECK(isMulticastIP("224.0.0.0"));
    CHECK(isMulticastIP("239.255.255.255"));
    CHECK_FALSE(isMulticastIP("240.0.0.0"));
}

TEST_CASE("The three clocks agree with each other and move forward") {
    using AES67::Utils::getMicroseconds;
    using AES67::Utils::getMilliseconds;
    using AES67::Utils::getNanoseconds;

    const uint64_t ns = getNanoseconds();
    const uint64_t us = getMicroseconds();
    const uint64_t ms = getMilliseconds();

    CHECK(ns > 0);
    CHECK(us > 0);
    CHECK(ms > 0);

    // Same clock read in three units: the coarser readings have to be within
    // one tick of the finer one divided down.
    CHECK(us >= ns / 1000 - 1000);
    CHECK(ms >= us / 1000 - 1000);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(getNanoseconds() > ns);
}

TEST_CASE("Byte counts are printed in the largest unit that fits") {
    using AES67::Utils::formatBytes;

    CHECK(formatBytes(0) == "0.00 B");
    CHECK(formatBytes(512) == "512.00 B");
    CHECK(formatBytes(1023) == "1023.00 B");
    CHECK(formatBytes(1024) == "1.00 KB");
    CHECK(formatBytes(1536) == "1.50 KB");
    CHECK(formatBytes(1024ULL * 1024) == "1.00 MB");
    CHECK(formatBytes(1024ULL * 1024 * 1024) == "1.00 GB");
    CHECK(formatBytes(1024ULL * 1024 * 1024 * 1024) == "1.00 TB");
}

TEST_CASE("Terabytes are the largest unit, so past them the number keeps growing") {
    using AES67::Utils::formatBytes;

    // The unit table stops at TB, and the loop stops with it rather than
    // running off the end of the array.
    CHECK(formatBytes(4096ULL * 1024 * 1024 * 1024 * 1024) == "4096.00 TB");
}

TEST_CASE("A duration prints only the units it needs") {
    using AES67::Utils::formatDuration;
    using std::chrono::milliseconds;

    CHECK(formatDuration(milliseconds(0)) == "0s");
    CHECK(formatDuration(milliseconds(999)) == "0s");
    CHECK(formatDuration(milliseconds(1500)) == "1s");
    CHECK(formatDuration(milliseconds(59000)) == "59s");
    CHECK(formatDuration(milliseconds(60000)) == "1m 0s");
    CHECK(formatDuration(milliseconds(90000)) == "1m 30s");
    CHECK(formatDuration(milliseconds(3600000)) == "1h 0m 0s");
    CHECK(formatDuration(milliseconds(3661000)) == "1h 1m 1s");
}

TEST_CASE("Minutes are printed once there are hours, even when there are none") {
    using AES67::Utils::formatDuration;
    using std::chrono::milliseconds;

    // 2h 0m 5s rather than 2h 5s: a zero minute count is kept so the reading
    // stays unambiguous.
    CHECK(formatDuration(milliseconds(7205000)) == "2h 0m 5s");
}
