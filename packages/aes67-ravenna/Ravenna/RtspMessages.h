//
// RtspMessages.h
// AES67 RAVENNA session layer
// The RTSP a RAVENNA discovery exchange actually uses, and nothing else.
//
// RAVENNA does not stream over RTSP. What it uses RTSP for is one question --
// "describe the session you advertised" -- answered with the SDP of a
// multicast stream that is already on the wire whether anybody asked or not.
// So there is no SETUP, no PLAY, no session state and no transport
// negotiation here: a device DESCRIBEs, gets the SDP, and joins the group
// itself.
//
// Platform-free, because a parser that needs a socket to be tested is a
// parser that does not get tested.
//
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace AES67::Ravenna {

/// The methods this server answers. Everything else gets 501, which is the
/// honest answer for a server that implements two.
enum class RtspMethod {
    Options,
    Describe,
    Unsupported,
};

struct RtspRequest {
    RtspMethod method = RtspMethod::Unsupported;
    std::string methodName;   ///< as it arrived, for the log and the 501
    std::string uri;
    std::string version;      ///< "RTSP/1.0"
    /// Lowercased header names: RFC 2326 says they are case insensitive and a
    /// client that sends "cseq" is not wrong.
    std::map<std::string, std::string> headers;

    /// The CSeq a response has to echo. Zero when the request carried none,
    /// which is itself a malformed request.
    uint32_t sequence = 0;
};

/// Parses the request line and the headers. The body is not read: neither
/// method this server answers has one.
///
/// False means the text is not an RTSP request at all -- not that the method
/// is unknown, which parses fine and comes back as Unsupported.
bool parseRtspRequest(const std::string& text, RtspRequest& out);

/// A URI path with its percent escapes resolved. A session called "Mix A" is
/// advertised with a space and asked for as %20, and comparing the two
/// without this answers 404 to a client that asked correctly.
///
/// Anything that is not a valid escape is left as it stands: this reads text
/// off a socket, and a stray % is a malformed path, not a reason to throw.
///
/// This decodes %2F to a slash, which HttpServer.cpp's decodePath does not.
/// The difference is what happens next: an RTSP URL is decoded whole and
/// compared against a session name, so a slash in it is a character like any
/// other, while an HTTP path is decoded and then re-split on its separators,
/// so an escaped slash that became a real one would silently name a different
/// resource. Two rules, two functions, on purpose.
std::string percentDecode(const std::string& text);

/// True once the text holds a complete header block, which is what tells a
/// reader to stop reading. RTSP has no length to read ahead of.
bool hasCompleteHeaders(const std::string& text);

/// A response with a body, whose Content-Length is the body's length in
/// bytes and not in anything else.
std::string buildRtspResponse(uint32_t sequence, int statusCode,
                              const std::string& statusText,
                              const std::map<std::string, std::string>& headers,
                              const std::string& body);

/// 200 with the SDP, Content-Type application/sdp. `contentBase` is the URI
/// the description was asked for, which RFC 2326 says a DESCRIBE answer
/// should carry so relative URIs inside resolve.
std::string buildDescribeResponse(uint32_t sequence, const std::string& contentBase,
                                  const std::string& sdp);

/// 200 with the Public header. What a client asks OPTIONS for.
std::string buildOptionsResponse(uint32_t sequence);

/// 501 Not Implemented, for a method this server does not answer.
std::string buildNotImplementedResponse(uint32_t sequence);

/// 404, for a URI that names no session here.
std::string buildNotFoundResponse(uint32_t sequence);

}  // namespace AES67::Ravenna
