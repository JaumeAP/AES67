//
// FuzzDriverParsers.cpp
// aes67-macos-driver
//
// Everything in this driver that reads bytes it did not write, fed what
// nobody wrote a test for: a SAP announcement off the multicast group, an
// RTSP response and URL from a device it asked, an IS-05 PATCH body from a
// controller, an SDP fetcher's URL, and an RTP frame off the wire. Each of
// them runs inside coreaudiod, where a crash is the audio of every app on
// the machine.
//
// Two entry points over one function. LLVMFuzzerTestOneInput is for
// `-fsanitize=fuzzer` (-DAES67_LIBFUZZER=ON), which CI has and Apple's clang
// does not ship. main is a deterministic driver over the same function --
// fixed seed, printed -- so the check runs everywhere and a failure is one
// command away from being reproduced.
//
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "NetworkEngine/Discovery/SAPListener.h"
#include "NetworkEngine/Discovery/RTSPClient.h"
#include "NetworkEngine/Discovery/ConnectionAPIServer.h"
#include "NetworkEngine/Discovery/SDPFetcher.h"
#include "NetworkEngine/RTP/SimpleRTP.h"
#include "Testing/FuzzDriver.h"

using namespace AES67;

namespace {

std::vector<std::vector<uint8_t>> seedCorpus() {
    auto bytes = [](const char* s) { return std::vector<uint8_t>(s, s + std::char_traits<char>::length(s)); };
    // A SAP announcement: version 1, IPv4, no auth, hash, origin, the MIME
    // type Dante insists on, then an SDP.
    std::vector<uint8_t> sap{0x20, 0x00, 0x12, 0x34, 192, 168, 0, 10};
    for (char c : std::string("application/sdp")) sap.push_back((uint8_t)c);
    sap.push_back(0);
    for (char c : std::string("v=0\r\no=- 1 1 IN IP4 192.168.0.10\r\ns=Seed\r\nc=IN IP4 239.1.2.3/32\r\nt=0 0\r\n"
                              "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 L24/48000/2\r\na=ptime:1\r\n")) sap.push_back((uint8_t)c);
    // An RTP frame: V=2, PT 96, seq 1, timestamp, SSRC, 12 bytes of L24.
    std::vector<uint8_t> rtp{0x80, 0x60, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0xde, 0xad, 0xbe, 0xef};
    rtp.resize(12 + 12, 0x11);
    return {
        sap,
        rtp,
        bytes("RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Type: application/sdp\r\nContent-Length: 4\r\n\r\nv=0\n"),
        bytes("rtsp://192.168.0.10:8554/by-name/Seed"),
        bytes("http://192.168.0.10:80/sdp/Seed.sdp"),
        bytes("{\"master_enable\":true,\"activation\":{\"mode\":\"activate_immediate\"},"
              "\"transport_file\":{\"data\":\"v=0\\r\\n\",\"type\":\"application/sdp\"},"
              "\"transport_params\":[{\"multicast_ip\":\"239.1.2.3\",\"destination_port\":5004}]}"),
    };
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 16384) return 0;
    const std::string text(reinterpret_cast<const char*>(data), size);

    (void)SAPListener::parseAnnouncement(reinterpret_cast<const char*>(data), size, "192.168.0.10");
    (void)RTSPClient::parseResponse(text);
    { std::string host, path; uint16_t port = 0; (void)RTSPClient::parseURL(text, host, port, path); }
    (void)ConnectionAPIServer::parsePatch(text);
    { SDPFetcher::URLParts parts; (void)SDPFetcher::parseURL(text, parts); }
    { RTP::RTPPacket packet{}; (void)RTP::RTPSocket::parseFrame(data, size, packet); }
    return 0;
}

#ifndef FUZZ_LIBFUZZER
int main(int argc, char** argv) {
    return AES67::Testing::runFuzzDriver(argc, argv, seedCorpus(), 1500);
}
#endif
