//
// ConnectionAPIServer.cpp
// AES67 macOS Driver
//

#include "NetworkEngine/Discovery/ConnectionAPIServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>

namespace AES67 {

namespace {

constexpr size_t kMaxRequestBytes = size_t{64} * 1024;  // an SDP in a patch, and no more
constexpr int kListenBacklog = 8;
constexpr int kSelectTimeoutMs = 250;   ///< how fast the accept loop notices stop()
/// A client that connects and then says nothing is dropped. One thread
/// serves every connection in turn, so without this a single open socket
/// holds the whole API shut for as long as the peer cares to keep it
/// (2026-09-04 audit); RTSPServer has had the same bound since it was written.
constexpr int kClientTimeoutMs = 2000;

/// Offset of the value that follows a header name, matched without regard to
/// case, or npos. `name` is lowercase; the head is searched line by line so a
/// name appearing inside another header's value cannot be mistaken for one.
size_t findHeader(const std::string& head, const std::string& name) {
    size_t lineStart = 0;
    while (lineStart < head.size()) {
        size_t lineEnd = head.find("\r\n", lineStart);
        if (lineEnd == std::string::npos) lineEnd = head.size();
        const size_t colon = head.find(':', lineStart);
        if (colon != std::string::npos && colon < lineEnd &&
            colon - lineStart == name.size()) {
            bool same = true;
            for (size_t i = 0; i < name.size() && same; ++i) {
                same = std::tolower(static_cast<unsigned char>(head[lineStart + i])) ==
                       static_cast<unsigned char>(name[i]);
            }
            if (same) return colon + 1;
        }
        if (lineEnd == head.size()) break;
        lineStart = lineEnd + 2;
    }
    return std::string::npos;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (c == '\n') { out += "\\n"; continue; }
        if (c == '\r') { out += "\\r"; continue; }
        out.push_back(c);
    }
    return out;
}

/// The path split on '/', empty pieces dropped. Percent-decoding is not
/// needed: every id here is a UUID and every other piece is a fixed word.
std::vector<std::string> pathPieces(const std::string& path) {
    std::vector<std::string> pieces;
    size_t start = 0;
    while (start < path.size()) {
        const size_t slash = path.find('/', start);
        const size_t end = (slash == std::string::npos) ? path.size() : slash;
        if (end > start) pieces.push_back(path.substr(start, end - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return pieces;
}

std::string jsonList(const std::vector<std::string>& entries) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < entries.size(); i++) {
        if (i > 0) out << ", ";
        out << "\"" << jsonEscape(entries[i]) << "\"";
    }
    out << "]";
    return out.str();
}

/// The offset just past the colon of a "key": at this object's own level, or
/// npos. Scanning rather than searching for the quoted name: a plain find()
/// matches the same text inside a nested object or, worse, inside a string
/// value — and one of the values this server accepts is `data`, an entire SDP
/// supplied by the caller, which could carry `"master_enable": true` in its
/// own text and have it read as the request's (2026-09-04 audit). Nesting is
/// handled by slicing (below) and looking again, not by matching at depth.
size_t fieldAt(const std::string& json, const std::string& key) {
    int depth = 0;
    bool inString = false;
    size_t stringStart = 0;
    for (size_t i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (inString) {
            if (c == '\\') { ++i; continue; }
            if (c != '"') continue;
            inString = false;
            if (depth != 1) continue;   // a name of some nested object
            size_t after = i + 1;
            while (after < json.size() &&
                   std::isspace(static_cast<unsigned char>(json[after]))) ++after;
            if (after >= json.size() || json[after] != ':') continue;
            if (i - stringStart != key.size()) continue;
            if (json.compare(stringStart, key.size(), key) != 0) continue;
            return after + 1;
        }
        if (c == '"') { inString = true; stringStart = i + 1; continue; }
        if (c == '{' || c == '[') { ++depth; continue; }
        if (c == '}' || c == ']') { --depth; continue; }
    }
    return std::string::npos;
}

/// The first `{...}` object inside `text`, braces included, or an empty
/// string. What IS-05 nests one level down — the transport parameters live in
/// an array of objects, transport_file and activation are objects — is read by
/// slicing it out and searching that, so a name is only ever matched in the
/// object it belongs to.
std::string firstObject(const std::string& text) {
    int depth = 0;
    bool inString = false;
    size_t start = std::string::npos;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '{') {
            if (depth == 0) start = i;
            ++depth;
            continue;
        }
        if (c == '}') {
            --depth;
            if (depth == 0 && start != std::string::npos) return text.substr(start, i - start + 1);
        }
    }
    return {};
}

