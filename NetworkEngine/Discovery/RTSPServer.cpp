//
// RTSPServer.cpp
// AES67 macOS Driver
//
// A DESCRIBE-only RTSP endpoint over TCP. One acceptor thread, one
// request per connection, no session state — an AES67 stream is already
// running on its multicast group, so a client needs its description and
// nothing more.
//
// Every input here comes off the network from an unauthenticated peer,
// so the parsing below reads defensively by construction: a bounded
// read, a bounded request line, no allocation driven by a
// client-supplied length, and any malformed request answered with a
// status code rather than an exception (this code runs inside
// coreaudiod — an escaped exception is std::terminate for the whole
// audio daemon, the same trap the RTSPClient Content-Length fix
// addressed on 2026-08-31).
//

#include "NetworkEngine/Discovery/RTSPServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>

namespace AES67 {

namespace {

constexpr int kSelectTimeoutMs = 250;   ///< stop() responsiveness, as elsewhere.
constexpr int kClientTimeoutMs = 2000;  ///< A client that says nothing is dropped.

/// RTSP is CRLF-delimited and case-sensitive in its method names
/// (RFC 2326 §6.1). Headers end at a blank line.
constexpr const char* kCRLF = "\r\n";

std::string statusLine(int code, const std::string& reason, int cseq) {
    std::ostringstream out;
    out << "RTSP/1.0 " << code << " " << reason << kCRLF
        << "CSeq: " << cseq << kCRLF;
    return out.str();
}

/// The CSeq a client sent, echoed back on every response (RFC 2326 §12.17
/// makes it mandatory). 0 when absent — answering with 0 is better than
/// dropping a request that is otherwise well formed.
int parseCSeq(const std::string& request) {
    const std::string key = "cseq:";
    std::string lowered;
    lowered.reserve(request.size());
    for (char c : request) lowered.push_back(static_cast<char>(::tolower(c)));
    const size_t at = lowered.find(key);
    if (at == std::string::npos) return 0;
    size_t cursor = at + key.size();
    while (cursor < request.size() && (request[cursor] == ' ' || request[cursor] == '\t')) ++cursor;
    int value = 0;
    bool any = false;
    while (cursor < request.size() && request[cursor] >= '0' && request[cursor] <= '9') {
        if (value > 100000000) break; // absurd: stop rather than overflow
        value = value * 10 + (request[cursor] - '0');
        ++cursor;
        any = true;
    }
    return any ? value : 0;
}

/// Percent-decoding, RFC 3986 §2.1. A published path can carry any
/// character a session name does -- "Studio Mic 1" is ordinary -- and a
/// URL cannot carry a raw space, so a client sends "%20" and the server
/// has to undo it (2026-08-31, found by the RTSP server's own tests: a
/// raw space split the request line and the description was
/// unreachable). Malformed escapes are left verbatim rather than
/// rejected: this is a lookup key, and an unmatched key is already a
/// clean 404.
std::string percentDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hex(text[i + 1]);
            const int lo = hex(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

/// Splits "DESCRIBE rtsp://host:554/path RTSP/1.0" into method and path.
/// Returns false for anything that is not three space-separated tokens.
bool parseRequestLine(const std::string& request, std::string& method, std::string& path) {
    const size_t lineEnd = request.find(kCRLF);
    const std::string line = request.substr(0, lineEnd == std::string::npos ? request.size() : lineEnd);
    std::istringstream in(line);
    std::string url, version;
    if (!(in >> method >> url >> version)) return false;
    if (version.rfind("RTSP/", 0) != 0) return false;

    // The URL may be absolute ("rtsp://host:554/path") or just a path.
    const size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        path = url;
    } else {
        const size_t pathStart = url.find('/', schemeEnd + 3);
        path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    }
    if (path.empty()) path = "/";
    path = percentDecode(path);
    return true;
}

} // namespace

class RTSPServer::Impl {
public:
    explicit Impl(uint16_t port) : requestedPort_(port) {}
    ~Impl() { stop(); }

    bool start(RTSPStreamProvider provider) {
        if (running_.load(std::memory_order_acquire)) return false;
        if (!provider) return false;
        provider_ = std::move(provider);

        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) return false;

        int reuse = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(requestedPort_);
        if (bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            closeListen();
            return false;
        }
        if (listen(listenFd_, RTSPServer::kListenBacklog) < 0) {
            closeListen();
            return false;
        }

