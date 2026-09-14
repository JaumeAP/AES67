//
// ConnectionAPIServer.cpp
// AES67 macOS Driver
//

#include "NetworkEngine/Discovery/ConnectionAPIServer.h"
#include "NetworkEngine/Discovery/PathPieces.h"

#include "NetworkEngine/JsonEscape.h"
#include "Ravenna/ConnectionApi.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
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
    // A receiver's leg names the group it joins `multicast_ip`; a sender's
    // names the group it transmits to `destination_ip` (IS-05 sender and
    // receiver transport parameter schemas). One patch parser serves both, so
    // it takes whichever the controller used -- reading only the receiver's
    // name left a sender patch with no destination at all.
    patch.multicastAddress = stringField(params, "multicast_ip");
    if (!patch.multicastAddress.has_value() || patch.multicastAddress->empty()) {
        patch.multicastAddress = stringField(params, "destination_ip");
    }
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
               ConnectionReceiverPatcher patcher, ConnectionSenderPatcher senderPatcher) {
        if (running_.load()) return false;
        senders_ = std::move(senders);
        receivers_ = std::move(receivers);
        patcher_ = std::move(patcher);
        senderPatcher_ = std::move(senderPatcher);
        wireApi();

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
    bool hasSenderPatcher() const { return static_cast<bool>(senderPatcher_); }
    bool patchSender(const std::string& id, const ConnectionPatch& patch) const {
        return senderPatcher_ ? senderPatcher_(id, patch) : false;
    }

    // Not callable from inside route()'s own call tree (that includes the
    // onReceiverActivation/onSenderActivation callbacks wireApi() installs
    // below): route() holds apiMutex_ for the whole of api_.handle(), and
    // this is a plain mutex, not the recursive one ConnectionApi's own
    // resourcesMutex_ had to become for exactly this shape of problem. Safe
    // as designed because these are called from AES67Device's own NMOS sync
    // path, never nested inside a request this server is already serving.
    ConnectionActiveState senderActiveState(const std::string& id) const {
        std::lock_guard<std::mutex> held(apiMutex_);
        syncResources();
        ConnectionActiveState state;
        if (const auto sender = api_.sender(id)) {
            state.exists = true;
            state.masterEnable = sender->active.masterEnable;
            state.peerId = sender->active.receiverId;
        }
        return state;
    }

    ConnectionActiveState receiverActiveState(const std::string& id) const {
        std::lock_guard<std::mutex> held(apiMutex_);
        syncResources();
        ConnectionActiveState state;
        if (const auto receiver = api_.receiver(id)) {
            state.exists = true;
            state.masterEnable = receiver->active.masterEnable;
            state.peerId = receiver->active.senderId;
        }
        return state;
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

    // Set directly by ConnectionAPIServer::setFallbackRouter(); the serving
    // thread only ever reads it from route(), so no lock guards it.
    ConnectionAPIServer::FallbackRouter fallback_;

private:
    /// Wires the two activation callbacks, once. What IS-05 activates is what
    /// this driver is told to do: the API decides whether a request is legal
    /// and what the resulting state is, and the driver's own patchers are
    /// what make it happen.
    void wireApi();

    /// Brings the API's set of resources level with what the driver lists.
    /// The listers are the driver's live view -- streams are discovered and
    /// go away while this is serving -- and the API holds what a controller
    /// staged, so one is copied into the other rather than merged.
    void syncResources() const;

    ConnectionSenderLister senders_;
    ConnectionReceiverLister receivers_;
    ConnectionReceiverPatcher patcher_;
    ConnectionSenderPatcher senderPatcher_;

    uint16_t requestedPort_{0};
    uint16_t boundPort_{0};
    int listen_{-1};
    std::atomic<bool> running_{false};
    std::thread thread_;

    /// The IS-05 the RAVENNA package already serves. This driver used to
    /// answer the specification itself, and the two implementations drifted:
    /// the AMWA suite passed 57 of 61 against that one and 15 against this,
    /// over nine causes, every one of them already fixed there. Delegating is
    /// what keeps them from drifting again.
    mutable Ravenna::ConnectionApi api_;
    /// route() is const and the accept loop is one thread, but the routing is
    /// public so that it can be read without a socket, and the API underneath
    /// it is mutable state.
    mutable std::mutex apiMutex_;
    /// What each resource looked like the last time it was copied into the
    /// API, so a resource the driver changed underneath is re-seeded and one
    /// it did not is left with whatever a controller staged on it.
    mutable std::map<std::string, std::string> seeded_;
};

namespace {

/// Copies a staged leg's addresses and port onto the patch the driver reads.
/// The transport file carries most of this, but not all of it: a controller
/// may fix a group, a port or an interface that the file never named, and
/// those are exactly the values the driver would otherwise never see.
/// "auto" is not one of them -- it is a request for the device to choose,
/// not an address.
void legOntoPatch(const Ravenna::JsonObject& leg, bool forSender, ConnectionPatch& patch) {
    const auto text = [&leg](const char* name) -> std::optional<std::string> {
        const auto found = leg.find(name);
        if (found == leg.end() || !found->second.isString()) return std::nullopt;
        if (found->second.asString() == "auto" || found->second.asString().empty()) {
            return std::nullopt;
        }
        return found->second.asString();
    };

    patch.multicastAddress = text(forSender ? "destination_ip" : "multicast_ip");
    if (!forSender) patch.interfaceAddress = text("interface_ip");

    const auto port = leg.find("destination_port");
    if (port != leg.end() && port->second.isNumber()) {
        const double value = port->second.asNumber();
        if (value > 0 && value <= 65535) patch.port = static_cast<uint16_t>(value);
    }
}

}  // namespace

void ConnectionAPIServer::Impl::wireApi() {
    // A receiver joins a group by being handed a transport file. The API has
    // already worked out which one -- the controller's overrides followed into
    // the SDP -- so what reaches the driver is the stream it is to take.
    api_.onReceiverActivation([this](const std::string& id, const std::string& sdp,
                                     bool masterEnable, std::string& error) {
        ConnectionPatch patch;
        patch.masterEnable = masterEnable;
        patch.activateImmediate = true;
        if (!sdp.empty()) patch.transportFile = sdp;
        if (const auto receiver = api_.receiver(id)) {
            if (!receiver->staged.senderId.empty()) patch.senderId = receiver->staged.senderId;
            legOntoPatch(receiver->staged.transportParams, /*forSender=*/false, patch);
        }
        if (!this->patch(id, patch)) {
            error = "the driver would not take it";
            return false;
        }
        return true;
    });

    // A sender is re-addressed the same way: its transport file describes
    // where it transmits, and the API has already moved it.
    api_.onSenderActivation([this](const std::string& id, const std::string& sdp,
                                   bool masterEnable, std::string& error) {
        if (!hasSenderPatcher()) {
            error = "this driver's senders cannot be re-addressed";
            return false;
        }
        ConnectionPatch patch;
        patch.masterEnable = masterEnable;
        patch.activateImmediate = true;
        if (!sdp.empty()) patch.transportFile = sdp;
        if (const auto sender = api_.sender(id)) {
            legOntoPatch(sender->staged.transportParams, /*forSender=*/true, patch);
        }
        if (!patchSender(id, patch)) {
            error = "the driver would not take it";
            return false;
        }
        return true;
    });
}

void ConnectionAPIServer::Impl::syncResources() const {
    const std::vector<ConnectionSender> senderList = senders();
    const std::vector<ConnectionReceiver> receiverList = receivers();

    std::map<std::string, std::string> present;

    for (const ConnectionSender& sender : senderList) {
        std::ostringstream shape;
        shape << sender.label << '\x1f' << sender.multicastAddress << '\x1f' << sender.port
              << '\x1f' << sender.sourceAddress << '\x1f' << sender.enabled << '\x1f' << sender.sdp;
        present[sender.id] = shape.str();
        const auto known = seeded_.find(sender.id);
        if (known != seeded_.end() && known->second == shape.str()) continue;

        Ravenna::ConnectionSender fresh;
        fresh.id = sender.id;
        fresh.label = sender.label;
        fresh.sdp = sender.sdp;
        // What this sender is doing right now, which is what /active reports.
        // The transport file carries the addresses; master_enable is the one
        // thing the file cannot say.
        fresh.active.masterEnable = sender.enabled;
        fresh.active.transportFile = sender.sdp;
        fresh.staged = fresh.active;
        api_.addSender(fresh);
    }

    for (const ConnectionReceiver& receiver : receiverList) {
        std::ostringstream shape;
        shape << receiver.label << '\x1f' << receiver.multicastAddress << '\x1f' << receiver.port
              << '\x1f' << receiver.senderId << '\x1f' << receiver.enabled << '\x1f'
              << receiver.sdp;
        present[receiver.id] = shape.str();
        const auto known = seeded_.find(receiver.id);
        if (known != seeded_.end() && known->second == shape.str()) continue;

        Ravenna::ConnectionReceiver fresh;
        fresh.id = receiver.id;
        fresh.label = receiver.label;
        // The group and the port as the driver has them. The transport file
        // says both, but a receiver whose stream was never described by one
        // -- discovered over SAP, or pointed there by hand -- still has to
        // report where it is listening.
        fresh.active.masterEnable = receiver.enabled;
        fresh.active.senderId = receiver.senderId;
        fresh.active.transportFile = receiver.sdp;
        if (!receiver.multicastAddress.empty()) {
            fresh.active.transportParams["multicast_ip"] =
                Ravenna::JsonValue(receiver.multicastAddress);
        }
        if (receiver.port != 0) {
            fresh.active.transportParams["destination_port"] =
                Ravenna::JsonValue(static_cast<int>(receiver.port));
        }
        fresh.staged = fresh.active;
        api_.addReceiver(fresh);
    }

    // A resource the driver no longer lists is one a controller must stop
    // being able to patch.
    for (const auto& [id, shape] : seeded_) {
        if (present.count(id) != 0) continue;
        api_.removeSender(id);
        api_.removeReceiver(id);
    }
    seeded_ = std::move(present);
}


ConnectionAPIServer::Reply ConnectionAPIServer::Impl::route(const std::string& method,
                                                            const std::string& path,
                                                            const std::string& body) const {
    const std::vector<std::string> pieces = pathPieces(path);

    if (pieces.empty() || pieces[0] != "x-nmos") return {404, "application/json", "[]"};
    if (pieces.size() == 1) {
        // The APIs this port serves. The Node API is only listed when
        // something answers for it.
        return {200, "application/json",
                fallback_ ? jsonList({"connection/", "node/"}) : jsonList({"connection/"})};
    }
    if (pieces[1] != "connection") {
        if (fallback_) return fallback_(method, path, body);
        return {404, "application/json", "[]"};
    }
    if (pieces.size() == 2) {
        // The versions of the Connection API this serves. A controller walks
        // down from here rather than being told a version to assume, and
        // answering 404 at this level made the whole API undiscoverable.
        return {200, "application/json",
                jsonList({std::string(ConnectionAPIServer::kApiVersion) + "/"})};
    }
    if (pieces[2] != ConnectionAPIServer::kApiVersion) {
        // A version this does not serve is a 404 rather than a guess: a
        // controller that asked for v1.0 semantics must not be answered
        // in v1.1's.
        return {404, "application/json", "[]"};
    }

    // Senders stay read-only when the driver gave no way to re-address one.
    // The API below would take the patch and hand it to a callback that
    // refuses, which is a 400 -- "this request was wrong" -- when the truth
    // is that the resource is right and the feature is absent.
    if (method == "PATCH" && pieces.size() == 7 && pieces[3] == "single" &&
        pieces[4] == "senders" && pieces[6] == "staged" && !hasSenderPatcher()) {
        return {501, "application/json", "[]"};
    }

    std::lock_guard<std::mutex> held(apiMutex_);
    syncResources();
    const Ravenna::ApiResponse answer = api_.handle(method, path, body);
    return {answer.status, answer.contentType, answer.body};
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
        if (query != std::string::npos) path.resize(query);  // not substr: assigned to itself
        if (method == "OPTIONS") {
            // The preflight a browser-based controller sends before a PATCH.
            reply = {200, "text/plain", {}};
        } else {
            reply = route(method, path, body);
        }
    }

    std::ostringstream out;
    out << "HTTP/1.1 " << reply.status << " "
        << (reply.status == 200 ? "OK" : (reply.status == 404 ? "Not Found" : "Error")) << "\r\n"
        << "Content-Type: " << reply.contentType << "\r\n"
        << "Content-Length: " << reply.body.size() << "\r\n";
    // IS-05 controllers are often browser based, and a device that answers
    // without these is a device they cannot drive: a preflight that comes
    // back without Allow-Headers fails the request before it is sent.
    //
    // The 2026-09-04 audit sent these on GET only, so that a browser could
    // read the API but not patch it -- there is no authentication here, and
    // withholding the preflight is what stopped an arbitrary page from
    // re-pointing the device's audio. That protection is given up on purpose:
    // this server and the RAVENNA package's now answer identically, and the
    // RAVENNA one has always sent them. The exposure is unchanged for
    // anything that is not a browser, which was never blocked by a header.
    out << "Access-Control-Allow-Origin: *\r\n"
        << "Access-Control-Allow-Methods: GET, PUT, POST, PATCH, DELETE, HEAD, OPTIONS\r\n"
        << "Access-Control-Allow-Headers: Content-Type, Accept, Authorization\r\n"
        << "Access-Control-Max-Age: 3600\r\n";
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
                                ConnectionReceiverPatcher patcher,
                                ConnectionSenderPatcher senderPatcher) {
    return impl_->start(std::move(senders), std::move(receivers), std::move(patcher),
                        std::move(senderPatcher));
}

void ConnectionAPIServer::stop() { impl_->stop(); }
bool ConnectionAPIServer::isRunning() const { return impl_->isRunning(); }
uint16_t ConnectionAPIServer::boundPort() const { return impl_->boundPort(); }

void ConnectionAPIServer::setFallbackRouter(FallbackRouter router) {
    impl_->fallback_ = std::move(router);
}

std::string ConnectionAPIServer::controlHref(const std::string& host) const {
    return "http://" + host + ":" + std::to_string(impl_->boundPort()) + "/x-nmos/connection/" +
           kApiVersion + "/";
}

ConnectionAPIServer::Reply ConnectionAPIServer::route(const std::string& method,
                                                      const std::string& path,
                                                      const std::string& body) const {
    return impl_->route(method, path, body);
}

ConnectionActiveState ConnectionAPIServer::senderActiveState(const std::string& senderId) const {
    return impl_->senderActiveState(senderId);
}

ConnectionActiveState ConnectionAPIServer::receiverActiveState(const std::string& receiverId) const {
    return impl_->receiverActiveState(receiverId);
}

} // namespace AES67