/// The value of a key of this object, as raw text, or an empty string. Used to
/// descend one level: `member(body, "activation")` then `stringField(that,
/// "mode")`.
std::string member(const std::string& json, const std::string& key) {
    const size_t pos = fieldAt(json, key);
    if (pos == std::string::npos) return {};
    return json.substr(pos);
}

/// A string field out of the top level of a JSON object, or nothing. Written
/// by hand for the same reason the rest of this driver's JSON is: a parser
/// dependency inside coreaudiod is a liability, and what a controller sends
/// is small and known.
std::optional<std::string> stringField(const std::string& json, const std::string& key) {
    size_t pos = fieldAt(json, key);
    if (pos == std::string::npos) return std::nullopt;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size()) return std::nullopt;
    if (json.compare(pos, 4, "null") == 0) return std::nullopt;
    if (json[pos] != '"') return std::nullopt;
    ++pos;
    std::string value;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;
            if (json[pos] == 'n') { value.push_back('\n'); ++pos; continue; }
            if (json[pos] == 'r') { value.push_back('\r'); ++pos; continue; }
        }
        value.push_back(json[pos]);
        ++pos;
    }
    return value;
}

std::optional<bool> boolField(const std::string& json, const std::string& key) {
    size_t pos = fieldAt(json, key);
    if (pos == std::string::npos) return std::nullopt;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (json.compare(pos, 4, "true") == 0) return true;
    if (json.compare(pos, 5, "false") == 0) return false;
    return std::nullopt;
}

std::optional<long> numberField(const std::string& json, const std::string& key) {
    size_t pos = fieldAt(json, key);
    if (pos == std::string::npos) return std::nullopt;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    const size_t start = pos;
    if (pos < json.size() && (json[pos] == '-' || json[pos] == '+')) ++pos;
    const size_t digits = pos;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos == digits) return std::nullopt;
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(json.c_str() + start, &end, 10);
    if (errno == ERANGE) return std::nullopt;
    return value;
}

} // namespace

ConnectionPatch ConnectionAPIServer::parsePatch(const std::string& json) {
    ConnectionPatch patch;

    patch.masterEnable = boolField(json, "master_enable");
    patch.senderId = stringField(json, "sender_id");

    // transport_params is an array of objects, one per leg; this driver has
    // one, and IS-05 says a controller may send fewer than the receiver
    // declares, never more, so the first is the one that applies.
    const std::string params = firstObject(member(json, "transport_params"));
    patch.multicastAddress = stringField(params, "multicast_ip");
    patch.interfaceAddress = stringField(params, "interface_ip");
    if (auto port = numberField(params, "destination_port")) {
        if (*port > 0 && *port <= 65535) patch.port = static_cast<uint16_t>(*port);
    }

    // transport_file carries {"data": "<the SDP>", "type": "application/sdp"}.
    const std::string transportFile = firstObject(member(json, "transport_file"));
    if (auto data = stringField(transportFile, "data")) {
        if (!data->empty()) patch.transportFile = *data;
    }

    const std::string activation = firstObject(member(json, "activation"));
    if (auto mode = stringField(activation, "mode")) {
        patch.activateImmediate = (*mode == "activate_immediate");
    }
    return patch;
}

class ConnectionAPIServer::Impl {
public:
    explicit Impl(uint16_t port) : requestedPort_(port) {}

    ~Impl() { stop(); }

