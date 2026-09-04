//
// TestRTSPClient.cpp
// AES67 macOS Driver
//
// The two halves of the RTSP client that read text somebody else wrote: the
// URL it is pointed at, and the response a server sends back.
//
// Both were at zero coverage until 2026-09-04, and both have already
// produced defects of the same kind — a std::stoi on a port that is not a
// number, a Content-Length taken at its word — which inside coreaudiod is
// std::terminate for the whole audio daemon. What follows is mostly about
// what these two refuse.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/RTSPClient.h"

#include <string>

using namespace AES67;

namespace {

struct URL {
    std::string host;
    uint16_t port{0};
    std::string path;
    bool parsed{false};
};

URL split(const std::string& url) {
    URL out;
    out.parsed = RTSPClient::parseURL(url, out.host, out.port, out.path);
    return out;
}

} // namespace

TEST_CASE("A URL splits into host, port and path") {
    const URL full = split("rtsp://192.168.1.50:8554/by-name/Studio Mic");
    REQUIRE(full.parsed);
    CHECK(full.host == "192.168.1.50");
    CHECK(full.port == 8554);
    CHECK(full.path == "/by-name/Studio Mic");

    // No port: RTSP's own default, not zero.
    const URL defaulted = split("rtsp://cp850.local/stream");
    REQUIRE(defaulted.parsed);
    CHECK(defaulted.host == "cp850.local");
    CHECK(defaulted.port == 554);
    CHECK(defaulted.path == "/stream");

    // No path: the root.
    const URL bare = split("rtsp://cp850.local");
    REQUIRE(bare.parsed);
    CHECK(bare.host == "cp850.local");
    CHECK(bare.path == "/");
}

TEST_CASE("A port that is not a port is refused, not guessed") {
    // std::stoi threw on each of these, and a URL reaches this from a
    // settings file or a controller.
    CHECK_FALSE(split("rtsp://host:notaport/stream").parsed);
    CHECK_FALSE(split("rtsp://host:/stream").parsed);
    CHECK_FALSE(split("rtsp://host:99999/stream").parsed);
    CHECK_FALSE(split("rtsp://host:0/stream").parsed);
    CHECK_FALSE(split("rtsp://host:554abc/stream").parsed);
    CHECK_FALSE(split("rtsp://host:99999999999999999999/stream").parsed);
}

TEST_CASE("A response is a status, headers and a body") {
    const std::string text =
        "RTSP/1.0 200 OK\r\n"
        "CSeq: 3\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: 15\r\n"
        "\r\n"
        "v=0\r\ns=Studio\r\n";

    const auto response = RTSPClient::parseResponse(text);
    REQUIRE(response.has_value());
    CHECK(response->statusCode == 200);
    CHECK(response->statusMessage == "OK");
    CHECK(response->isSuccess());
    CHECK(response->headers.at("CSeq") == "3");
    CHECK(response->headers.at("Content-Type") == "application/sdp");
    CHECK(response->body == "v=0\r\ns=Studio\r\n");
}

TEST_CASE("A failure status is read as one") {
    const auto notFound = RTSPClient::parseResponse(
        "RTSP/1.0 404 Not Found\r\nCSeq: 4\r\nContent-Length: 0\r\n\r\n");
    REQUIRE(notFound.has_value());
    CHECK(notFound->statusCode == 404);
    CHECK_FALSE(notFound->isSuccess());
    CHECK(notFound->body.empty());

    const auto unimplemented =
        RTSPClient::parseResponse("RTSP/1.0 501 Not Implemented\r\nCSeq: 5\r\n\r\n");
    REQUIRE(unimplemented.has_value());
    CHECK(unimplemented->statusCode == 501);
    CHECK_FALSE(unimplemented->isSuccess());
}

TEST_CASE("A Content-Length this will not honour leaves the body empty") {
    // The header arrives from an unauthenticated server and drives a
    // resize(). A gigabyte is a request to allocate one, and garbage is not
    // a number to trust; both end with the response returned and no body,
    // rather than an exception out of coreaudiod.
    const auto huge = RTSPClient::parseResponse(
        "RTSP/1.0 200 OK\r\nContent-Length: 1073741824\r\n\r\nv=0\r\n");
    REQUIRE(huge.has_value());
    CHECK(huge->statusCode == 200);
    CHECK(huge->body.empty());

    const auto garbage = RTSPClient::parseResponse(
        "RTSP/1.0 200 OK\r\nContent-Length: soon\r\n\r\nv=0\r\n");
    REQUIRE(garbage.has_value());
    CHECK(garbage->body.empty());

    const auto negative = RTSPClient::parseResponse(
        "RTSP/1.0 200 OK\r\nContent-Length: -1\r\n\r\nv=0\r\n");
    REQUIRE(negative.has_value());
    CHECK(negative->body.empty());
}

TEST_CASE("A body shorter than declared is what arrived, not zero padding") {
    // resize() to the declared length and a short read leaves the rest as
    // NULs, which an SDP parser would then be handed as if the server had
    // sent them.
    const auto response = RTSPClient::parseResponse(
        "RTSP/1.0 200 OK\r\nContent-Length: 64\r\n\r\nv=0\r\n");
    REQUIRE(response.has_value());
    CHECK(response->body == "v=0\r\n");
}

TEST_CASE("Without a Content-Length the body is what is left") {
    // Some servers just close the connection instead of counting.
    const auto response = RTSPClient::parseResponse(
        "RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\nv=0\r\ns=No Length\r\n");
    REQUIRE(response.has_value());
    CHECK(response->statusCode == 200);
    CHECK(response->body == "v=0\r\ns=No Length\r\n");
}

TEST_CASE("Header values are trimmed, and a line without a colon is not one") {
    const auto response = RTSPClient::parseResponse(
        "RTSP/1.0 200 OK\r\n"
        "CSeq:    7   \r\n"
        "this line has no colon\r\n"
        "Session: 12345678\r\n"
        "\r\n");
    REQUIRE(response.has_value());
    CHECK(response->headers.at("CSeq") == "7");
    CHECK(response->headers.at("Session") == "12345678");
    CHECK(response->headers.count("this line has no colon") == 0);
}

TEST_CASE("Nothing where a response goes is nothing, not a zero") {
    // An empty read has to come back as no response at all: a
    // default-constructed one would read as status 0 and be mistaken for a
    // server that answered.
    CHECK_FALSE(RTSPClient::parseResponse("").has_value());
}
