//
// FuzzParsers.cpp
// aes67-ravenna
//
// The four parsers that read bytes off a socket -- RTSP, JSON, HTTP and the DNS-SD query, fed what nobody wrote a test for.
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

#include "Ravenna/RtspMessages.h"
#include "Ravenna/Json.h"
#include "Ravenna/HttpServer.h"
#include "Ravenna/DnsSd.h"
#include "Testing/FuzzDriver.h"

using namespace AES67::Ravenna;



namespace {

std::vector<std::vector<uint8_t>> seedCorpus() {
    auto bytes = [](const char* s) { return std::vector<uint8_t>(s, s + std::char_traits<char>::length(s)); };
    return {
        bytes("DESCRIBE rtsp://127.0.0.1:18999/by-name/GateSession RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n"),
        bytes("OPTIONS * RTSP/1.0\r\nCSeq: 2\r\n\r\n"),
        bytes("GET /x-nmos/node/v1.3/senders/ HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n"),
        bytes("PATCH /x-nmos/connection/v1.1/single/receivers/abc/staged HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}"),
        bytes("{\"id\":\"a\",\"label\":\"x\",\"n\":[1,2.5,-3e2,true,false,null],\"o\":{\"k\":\"v\\n\"}}"),
        bytes("[[[]]]"),
        // A DNS query for _rtsp._tcp.local: header, one question.
        std::vector<uint8_t>{0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
                             5,'_','r','t','s','p',4,'_','t','c','p',5,'l','o','c','a','l',0,
                             0x00,0x0c,0x00,0x01},
    };
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 8192) return 0;
    const std::string text(reinterpret_cast<const char*>(data), size);

    { RtspRequest request; (void)parseRtspRequest(text, request); }
    { JsonValue value; std::string error; (void)parseJson(text, value, error); }
    { std::string method, path, body; (void)parseHttpRequest(text, method, path, body); }
    (void)parseQueryNames(data, size);
    return 0;
}

#ifndef FUZZ_LIBFUZZER
int main(int argc, char** argv) {
    return AES67::Testing::runFuzzDriver(argc, argv, seedCorpus(), 1024);
}
#endif
