//
// NMOSRegistrationClient.cpp
// AES67 macOS Driver
//

#include "NetworkEngine/Discovery/NMOSRegistrationClient.h"

#include "NetworkEngine/Discovery/HTTPClient.h"
#include "NetworkEngine/Discovery/MDNSBrowser.h"

#include <chrono>
#include <sstream>
#include <thread>

namespace AES67 {

namespace {

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

} // namespace

NMOSRegistrationClient::NMOSRegistrationClient(NMOSNodeInfo node)
    : node_(std::move(node)) {}

NMOSRegistrationClient::~NMOSRegistrationClient() {
    stop();
}

std::string NMOSRegistrationClient::registrationPath(const std::string& apiVersion) {
    return "/x-nmos/registration/" + apiVersion + "/resource";
}

std::string NMOSRegistrationClient::healthPath(const std::string& apiVersion,
                                               const std::string& nodeId) {
    return "/x-nmos/registration/" + apiVersion + "/health/nodes/" + nodeId;
}

std::string NMOSRegistrationClient::buildRegistrationBody(const NMOSNodeInfo& node,
                                                          int64_t versionSeconds,
                                                          int32_t versionNanos) {
    // IS-04's version is "<seconds>:<nanoseconds>" and orders updates to a
    // resource. It is TAI in the specification; the registry compares it
    // against what it already holds rather than against its own clock, so
    // what matters here is that it never goes backwards.
    std::ostringstream json;
    json << "{\n"
         << "  \"type\": \"node\",\n"
         << "  \"data\": {\n"
         << "    \"id\": \"" << jsonEscape(node.id) << "\",\n"
         << "    \"version\": \"" << versionSeconds << ":" << versionNanos << "\",\n"
         << "    \"label\": \"" << jsonEscape(node.label) << "\",\n"
         << "    \"description\": \"" << jsonEscape(node.description) << "\",\n"
         << "    \"tags\": {},\n"
         << "    \"href\": \"" << jsonEscape(node.href) << "\",\n"
         << "    \"hostname\": \"" << jsonEscape(node.hostname) << "\",\n"
         << "    \"caps\": {},\n"
         << "    \"api\": {\n"
         << "      \"versions\": [\"v1.3\"],\n"
         << "      \"endpoints\": []\n"
         << "    },\n"
         << "    \"services\": [],\n"
         // The clock this node offers. "internal" is the honest answer
         // while the PTP subsystem is opt-in and off by default: a node
         // that claims a PTP clock it is not running is a node a
         // controller will try to slave things to.
         << "    \"clocks\": [{ \"name\": \"clk0\", \"ref_type\": \"internal\" }],\n"
         << "    \"interfaces\": []\n"
         << "  }\n"
         << "}\n";
    return json.str();
}

std::optional<NMOSRegistry> NMOSRegistrationClient::discoverRegistry(
    std::chrono::milliseconds waitFor) {
    MDNSBrowser browser(MDNSBrowser::kServiceTypeNMOSRegister);
    if (!browser.start()) {
        // No system responder: a lost convenience, never a failure. Same
        // stance as the rest of discovery here.
        return std::nullopt;
    }

    const auto deadline = std::chrono::steady_clock::now() + waitFor;
    std::optional<NMOSRegistry> found;
    while (std::chrono::steady_clock::now() < deadline && !found.has_value()) {
        for (const MDNSService& service : browser.discoveredServices()) {
            if (!service.isResolved()) continue;
            NMOSRegistry registry;
            registry.host = service.address;
            registry.port = service.port;
            found = registry;
            break;
        }
        if (!found.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    browser.stop();
    return found;
}

bool NMOSRegistrationClient::postNode() {
    NMOSRegistry registry;
    NMOSNodeInfo node;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registry = registry_;
        node = node_;
    }
    if (!registry.valid()) return false;

    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds);

    HTTPClient client(registry.host, registry.port);
    const HTTPResponse response = client.post(
        registrationPath(registry.apiVersion),
        buildRegistrationBody(node, seconds.count(), static_cast<int32_t>(nanos.count())),
        "application/json");

    // 201 is a new registration, 200 is the registry recognising an id it
    // already holds. Both mean it took.
    const bool accepted = response.error.empty() &&
                          (response.status == 200 || response.status == 201);
    registered_.store(accepted, std::memory_order_relaxed);
    return accepted;
}

bool NMOSRegistrationClient::registerWith(const NMOSRegistry& registry) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registry_ = registry;
        if (registry_.apiVersion.empty()) registry_.apiVersion = "v1.3";
    }
    return postNode();
}

bool NMOSRegistrationClient::heartbeat() {
    NMOSRegistry registry;
    std::string nodeId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registry = registry_;
        nodeId = node_.id;
    }
    if (!registry.valid()) return false;

    HTTPClient client(registry.host, registry.port);
    const HTTPResponse response = client.post(healthPath(registry.apiVersion, nodeId), "", "");

    if (response.error.empty() && response.status == 200) {
        registered_.store(true, std::memory_order_relaxed);
        return true;
    }

    // 404 is the registry saying it does not know this node any more,
    // which is what it says after garbage-collecting a node that went
    // quiet. Registering again is the documented way back in.
    if (response.status == 404) {
        registered_.store(false, std::memory_order_relaxed);
        return postNode();
    }

    registered_.store(false, std::memory_order_relaxed);
    return false;
}

void NMOSRegistrationClient::startHeartbeats() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;

    heartbeatThread_ = std::thread([this] {
        while (running_.load(std::memory_order_acquire)) {
            heartbeat();
            // Slept in slices so stop() does not wait a whole period.
            for (int i = 0; i < 50 && running_.load(std::memory_order_acquire); i++) {
                std::this_thread::sleep_for(kHeartbeatPeriod / 50);
            }
        }
    });
}

void NMOSRegistrationClient::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (heartbeatThread_.joinable()) heartbeatThread_.join();
}

bool NMOSRegistrationClient::unregister() {
    NMOSRegistry registry;
    std::string nodeId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registry = registry_;
        nodeId = node_.id;
    }
    if (!registry.valid()) return false;

    HTTPClient client(registry.host, registry.port);
    const HTTPResponse response =
        client.del(registrationPath(registry.apiVersion) + "/nodes/" + nodeId);

    registered_.store(false, std::memory_order_relaxed);
    // 204 is the documented answer; 404 means it had already forgotten
    // us, which is the same outcome from here.
    return response.error.empty() && (response.status == 204 || response.status == 404);
}

} // namespace AES67