    bool start(ConnectionSenderLister senders, ConnectionReceiverLister receivers,
               ConnectionReceiverPatcher patcher) {
        if (running_.load()) return false;
        senders_ = std::move(senders);
        receivers_ = std::move(receivers);
        patcher_ = std::move(patcher);

        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_ < 0) return false;
        int yes = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(requestedPort_);
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(listen_);
            listen_ = -1;
            return false;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            boundPort_ = ntohs(addr.sin_port);
        }
        if (::listen(listen_, kListenBacklog) != 0) {
            ::close(listen_);
            listen_ = -1;
            return false;
        }

        running_.store(true);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) {
            if (listen_ >= 0) {
                ::close(listen_);
                listen_ = -1;
            }
            return;
        }
        // Join FIRST, then close. Closing the listening socket while the
        // accept loop is blocked on it is a race the descriptor loses: the
        // number is free the moment close() returns and any thread that
        // opens a file next inherits it, so the loop can end up accepting on
        // an unrelated descriptor. The loop polls running_ every
        // kSelectTimeoutMs, so this waits a quarter of a second at worst
        // (2026-09-04 audit).
        if (thread_.joinable()) thread_.join();
        if (listen_ >= 0) {
            ::close(listen_);
            listen_ = -1;
        }
    }

    bool isRunning() const { return running_.load(); }
    uint16_t boundPort() const { return boundPort_; }

    std::vector<ConnectionSender> senders() const { return senders_ ? senders_() : std::vector<ConnectionSender>{}; }
    std::vector<ConnectionReceiver> receivers() const {
        return receivers_ ? receivers_() : std::vector<ConnectionReceiver>{};
    }
    bool patch(const std::string& id, const ConnectionPatch& patch) const {
        return patcher_ ? patcher_(id, patch) : false;
    }

private:
    void run() {
        while (running_.load()) {
            // select() with a timeout rather than a blocking accept(): it is
            // what lets stop() join this thread before closing the listening
            // socket, and it keeps a broken descriptor from spinning the
            // loop at full speed.
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(listen_, &readfds);
            struct timeval tv{0, kSelectTimeoutMs * 1000};
            const int ready = ::select(listen_ + 1, &readfds, nullptr, nullptr, &tv);
            if (ready < 0) {
                if (errno == EINTR) continue; // a signal, not a failure
                running_.store(false);
                return;
            }
            if (ready == 0) continue; // timeout — re-check running_

            const int client = ::accept(listen_, nullptr, nullptr);
            if (client < 0) continue;
            serve(client);
            ::close(client);
        }
    }

    void serve(int client);

public:
    ConnectionAPIServer::Reply route(const std::string& method, const std::string& path,
                                     const std::string& body) const;

private:
    /// The five leaves every sender and receiver carries.
    static std::vector<std::string> leaves(bool isSender) {
        std::vector<std::string> entries{"constraints/", "staged/", "active/", "transporttype/"};
        if (isSender) entries.push_back("transportfile/");
        return entries;
    }

    static std::string transportParams(const std::string& multicastAddress, uint16_t port,
                                       const std::string& interfaceAddress);
    static std::string connectionResource(const std::string& senderId, bool enabled,
                                          const std::string& multicastAddress, uint16_t port,
                                          const std::string& interfaceAddress);

    ConnectionSenderLister senders_;
    ConnectionReceiverLister receivers_;
    ConnectionReceiverPatcher patcher_;

    uint16_t requestedPort_{0};
    uint16_t boundPort_{0};
    int listen_{-1};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

std::string ConnectionAPIServer::Impl::transportParams(const std::string& multicastAddress,
                                                       uint16_t port,
                                                       const std::string& interfaceAddress) {
    std::ostringstream out;
    out << "[{ \"destination_port\": " << port << ", "
        << "\"multicast_ip\": " << (multicastAddress.empty()
                                        ? std::string("null")
                                        : "\"" + jsonEscape(multicastAddress) + "\"")
        << ", \"interface_ip\": " << (interfaceAddress.empty()
                                          ? std::string("\"auto\"")
                                          : "\"" + jsonEscape(interfaceAddress) + "\"")
        << ", \"rtp_enabled\": true }]";
    return out.str();
}

std::string ConnectionAPIServer::Impl::connectionResource(const std::string& senderId,
                                                          bool enabled,
                                                          const std::string& multicastAddress,
                                                          uint16_t port,
                                                          const std::string& interfaceAddress) {
    std::ostringstream out;
    out << "{\n"
        << "  \"master_enable\": " << (enabled ? "true" : "false") << ",\n"
        << "  \"sender_id\": "
        << (senderId.empty() ? std::string("null") : "\"" + jsonEscape(senderId) + "\"") << ",\n"
        // Nothing is ever pending here: this driver applies an activation
        // when it is asked for and stages nothing for later, so saying so
        // is more useful than a mode a controller would wait on.
        << "  \"activation\": { \"mode\": null, \"requested_time\": null, "
        << "\"activation_time\": null },\n"
        << "  \"transport_params\": " << transportParams(multicastAddress, port, interfaceAddress)
        << "\n}\n";
    return out.str();
}

