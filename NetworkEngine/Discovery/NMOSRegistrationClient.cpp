//
// NMOSRegistrationClient.cpp
// AES67 macOS Driver
//

#include "NetworkEngine/Discovery/NMOSRegistrationClient.h"

#include "NetworkEngine/Discovery/HTTPClient.h"
#include "NetworkEngine/Discovery/MDNSBrowser.h"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
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

namespace {

/// The sixteen bytes of a UUID written as "8-4-4-4-12".
std::string uuidText(const uint8_t bytes[16]) {
    char text[37];
    std::snprintf(text, sizeof(text),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                  bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
                  bytes[15]);
    return std::string(text);
}

/// The sixteen bytes behind a UUID's text, for use as a namespace. A
/// string that is not a UUID hashes as itself, which is worse than
/// nothing only if two callers disagree about what a namespace is.
bool uuidBytes(const std::string& text, uint8_t out[16]) {
    std::string hex;
    for (char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c))) hex.push_back(c);
    }
    if (hex.size() != 32) return false;
    for (int i = 0; i < 16; i++) {
        out[i] = static_cast<uint8_t>(std::stoul(hex.substr(static_cast<size_t>(i) * 2, 2),
                                                 nullptr, 16));
    }
    return true;
}

/// The version string IS-04 orders updates by.
std::string versionText(int64_t seconds, int32_t nanos) {
    return std::to_string(seconds) + ":" + std::to_string(nanos);
}

/// A JSON array of quoted strings.
std::string jsonStringArray(const std::vector<std::string>& values) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < values.size(); i++) {
        if (i > 0) out << ", ";
        out << "\"" << values[i] << "\"";
    }
    out << "]";
    return out.str();
}

} // namespace

std::string NMOSRegistrationClient::deriveId(const std::string& namespaceUuid,
                                             const std::string& name) {
    // RFC 4122 version 5: SHA-1 over the namespace's bytes followed by the
    // name, with the version and variant bits forced. Deterministic, so
    // the same stream keeps the same id across restarts without anything
    // being stored.
    uint8_t ns[16] = {0};
    std::string input;
    if (uuidBytes(namespaceUuid, ns)) {
        input.assign(reinterpret_cast<const char*>(ns), sizeof(ns));
    } else {
        input = namespaceUuid;
    }
    input += name;

    uint8_t digest[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1(input.data(), static_cast<CC_LONG>(input.size()), digest);

    uint8_t bytes[16];
    std::copy(digest, digest + 16, bytes);
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x50); // version 5
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80); // variant 1
    return uuidText(bytes);
}

std::string NMOSRegistrationClient::buildDeviceBody(const std::string& deviceId,
                                                    const std::string& nodeId,
                                                    const std::string& label,
                                                    const std::vector<std::string>& senderIds,
                                                    const std::vector<std::string>& receiverIds,
                                                    int64_t versionSeconds, int32_t versionNanos) {
    std::ostringstream json;
    json << "{\n  \"type\": \"device\",\n  \"data\": {\n"
         << "    \"id\": \"" << deviceId << "\",\n"
         << "    \"version\": \"" << versionText(versionSeconds, versionNanos) << "\",\n"
         << "    \"label\": \"" << jsonEscape(label) << "\",\n"
         << "    \"description\": \"\",\n"
         << "    \"tags\": {},\n"
         << "    \"type\": \"urn:x-nmos:device:audio\",\n"
         << "    \"node_id\": \"" << nodeId << "\",\n"
         << "    \"senders\": " << jsonStringArray(senderIds) << ",\n"
         << "    \"receivers\": " << jsonStringArray(receiverIds) << ",\n"
         // No IS-05 connection API here yet, and an empty list is the
         // honest way to say so: a controller reads this to find out
         // whether it can make connections, and a control that answers
         // nothing is worse than one that was never advertised.
         << "    \"controls\": []\n"
         << "  }\n}\n";
    return json.str();
}

std::string NMOSRegistrationClient::buildSourceBody(const std::string& sourceId,
                                                    const std::string& deviceId,
                                                    const NMOSSenderResource& sender,
                                                    int64_t versionSeconds, int32_t versionNanos) {
    std::ostringstream json;
    json << "{\n  \"type\": \"source\",\n  \"data\": {\n"
         << "    \"id\": \"" << sourceId << "\",\n"
         << "    \"version\": \"" << versionText(versionSeconds, versionNanos) << "\",\n"
         << "    \"label\": \"" << jsonEscape(sender.name) << "\",\n"
         << "    \"description\": \"" << jsonEscape(sender.description) << "\",\n"
         << "    \"tags\": {},\n"
         << "    \"caps\": {},\n"
         << "    \"device_id\": \"" << deviceId << "\",\n"
         << "    \"parents\": [],\n"
         << "    \"clock_name\": \"clk0\",\n"
         << "    \"format\": \"urn:x-nmos:format:audio\",\n"
         << "    \"channels\": [";
    for (uint16_t c = 0; c < sender.channels; c++) {
        if (c > 0) json << ", ";
        json << "{ \"label\": \"Channel " << (c + 1) << "\" }";
    }
    json << "]\n  }\n}\n";
    return json.str();
}