        struct sockaddr_in bound{};
        socklen_t boundLen = sizeof(bound);
        if (getsockname(listenFd_, reinterpret_cast<struct sockaddr*>(&bound), &boundLen) == 0) {
            boundPort_.store(ntohs(bound.sin_port), std::memory_order_release);
        }

        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&Impl::run, this);
        return true;
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (thread_.joinable()) thread_.join();
        closeListen();
    }

    bool isRunning() const { return running_.load(std::memory_order_acquire); }
    uint16_t boundPort() const { return boundPort_.load(std::memory_order_acquire); }
    int requestCount() const { return requestCount_.load(std::memory_order_relaxed); }

private:
    void closeListen() {
        if (listenFd_ >= 0) {
            close(listenFd_);
            listenFd_ = -1;
        }
    }

    void run() {
        while (running_.load(std::memory_order_acquire)) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(listenFd_, &readfds);
            struct timeval tv{0, kSelectTimeoutMs * 1000};
            const int ready = select(listenFd_ + 1, &readfds, nullptr, nullptr, &tv);
            if (ready < 0) break;
            if (ready == 0) continue; // timeout — re-check running_

            const int clientFd = accept(listenFd_, nullptr, nullptr);
            if (clientFd < 0) continue;
            serve(clientFd);
            close(clientFd);
        }
    }

    /// One request, one response, one connection. Keeping it that simple
    /// is what makes this endpoint safe to expose: no session table for a
    /// peer to grow, nothing to leak between connections.
    void serve(int clientFd) {
        struct timeval tv{kClientTimeoutMs / 1000, (kClientTimeoutMs % 1000) * 1000};
        setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::string request;
        char buffer[1024];
        while (request.size() < RTSPServer::kMaxRequestBytes) {
            const ssize_t got = recv(clientFd, buffer, sizeof(buffer), 0);
            if (got <= 0) break;
            request.append(buffer, static_cast<size_t>(got));
            if (request.find("\r\n\r\n") != std::string::npos) break;
            if (request.find("\n\n") != std::string::npos) break; // lenient client
        }
        if (request.empty()) return;

        requestCount_.fetch_add(1, std::memory_order_relaxed);

        const int cseq = parseCSeq(request);
        std::string method, path;
        if (!parseRequestLine(request, method, path)) {
            send(clientFd, statusLine(400, "Bad Request", cseq) + kCRLF);
            return;
        }

        if (method == "OPTIONS") {
            // Every client sends this first; naming only what we answer
            // keeps the advertisement honest.
            send(clientFd,
                 statusLine(200, "OK", cseq) + "Public: OPTIONS, DESCRIBE" + kCRLF + kCRLF);
            return;
        }

        if (method != "DESCRIBE") {
            send(clientFd, statusLine(501, "Not Implemented", cseq) + kCRLF);
            return;
        }

        std::vector<RTSPPublishedStream> streams;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (provider_) streams = provider_();
        }

        for (const auto& stream : streams) {
            if (stream.path != path) continue;
            std::ostringstream out;
            out << statusLine(200, "OK", cseq)
                << "Content-Type: application/sdp" << kCRLF
                << "Content-Base: " << path << kCRLF
                << "Content-Length: " << stream.sdp.size() << kCRLF
                << kCRLF
                << stream.sdp;
            send(clientFd, out.str());
            return;
        }

        send(clientFd, statusLine(404, "Not Found", cseq) + kCRLF);
    }

    void send(int fd, const std::string& text) const {
        size_t sent = 0;
        while (sent < text.size()) {
            const ssize_t wrote = ::send(fd, text.data() + sent, text.size() - sent, 0);
            if (wrote <= 0) return; // peer went away mid-response
            sent += static_cast<size_t>(wrote);
        }
    }

    const uint16_t requestedPort_;
    int listenFd_{-1};
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint16_t> boundPort_{0};
    std::atomic<int> requestCount_{0};

    mutable std::mutex mutex_;
    RTSPStreamProvider provider_;
};

RTSPServer::RTSPServer(uint16_t port) : pimpl_(std::make_unique<Impl>(port)) {}
RTSPServer::~RTSPServer() = default;

bool RTSPServer::start(RTSPStreamProvider provider) { return pimpl_->start(std::move(provider)); }
void RTSPServer::stop() { pimpl_->stop(); }
bool RTSPServer::isRunning() const { return pimpl_->isRunning(); }
uint16_t RTSPServer::boundPort() const { return pimpl_->boundPort(); }
int RTSPServer::requestCount() const { return pimpl_->requestCount(); }

} // namespace AES67