ConnectionAPIServer::Reply ConnectionAPIServer::Impl::route(const std::string& method,
                                                            const std::string& path,
                                                            const std::string& body) const {
    const std::vector<std::string> pieces = pathPieces(path);

    // /x-nmos/connection/v1.1/...
    if (pieces.size() < 3 || pieces[0] != "x-nmos" || pieces[1] != "connection") {
        return {404, "application/json", "[]"};
    }
    if (pieces[2] != ConnectionAPIServer::kApiVersion) {
        // A version this does not serve is a 404 rather than a guess: a
        // controller that asked for v1.0 semantics must not be answered
        // in v1.1's.
        return {404, "application/json", "[]"};
    }
    if (pieces.size() == 3) return {200, "application/json", jsonList({"single/", "bulk/"})};
    if (pieces[3] == "bulk") {
        // Bulk staging is one PATCH for many resources. Not served, and
        // said so rather than half-answered.
        return {501, "application/json", "[]"};
    }
    if (pieces[3] != "single") return {404, "application/json", "[]"};
    if (pieces.size() == 4) {
        return {200, "application/json", jsonList({"senders/", "receivers/"})};
    }

    const bool isSender = (pieces[4] == "senders");
    const bool isReceiver = (pieces[4] == "receivers");
    if (!isSender && !isReceiver) return {404, "application/json", "[]"};

    const std::vector<ConnectionSender> senderList = senders();
    const std::vector<ConnectionReceiver> receiverList = receivers();

    if (pieces.size() == 5) {
        std::vector<std::string> ids;
        if (isSender) {
            for (const ConnectionSender& sender : senderList) ids.push_back(sender.id + "/");
        } else {
            for (const ConnectionReceiver& receiver : receiverList) ids.push_back(receiver.id + "/");
        }
        return {200, "application/json", jsonList(ids)};
    }

    const std::string& id = pieces[5];
    const ConnectionSender* sender = nullptr;
    const ConnectionReceiver* receiver = nullptr;
    if (isSender) {
        for (const ConnectionSender& candidate : senderList) {
            if (candidate.id == id) sender = &candidate;
        }
        if (sender == nullptr) return {404, "application/json", "[]"};
    } else {
        for (const ConnectionReceiver& candidate : receiverList) {
            if (candidate.id == id) receiver = &candidate;
        }
        if (receiver == nullptr) return {404, "application/json", "[]"};
    }

    if (pieces.size() == 6) return {200, "application/json", jsonList(leaves(isSender))};

    const std::string& leaf = pieces[6];

    if (leaf == "transporttype") {
        return {200, "application/json", "\"urn:x-nmos:transport:rtp.mcast\""};
    }

    if (leaf == "constraints") {
        // One leg, and nothing constrained beyond what the transport is:
        // an empty object per leg is IS-05's way of saying "anything this
        // transport allows".
        return {200, "application/json", "[{}]"};
    }

    if (leaf == "transportfile") {
        if (!isSender) return {404, "application/json", "[]"};
        if (sender->sdp.empty()) return {404, "application/json", "[]"};
        return {200, "application/sdp", sender->sdp};
    }

    if (leaf == "staged" || leaf == "active") {
        if (method == "GET") {
            if (isSender) {
                return {200, "application/json",
                        connectionResource("", sender->enabled, sender->multicastAddress,
                                           sender->port, sender->sourceAddress)};
            }
            return {200, "application/json",
                    connectionResource(receiver->senderId, receiver->enabled,
                                       receiver->multicastAddress, receiver->port, "")};
        }

        if (method == "PATCH") {
            if (leaf == "active") {
                // active is what IS-05 reports, never what it is told.
                return {405, "application/json", "[]"};
            }
            if (isSender) {
                // Senders are configured through this driver's own
                // settings; a control that accepted the patch and did
                // nothing would be worse than one that says no.
                return {501, "application/json", "[]"};
            }
            const ConnectionPatch parsed = ConnectionAPIServer::parsePatch(body);
            if (!patch(id, parsed)) {
                return {500, "application/json", "[]"};
            }
            // Answer with what the patch asked for: a controller reads
            // this back to confirm what it staged.
            const std::string multicast = parsed.multicastAddress.value_or(
                receiver ? receiver->multicastAddress : std::string());
            const uint16_t port = parsed.port.value_or(receiver ? receiver->port : 0);
            const std::string senderId = parsed.senderId.value_or(
                receiver ? receiver->senderId : std::string());
            const bool enabled = parsed.masterEnable.value_or(receiver ? receiver->enabled : true);
            return {200, "application/json",
                    connectionResource(senderId, enabled, multicast, port,
                                       parsed.interfaceAddress.value_or(""))};
        }

        return {405, "application/json", "[]"};
    }

    return {404, "application/json", "[]"};
}

