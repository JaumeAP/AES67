//
// TestRtspMessages.cpp
// AES67 RAVENNA session layer
// The two questions this server answers, and everything a device might send
// that is neither.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Ravenna/RtspMessages.h"

using namespace AES67::Ravenna;

TEST_CASE("A DESCRIBE parses into its method, its URI and its CSeq") {
    const std::string text =
        "DESCRIBE rtsp://192.168.1.50:554/by-name/Mix%20A RTSP/1.0\r\n"
        "CSeq: 3\r\n"
        "Accept: application/sdp\r\n"
        "User-Agent: Merging\r\n"
        "\r\n";

    RtspRequest request;
    REQUIRE(parseRtspRequest(text, request));
    CHECK(request.method == RtspMethod::Describe);
    CHECK(request.uri == "rtsp://192.168.1.50:554/by-name/Mix%20A");
    CHECK(request.sequence == 3);
    CHECK(request.headers.at("accept") == "application/sdp");
}

TEST_CASE("Header names are case insensitive, because RFC 2326 says so") {
    const std::string text = "OPTIONS * RTSP/1.0\r\ncseq: 11\r\n\r\n";

    RtspRequest request;
    REQUIRE(parseRtspRequest(text, request));
    CHECK(request.method == RtspMethod::Options);
    CHECK(request.sequence == 11);
}

TEST_CASE("A method this server does not answer still parses") {
    // It has to: the difference between "not RTSP" and "RTSP asking for
    // something else" is the difference between silence and a 501.
    const std::string text = "SETUP rtsp://host/by-name/x RTSP/1.0\r\nCSeq: 4\r\n\r\n";

    RtspRequest request;
    REQUIRE(parseRtspRequest(text, request));
    CHECK(request.method == RtspMethod::Unsupported);
    CHECK(request.methodName == "SETUP");
    CHECK(request.sequence == 4);
}

TEST_CASE("What is not RTSP is refused") {
    RtspRequest request;
    CHECK(parseRtspRequest("GET / HTTP/1.1\r\nHost: x\r\n\r\n", request) == false);
    CHECK(parseRtspRequest("", request) == false);
    CHECK(parseRtspRequest("garbage\r\n\r\n", request) == false);
}

TEST_CASE("A request with no CSeq comes back as zero rather than as a guess") {
    RtspRequest request;
    REQUIRE(parseRtspRequest("OPTIONS * RTSP/1.0\r\n\r\n", request));
    CHECK(request.sequence == 0);

    REQUIRE(parseRtspRequest("OPTIONS * RTSP/1.0\r\nCSeq: nonsense\r\n\r\n", request));
    CHECK(request.sequence == 0);
}

TEST_CASE("The end of the headers is what tells a reader to stop") {
    CHECK(hasCompleteHeaders("DESCRIBE x RTSP/1.0\r\nCSeq: 1\r\n") == false);
    CHECK(hasCompleteHeaders("DESCRIBE x RTSP/1.0\r\nCSeq: 1\r\n\r\n"));
    CHECK(hasCompleteHeaders("DESCRIBE x RTSP/1.0\nCSeq: 1\n\n"));
}

TEST_CASE("A DESCRIBE answer carries the SDP, its length and its base") {
    const std::string sdp = "v=0\r\no=- 1 1 IN IP4 192.168.1.50\r\ns=Mix A\r\n";
    const std::string response =
        buildDescribeResponse(7, "rtsp://192.168.1.50:554/by-name/Mix A", sdp);

    CHECK(response.find("RTSP/1.0 200 OK\r\n") == 0);
    CHECK(response.find("CSeq: 7\r\n") != std::string::npos);
    CHECK(response.find("Content-Type: application/sdp\r\n") != std::string::npos);
    CHECK(response.find("Content-Base: rtsp://192.168.1.50:554/by-name/Mix A\r\n") !=
          std::string::npos);
    CHECK(response.find("Content-Length: " + std::to_string(sdp.size()) + "\r\n") !=
          std::string::npos);
    // The body starts after the blank line and is the SDP unchanged.
    const auto blank = response.find("\r\n\r\n");
    REQUIRE(blank != std::string::npos);
    CHECK(response.substr(blank + 4) == sdp);
}

TEST_CASE("An empty body carries no Content-Length at all") {
    // Not "Content-Length: 0": a length header on a response with no body is
    // what makes a client wait for a body that is not coming.
    const std::string response = buildOptionsResponse(1);
    CHECK(response.find("Content-Length") == std::string::npos);
    CHECK(response.find("Public: OPTIONS, DESCRIBE\r\n") != std::string::npos);
}

TEST_CASE("A path is compared with its escapes resolved") {
    // A session called "Mix A" is advertised with a space and asked for as
    // %20. Comparing the two as they arrive answers 404 to a client that did
    // exactly the right thing.
    CHECK(percentDecode("/by-name/Mix%20A") == "/by-name/Mix A");
    CHECK(percentDecode("/by-name/Mix%2fB") == "/by-name/Mix/B");
    CHECK(percentDecode("/by-name/Plain") == "/by-name/Plain");

    // A malformed escape stays as it stands: this reads text off a socket.
    CHECK(percentDecode("100%") == "100%");
    CHECK(percentDecode("%zz") == "%zz");
    CHECK(percentDecode("%2") == "%2");
}

TEST_CASE("The refusals say which refusal they are") {
    CHECK(buildNotImplementedResponse(2).find("RTSP/1.0 501 Not Implemented\r\n") == 0);
    CHECK(buildNotFoundResponse(2).find("RTSP/1.0 404 Not Found\r\n") == 0);
    CHECK(buildNotFoundResponse(2).find("CSeq: 2\r\n") != std::string::npos);
}