std::string NMOSRegistrationClient::buildFlowBody(const std::string& flowId,
                                                  const std::string& sourceId,
                                                  const std::string& deviceId,
                                                  const NMOSSenderResource& sender,
                                                  int64_t versionSeconds, int32_t versionNanos) {
    const int bitDepth = (sender.encoding == "L16") ? 16 : 24;
    std::ostringstream json;
    json << "{\n  \"type\": \"flow\",\n  \"data\": {\n"
         << "    \"id\": \"" << flowId << "\",\n"
         << "    \"version\": \"" << versionText(versionSeconds, versionNanos) << "\",\n"
         << "    \"label\": \"" << jsonEscape(sender.name) << "\",\n"
         << "    \"description\": \"" << jsonEscape(sender.description) << "\",\n"
         << "    \"tags\": {},\n"
         << "    \"source_id\": \"" << sourceId << "\",\n"
         << "    \"device_id\": \"" << deviceId << "\",\n"
         << "    \"parents\": [],\n"
         << "    \"format\": \"urn:x-nmos:format:audio\",\n"
         << "    \"media_type\": \"audio/" << sender.encoding << "\",\n"
         << "    \"sample_rate\": { \"numerator\": " << sender.sampleRate
         << ", \"denominator\": 1 },\n"
         << "    \"bit_depth\": " << bitDepth << "\n"
         << "  }\n}\n";
    return json.str();
}

std::string NMOSRegistrationClient::buildSenderBody(const std::string& senderId,
                                                    const std::string& flowId,
                                                    const std::string& deviceId,
                                                    const NMOSSenderResource& sender,
                                                    int64_t versionSeconds, int32_t versionNanos) {
    std::ostringstream json;
    json << "{\n  \"type\": \"sender\",\n  \"data\": {\n"
         << "    \"id\": \"" << senderId << "\",\n"
         << "    \"version\": \"" << versionText(versionSeconds, versionNanos) << "\",\n"
         << "    \"label\": \"" << jsonEscape(sender.name) << "\",\n"
         << "    \"description\": \"" << jsonEscape(sender.description) << "\",\n"
         << "    \"tags\": {},\n"
         << "    \"flow_id\": \"" << flowId << "\",\n"
         << "    \"device_id\": \"" << deviceId << "\",\n"
         << "    \"transport\": \"urn:x-nmos:transport:rtp.mcast\",\n"
         << "    \"interface_bindings\": [],\n"
         // The SDP for this sender is served over RTSP DESCRIBE, not over
         // HTTP, and manifest_href names an HTTP URL. Null says "ask me
         // another way" instead of pointing at something that will 404.
         << "    \"manifest_href\": null,\n"
         << "    \"subscription\": { \"receiver_id\": null, \"active\": true }\n"
         << "  }\n}\n";
    return json.str();
}

std::string NMOSRegistrationClient::buildReceiverBody(const std::string& receiverId,
                                                      const std::string& deviceId,
                                                      const NMOSReceiverResource& receiver,
                                                      int64_t versionSeconds,
                                                      int32_t versionNanos) {
    std::ostringstream json;
    json << "{\n  \"type\": \"receiver\",\n  \"data\": {\n"
         << "    \"id\": \"" << receiverId << "\",\n"
         << "    \"version\": \"" << versionText(versionSeconds, versionNanos) << "\",\n"
         << "    \"label\": \"" << jsonEscape(receiver.name) << "\",\n"
         << "    \"description\": \"" << jsonEscape(receiver.description) << "\",\n"
         << "    \"tags\": {},\n"
         << "    \"device_id\": \"" << deviceId << "\",\n"
         << "    \"transport\": \"urn:x-nmos:transport:rtp.mcast\",\n"
         << "    \"interface_bindings\": [],\n"
         << "    \"format\": \"urn:x-nmos:format:audio\",\n"
         // What this receiver can take, which is what the RTP path
         // decodes: nothing else belongs here, however much the driver
         // might wish it did.
         << "    \"caps\": { \"media_types\": [\"audio/L16\", \"audio/L24\"] },\n"
         << "    \"subscription\": { \"sender_id\": null, \"active\": "
         << (receiver.active ? "true" : "false") << " }\n"
         << "  }\n}\n";
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

bool NMOSRegistrationClient::postResource(const std::string& body) {
    NMOSRegistry registry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registry = registry_;
    }
    if (!registry.valid()) return false;

    HTTPClient client(registry.host, registry.port);
    const HTTPResponse response =
        client.post(registrationPath(registry.apiVersion), body, "application/json");
    return response.error.empty() && (response.status == 200 || response.status == 201);
}