void ConnectionAPIServer::Impl::serve(int client) {
    struct timeval tv{kClientTimeoutMs / 1000, (kClientTimeoutMs % 1000) * 1000};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string request;
    char chunk[4096];
    size_t headEnd = std::string::npos;

    // Head first, then as much body as Content-Length says.
    while (request.size() < kMaxRequestBytes) {
        const ssize_t n = ::recv(client, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        request.append(chunk, static_cast<size_t>(n));
        headEnd = request.find("\r\n\r\n");
        if (headEnd == std::string::npos) continue;

        const std::string head = request.substr(0, headEnd);
        long declared = 0;
        // Header names are case-insensitive (RFC 9110 §5.1) and curl sends
        // "Content-Length" while plenty of controllers send "content-length";
        // matching one spelling meant the body was never waited for.
        const size_t lengthAt = findHeader(head, "content-length");
        if (lengthAt != std::string::npos) {
            declared = std::strtol(head.c_str() + lengthAt, nullptr, 10);
            if (declared < 0 || static_cast<size_t>(declared) > kMaxRequestBytes) declared = 0;
        }
        if (request.size() >= headEnd + 4 + static_cast<size_t>(declared)) break;
    }

    if (headEnd == std::string::npos) return;

    const std::string head = request.substr(0, headEnd);
    const std::string body = request.substr(headEnd + 4);

    const size_t firstSpace = head.find(' ');
    const size_t secondSpace = (firstSpace == std::string::npos)
                                   ? std::string::npos
                                   : head.find(' ', firstSpace + 1);
    ConnectionAPIServer::Reply reply{400, "application/json", "[]"};
    std::string method;
    if (firstSpace != std::string::npos && secondSpace != std::string::npos) {
        method = head.substr(0, firstSpace);
        std::string path = head.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        const size_t query = path.find('?');
        if (query != std::string::npos) path = path.substr(0, query);
        reply = route(method, path, body);
    }

    std::ostringstream out;
    out << "HTTP/1.1 " << reply.status << " "
        << (reply.status == 200 ? "OK" : (reply.status == 404 ? "Not Found" : "Error")) << "\r\n"
        << "Content-Type: " << reply.contentType << "\r\n"
        << "Content-Length: " << reply.body.size() << "\r\n";
    // A controller reads this from a browser as often as from code, so GET
    // stays readable cross-origin. Activations do not: this endpoint has no
    // authentication, and letting an arbitrary page's script read or drive
    // one would hand every website the user visits a way to re-point the
    // device's audio (2026-09-04 audit).
    if (method == "GET") {
        out << "Access-Control-Allow-Origin: *\r\n";
    }
    out << "Connection: close\r\n\r\n" << reply.body;
    const std::string answer = out.str();
    // Loop: a transport file is comfortably larger than a socket buffer, and
    // a short write truncated the response rather than failing it.
    size_t sent = 0;
    while (sent < answer.size()) {
        const ssize_t wrote = ::send(client, answer.data() + sent, answer.size() - sent, 0);
        if (wrote <= 0) return; // peer went away mid-response
        sent += static_cast<size_t>(wrote);
    }
}

ConnectionAPIServer::ConnectionAPIServer(uint16_t port) : impl_(std::make_unique<Impl>(port)) {}
ConnectionAPIServer::~ConnectionAPIServer() = default;

bool ConnectionAPIServer::start(ConnectionSenderLister senders, ConnectionReceiverLister receivers,
                                ConnectionReceiverPatcher patcher) {
    return impl_->start(std::move(senders), std::move(receivers), std::move(patcher));
}

void ConnectionAPIServer::stop() { impl_->stop(); }
bool ConnectionAPIServer::isRunning() const { return impl_->isRunning(); }
uint16_t ConnectionAPIServer::boundPort() const { return impl_->boundPort(); }

std::string ConnectionAPIServer::controlHref(const std::string& host) const {
    return "http://" + host + ":" + std::to_string(impl_->boundPort()) + "/x-nmos/connection/" +
           kApiVersion + "/";
}

ConnectionAPIServer::Reply ConnectionAPIServer::route(const std::string& method,
                                                      const std::string& path,
                                                      const std::string& body) const {
    return impl_->route(method, path, body);
}

} // namespace AES67
