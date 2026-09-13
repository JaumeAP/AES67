//
// DaemonRtsp.h
// AES67 macOS Driver - Tests
// The RTSP bytes the AES67 Linux daemon sends, and the ones it accepts.
//
// Mirrors the daemon's client (rtsp_client.cpp, RtspClient::process and
// read_response) and its server (rtsp_server.cpp, RtspSession::process_request,
// build_response and send_error) -- bondagit/aes67-linux-daemon, not vendored
// here. Its RTSP runs over a Boost.Asio tcp::iostream this cannot call
// directly, so this is the same approach as DaemonSap.h: what it puts on the
// wire, and what it accepts off it, reproduced from source so this driver's
// real RTSPClient and RTSPServer can be held against a real peer's rules
// rather than against RFC 2326 alone.
//
// The daemon's URL percent-encoding (rtsp_client.cpp's use of
// httplib::detail::encode_url) is narrower than RFC 3986: only space, '+',
// CR, LF, apostrophe, comma, semicolon and bytes >= 0x80 are escaped -- '/'
// and ':' pass through literally. Unchanged across the httplib releases
// checked (v0.9.6 through v0.14.3), so it is reproduced verbatim rather than
// approximated with a general-purpose encoder.
//
#pragma once

#include <cstdint>
#include <string>

namespace AES67 {
namespace Tests {

/// httplib::detail::encode_url, as the daemon's RTSP client applies it to a
/// request path before sending.
std::string daemonEncodeUrl(const std::string& text);

/// The DESCRIBE request the daemon's client sends (rtsp_client.cpp:152-156):
/// an absolute URL, an incrementing CSeq, and an Accept header naming the
/// only content type it goes on to read.
std::string daemonDescribeRequest(const std::string& host, uint16_t port,
                                  const std::string& path, int cseq);

/// What the daemon's client makes of a response to that request
/// (rtsp_client.cpp: RtspClient::process and read_response). A DESCRIBE is
/// accepted only on status 200 with a CSeq that echoes what was sent and a
/// Content-Type that starts with "application/sdp" once every header has
/// been lowercased -- which is also why the check is case-insensitive.
struct DaemonRtspDescribeResult {
    bool accepted{false};
    std::string sdp;
    std::string refusal;  ///< Which of the daemon's checks failed.
};

DaemonRtspDescribeResult daemonReadDescribeResponse(const std::string& response, int cseqSent);

/// The success response the daemon's server sends to a DESCRIBE it can
/// answer (rtsp_server.cpp: RtspSession::build_response, the 200 OK path).
std::string daemonDescribeOkResponse(int cseq, const std::string& sdp);

/// The daemon's error responses (rtsp_server.cpp: RtspSession::send_error),
/// carrying the CSeq the request named -- or none, when the request was
/// rejected before a CSeq could be read.
std::string daemonErrorResponse(int statusCode, const std::string& reason, int cseq);

/// What the daemon's server decides about one request line plus headers,
/// reproducing RtspSession::process_request's checks in order: at least
/// three space-separated fields on the request line; a CSeq header present;
/// the third field naming RTSP; the method is DESCRIBE. `pathExists` stands
/// in for session_manager_->get_source_sdp() -- the daemon answers 404
/// itself once the method and version are accepted, so the path lookup is
/// the caller's to supply.
struct DaemonRtspServerDecision {
    int statusCode{0};       ///< 0 means the daemon reads more and answers nothing yet.
    std::string reason;
    int cseq{-1};
    std::string path;        ///< The request-line path, once one was found.
};

DaemonRtspServerDecision daemonDecideRequest(const std::string& request, bool pathExists);

} // namespace Tests
} // namespace AES67
