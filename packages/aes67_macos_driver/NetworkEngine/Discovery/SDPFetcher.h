//
// SDPFetcher.h
// AES67 macOS Driver
//
// Reads a session description from wherever it is published, not only from
// a file on this Mac. The reference Linux daemon fetches a sink's SDP from
// an HTTP or RTSP URL (its use_sdp/source options); we could only read
// local files, so setting up a receiver from a cinema processor that
// publishes its own SDP meant copying the text across by hand.
//
// Three sources, one entry point:
//   file:///path/to.sdp   or a bare path        -- read it
//   http://host[:port]/p                        -- GET it
//   rtsp://host[:port]/p                        -- DESCRIBE it
//
// https:// is refused with a clear message rather than silently: this
// layer speaks BSD sockets and has no TLS, and pretending otherwise would
// fail at a worse moment.
//
// Everything here can be reached by whoever runs the server at the other
// end, and it is linked into coreaudiod, so the parsing is defensive: no
// exceptions escape, no unbounded body, and a timeout on every socket.
//
#pragma once

#include <cstdint>
#include <string>

namespace AES67 {

/// What came back, or why nothing did. `text` is empty exactly when
/// `error` is not.
struct SDPFetchResult {
    std::string text;
    std::string error;

    bool ok() const { return error.empty(); }
};

class SDPFetcher {
public:
    /// The largest description this will accept. An SDP is bytes; a server
    /// announcing megabytes is either broken or hostile.
    static constexpr size_t kMaxSDPBytes = 1 << 20; // 1 MiB

    /// Default timeout for the network schemes.
    static constexpr int kDefaultTimeoutMs = 3000;

    /// Fetches the description named by `url`. Never throws.
    static SDPFetchResult fetch(const std::string& url,
                                int timeoutMs = kDefaultTimeoutMs);

    /// The pieces of a URL. Exposed because the dispatch is worth testing
    /// on its own, without a server at the other end.
    struct URLParts {
        std::string scheme;   ///< lowercased, empty for a bare path
        std::string host;
        uint16_t port{0};     ///< the scheme's default when the URL omits it
        std::string path;     ///< always starts with '/' for the network schemes
    };

    /// Splits `url`. Returns false when it is not a URL this can use; a
    /// bare path is not a failure, it comes back with an empty scheme.
    static bool parseURL(const std::string& url, URLParts& out);

private:
    static SDPFetchResult fetchFile(const std::string& path);
    static SDPFetchResult fetchHTTP(const URLParts& parts, int timeoutMs);
    static SDPFetchResult fetchRTSP(const std::string& url, const URLParts& parts,
                                    int timeoutMs);
};

} // namespace AES67