bool NMOSRegistrationClient::deleteResource(const std::string& type, const std::string& id) {
    NMOSRegistry registry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registry = registry_;
    }
    if (!registry.valid()) return false;

    HTTPClient client(registry.host, registry.port);
    const HTTPResponse response =
        client.del(registrationPath(registry.apiVersion) + "/" + type + "/" + id);
    // 404 means it had already gone, which from here is the same outcome.
    return response.error.empty() && (response.status == 204 || response.status == 404);
}

bool NMOSRegistrationClient::syncResources(const std::vector<NMOSSenderResource>& senders,
                                           const std::vector<NMOSReceiverResource>& receivers) {
    std::string nodeId;
    std::string nodeLabel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        nodeId = node_.id;
        nodeLabel = node_.label;
    }
    if (nodeId.empty()) return false;

    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds);
    const int64_t versionSeconds = seconds.count();
    const int32_t versionNanos = static_cast<int32_t>(nanos.count());

    // One device, always the same id for this node: a driver is one audio
    // device however many streams it carries.
    const std::string deviceId = deriveId(nodeId, "device");

    std::vector<std::string> senderIds;
    std::vector<std::string> receiverIds;
    senderIds.reserve(senders.size());
    receiverIds.reserve(receivers.size());
    for (const NMOSSenderResource& sender : senders) {
        senderIds.push_back(deriveId(nodeId, "sender:" + sender.name));
    }
    for (const NMOSReceiverResource& receiver : receivers) {
        receiverIds.push_back(deriveId(nodeId, "receiver:" + receiver.name));
    }

    // The device names what is under it, so it goes first and the
    // registry never holds a device pointing at things it has not seen.
    bool allAccepted = postResource(buildDeviceBody(deviceId, nodeId, nodeLabel, senderIds,
                                                    receiverIds, versionSeconds, versionNanos));

    std::vector<std::pair<std::string, std::string>> published;
    published.emplace_back("devices", deviceId);

    for (size_t i = 0; i < senders.size(); i++) {
        const std::string sourceId = deriveId(nodeId, "source:" + senders[i].name);
        const std::string flowId = deriveId(nodeId, "flow:" + senders[i].name);

        // Source, then flow, then sender: each names the one before it.
        allAccepted &= postResource(
            buildSourceBody(sourceId, deviceId, senders[i], versionSeconds, versionNanos));
        allAccepted &= postResource(
            buildFlowBody(flowId, sourceId, deviceId, senders[i], versionSeconds, versionNanos));
        allAccepted &= postResource(buildSenderBody(senderIds[i], flowId, deviceId, senders[i],
                                                    versionSeconds, versionNanos));

        published.emplace_back("sources", sourceId);
        published.emplace_back("flows", flowId);
        published.emplace_back("senders", senderIds[i]);
    }

    for (size_t i = 0; i < receivers.size(); i++) {
        allAccepted &= postResource(buildReceiverBody(receiverIds[i], deviceId, receivers[i],
                                                      versionSeconds, versionNanos));
        published.emplace_back("receivers", receiverIds[i]);
    }

    // Whatever we put there last time and have not put there now is a
    // stream that is gone: a registry showing it sends controllers after
    // nothing. Removed in reverse order so nothing is left pointing at
    // something already deleted.
    std::vector<std::pair<std::string, std::string>> previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = published_;
        published_ = published;
    }
    for (auto it = previous.rbegin(); it != previous.rend(); ++it) {
        const bool stillThere = std::find(published.begin(), published.end(), *it) != published.end();
        if (!stillThere) deleteResource(it->first, it->second);
    }

    return allAccepted;
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

    // Everything under the node first, newest to oldest: a registry left
    // holding a sender whose device is gone is a registry with a dangling
    // reference in it.
    std::vector<std::pair<std::string, std::string>> published;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        published = published_;
        published_.clear();
    }
    for (auto it = published.rbegin(); it != published.rend(); ++it) {
        deleteResource(it->first, it->second);
    }

    HTTPClient client(registry.host, registry.port);
    const HTTPResponse response =
        client.del(registrationPath(registry.apiVersion) + "/nodes/" + nodeId);

    registered_.store(false, std::memory_order_relaxed);
    // 204 is the documented answer; 404 means it had already forgotten
    // us, which is the same outcome from here.
    return response.error.empty() && (response.status == 204 || response.status == 404);
}

} // namespace AES67
