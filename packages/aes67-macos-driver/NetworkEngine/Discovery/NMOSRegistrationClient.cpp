//
// NMOSRegistrationClient.cpp
// AES67 macOS Driver
//

#include <ranges>
#include <iterator>
#include "NetworkEngine/Discovery/NMOSRegistrationClient.h"
#include "Driver/DebugLog.h"
#include "NetworkEngine/NetworkInterfaceDetection.h"
#include "Ravenna/RegistryBrowser.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include "NetworkEngine/JsonEscape.h"

#include "Ravenna/HTTPClient.h"
#include "NetworkEngine/Discovery/MDNSBrowser.h"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <thread>

namespace AES67 {

namespace {
/// The node's `interfaces`, which is the one this host uses or none at all.
/// `chassis_id` is null: this is a Mac, not a chassis with a backplane
/// identifier, and the schema takes null for exactly that case.
std::string interfacesJson(const NMOSNodeInfo& node) {
    if (node.interfaceName.empty()) return "[]";
    std::ostringstream json;
    json << "[{ \"name\": \"" << jsonEscape(node.interfaceName) << "\", "
         << "\"chassis_id\": null, \"port_id\": \""
         << jsonEscape(node.interfaceMac.empty() ? std::string("00-00-00-00-00-00")
                                                 : node.interfaceMac)
         << "\" }]";
    return json.str();
}

/// A resource's `interface_bindings`: the node interface it uses, by name.
std::string bindingsJson(const std::string& interfaceName) {
    if (interfaceName.empty()) return "[]";
    return "[\"" + jsonEscape(interfaceName) + "\"]";
}

}  // namespace


namespace {

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

std::string NMOSRegistrationClient::wrapResource(const std::string& type, const std::string& data) {
    return "{\n  \"type\": \"" + type + "\",\n  \"data\": " + data + "\n}\n";
}

std::string NMOSRegistrationClient::buildRegistrationBody(const NMOSNodeInfo& node,
                                                          int64_t versionSeconds,
                                                          int32_t versionNanos) {
    return wrapResource("node", buildNodeData(node, versionSeconds, versionNanos));
}

std::string NMOSRegistrationClient::buildNodeData(const NMOSNodeInfo& node,
                                                  int64_t versionSeconds,
                                                  int32_t versionNanos) {
    // IS-04's version is "<seconds>:<nanoseconds>" and orders updates to a
    // resource. It is TAI in the specification; the registry compares it
    // against what it already holds rather than against its own clock, so
    // what matters here is that it never goes backwards.
    std::ostringstream json;
    json << "{\n"
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
         << "      \"endpoints\": "
         << (node.apiPort == 0
                 ? std::string("[]")
                 : "[{ \"host\": \"" + jsonEscape(node.apiHost) + "\", \"port\": " +
                       std::to_string(node.apiPort) + ", \"protocol\": \"http\" }]")
         << "\n"
         << "    },\n"
         << "    \"services\": [],\n"
         // The clock this node offers. "internal" is the honest answer
         // while the PTP subsystem is opt-in and off by default: a node
         // that claims a PTP clock it is not running is a node a
         // controller will try to slave things to.
         << "    \"clocks\": [{ \"name\": \"clk0\", \"ref_type\": \"internal\" }],\n"
         // The interface this node's streams use, named so that senders and
         // receivers can bind to it: a sender that binds to nothing is a
         // sender a controller cannot tell is reachable. `chassis_id` is null
         // because this is a Mac and not a chassis with a backplane id; the
         // schema takes null for exactly that.
         << "    \"interfaces\": " << interfacesJson(node) << "\n"
         << "  }";
    return json.str();
}

namespace {

/// The sixteen bytes of a UUID written as "8-4-4-4-12".
std::string uuidText(const uint8_t bytes[16]) {
    char text[37];
    (void)std::snprintf(text, sizeof(text),
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
    std::copy_if(text.begin(), text.end(), std::back_inserter(hex), [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
    });
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
                                                    const std::string& controlHref,
                                                    int64_t versionSeconds, int32_t versionNanos) {
    return wrapResource("device", buildDeviceData(deviceId, nodeId, label, senderIds, receiverIds,
                                                   controlHref, versionSeconds, versionNanos));
}

std::string NMOSRegistrationClient::buildDeviceData(const std::string& deviceId,
                                                    const std::string& nodeId,
                                                    const std::string& label,
                                                    const std::vector<std::string>& senderIds,
                                                    const std::vector<std::string>& receiverIds,
                                                    const std::string& controlHref,
                                                    int64_t versionSeconds, int32_t versionNanos) {
    std::ostringstream json;
    json << "{\n"
         << "    \"id\": \"" << deviceId << "\",\n"
         << "    \"version\": \"" << versionText(versionSeconds, versionNanos) << "\",\n"
         << "    \"label\": \"" << jsonEscape(label) << "\",\n"
         << "    \"description\": \"\",\n"
         << "    \"tags\": {},\n"
         << "    \"type\": \"urn:x-nmos:device:audio\",\n"
         << "    \"node_id\": \"" << nodeId << "\",\n"
         << "    \"senders\": " << jsonStringArray(senderIds) << ",\n"
         << "    \"receivers\": " << jsonStringArray(receiverIds) << ",\n"
         // What a controller reads to find out whether it can make
         // connections here. An empty list when there is no Connection
         // API to point at: a control that answers nothing is worse than
         // one that was never advertised.
         << "    \"controls\": "
         << (controlHref.empty()
                 ? std::string("[]")
                 : "[{ \"href\": \"" + jsonEscape(controlHref) +
                       "\", \"type\": \"urn:x-nmos:control:sr-ctrl/v1.1\" }]")
         << "\n"
         << "  }";
    return json.str();
}

std::string NMOSRegistrationClient::buildSourceBody(const std::string& sourceId,
                                                    const std::string& deviceId,
                                                    const NMOSSenderResource& sender,
                                                    int64_t versionSeconds, int32_t versionNanos) {
    return wrapResource("source", buildSourceData(sourceId, deviceId, sender, versionSeconds,
                                                   versionNanos));
}

std::string NMOSRegistrationClient::buildSourceData(const std::string& sourceId,
                                                    const std::string& deviceId,
                                                    const NMOSSenderResource& sender,
                                                    int64_t versionSeconds, int32_t versionNanos) {
    std::ostringstream json;
    json << "{\n"
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
    json << "]\n  }";
    return json.str();
}

std::string NMOSRegistrationClient::buildFlowBody(const std::string& flowId,
                                                  const std::string& sourceId,
                                                  const std::string& deviceId,
                                                  const NMOSSenderResource& sender,
                                                  int64_t versionSeconds, int32_t versionNanos) {
    return wrapResource("flow", buildFlowData(flowId, sourceId, deviceId, sender, versionSeconds,
                                               versionNanos));
}

std::string NMOSRegistrationClient::buildFlowData(const std::string& flowId,
                                                  const std::string& sourceId,
                                                  const std::string& deviceId,
                                                  const NMOSSenderResource& sender,
                                                  int64_t versionSeconds, int32_t versionNanos) {
    const int bitDepth = (sender.encoding == "L16") ? 16 : 24;
    std::ostringstream json;
    json << "{\n"
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
         << "  }";
    return json.str();
}

std::string NMOSRegistrationClient::buildSenderBody(const std::string& senderId,
                                                    const std::string& flowId,
                                                    const std::string& deviceId,
                                                    const NMOSSenderResource& sender,
                                                    int64_t versionSeconds, int32_t versionNanos,
                                                    const std::string& interfaceName) {
    return wrapResource("sender", buildSenderData(senderId, flowId, deviceId, sender,
                                                   versionSeconds, versionNanos,
                                                   interfaceName));
}

std::string NMOSRegistrationClient::buildSenderData(const std::string& senderId,
                                                    const std::string& flowId,
                                                    const std::string& deviceId,
                                                    const NMOSSenderResource& sender,
                                                    int64_t versionSeconds, int32_t versionNanos,
                                                    const std::string& interfaceName) {
    std::ostringstream json;
    json << "{\n"
         << "    \"id\": \"" << senderId << "\",\n"
         << "    \"version\": \"" << versionText(versionSeconds, versionNanos) << "\",\n"
         << "    \"label\": \"" << jsonEscape(sender.name) << "\",\n"
         << "    \"description\": \"" << jsonEscape(sender.description) << "\",\n"
         << "    \"tags\": {},\n"
         << "    \"flow_id\": \"" << flowId << "\",\n"
         << "    \"device_id\": \"" << deviceId << "\",\n"
         << "    \"transport\": \"urn:x-nmos:transport:rtp.mcast\",\n"
         << "    \"interface_bindings\": " << bindingsJson(interfaceName) << ",\n"
         // The SDP for this sender is served over RTSP DESCRIBE, not over
         // HTTP, and manifest_href names an HTTP URL. Null says "ask me
         // another way" instead of pointing at something that will 404.
         << "    \"manifest_href\": null,\n"
         << "    \"subscription\": { \"receiver_id\": null, \"active\": true }\n"
         << "  }";
    return json.str();
}

std::string NMOSRegistrationClient::buildReceiverBody(const std::string& receiverId,
                                                      const std::string& deviceId,
                                                      const NMOSReceiverResource& receiver,
                                                      int64_t versionSeconds,
                                                      int32_t versionNanos,
                                                      const std::string& interfaceName) {
    return wrapResource("receiver", buildReceiverData(receiverId, deviceId, receiver,
                                                       versionSeconds, versionNanos,
                                                       interfaceName));
}

std::string NMOSRegistrationClient::buildReceiverData(const std::string& receiverId,
                                                      const std::string& deviceId,
                                                      const NMOSReceiverResource& receiver,
                                                      int64_t versionSeconds,
                                                      int32_t versionNanos,
                                                      const std::string& interfaceName) {
    std::ostringstream json;
    json << "{\n"
         << "    \"id\": \"" << receiverId << "\",\n"
         << "    \"version\": \"" << versionText(versionSeconds, versionNanos) << "\",\n"
         << "    \"label\": \"" << jsonEscape(receiver.name) << "\",\n"
         << "    \"description\": \"" << jsonEscape(receiver.description) << "\",\n"
         << "    \"tags\": {},\n"
         << "    \"device_id\": \"" << deviceId << "\",\n"
         << "    \"transport\": \"urn:x-nmos:transport:rtp.mcast\",\n"
         << "    \"interface_bindings\": " << bindingsJson(interfaceName) << ",\n"
         << "    \"format\": \"urn:x-nmos:format:audio\",\n"
         // What this receiver can take, which is what the RTP path
         // decodes: nothing else belongs here, however much the driver
         // might wish it did.
         << "    \"caps\": { \"media_types\": [\"audio/L16\", \"audio/L24\"] },\n"
         << "    \"subscription\": { \"sender_id\": null, \"active\": "
         << (receiver.active ? "true" : "false") << " }\n"
         << "  }";
    return json.str();
}

std::vector<NMOSRegistry> NMOSRegistrationClient::discoverRegistries(
    std::chrono::milliseconds waitFor) const {
    std::string interfaceName;
    std::string apiVersion;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        interfaceName = node_.interfaceName;
        apiVersion = registry_.apiVersion.empty() ? std::string("v1.3") : registry_.apiVersion;
    }
    if (interfaceName.empty()) return {};

    uint32_t addressV4 = 0;
    {
        const std::string address = NetworkInterfaceDetection::getInterfaceIPAddress(interfaceName);
        struct in_addr parsed {};
        if (!address.empty() && ::inet_pton(AF_INET, address.c_str(), &parsed) == 1) {
            addressV4 = ntohl(parsed.s_addr);
        }
    }

    Ravenna::RegistryBrowser browser(interfaceName, addressV4, apiVersion);
    std::string error;
    if (!browser.start(error)) {
        // No responder, or the port is taken. A lost convenience, never a
        // failure: this is how a plant with no registry at all behaves.
        return {};
    }

    // Asked once at the start and answered for the rest of the window. A
    // responder announces unprompted when a service appears, so the listening
    // matters as much as the question.
    const auto deadline = std::chrono::steady_clock::now() + waitFor;
    while (std::chrono::steady_clock::now() < deadline) {
        browser.service(100);
    }

    std::vector<NMOSRegistry> found;
    for (const Ravenna::NmosRegistry& one : browser.registries()) {
        NMOSRegistry registry;
        registry.host = one.host;
        registry.port = one.port;
        registry.apiVersion = one.apiVersion;
        found.push_back(std::move(registry));
    }
    browser.stop();
    return found;
}

std::optional<NMOSRegistry> NMOSRegistrationClient::discoverRegistry(
    std::chrono::milliseconds waitFor) const {
    const std::vector<NMOSRegistry> found = discoverRegistries(waitFor);
    if (found.empty()) return std::nullopt;
    return found.front();
}

void NMOSRegistrationClient::versionNow(int64_t& seconds, int32_t& nanos) const {
    if (versionSource_) {
        versionSource_(seconds, nanos);
        return;
    }
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto whole = std::chrono::duration_cast<std::chrono::seconds>(now);
    seconds = whole.count();
    nanos = static_cast<int32_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - whole).count());
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

    int64_t seconds = 0;
    int32_t nanos = 0;
    versionNow(seconds, nanos);

    HTTPClient client(registry.host, registry.port);
    const HTTPResponse response = client.post(registrationPath(registry.apiVersion),
                                              buildRegistrationBody(node, seconds, nanos),
                                              "application/json");

    if (!response.error.empty()) {
        registered_.store(false, std::memory_order_relaxed);
        return false;
    }

    // 201 is a new registration. 200 is the registry saying it already holds
    // this id, which IS-04 sec 4.2 does not let a node simply accept: what it
    // holds may be a stale copy of a node that never went away cleanly, and
    // the specified way out is to delete it and register again from nothing.
    // Taking the 200 as success left the registry serving whatever it had.
    if (response.status == 200) {
        HTTPClient remover(registry.host, registry.port);
        (void)remover.del(registrationPath(registry.apiVersion) + "/" + node.id);
        const HTTPResponse again = client.post(registrationPath(registry.apiVersion),
                                               buildRegistrationBody(node, seconds, nanos),
                                               "application/json");
        const bool retook = again.error.empty() && again.status == 201;
        registered_.store(retook, std::memory_order_relaxed);
        return retook;
    }

    const bool accepted = response.status == 201;
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
        // The link is browsed for the whole time this runs, not asked once
        // when a beat has already been lost. IS-04 sec 4.2 has a registry
        // forget a node twelve seconds after it goes quiet, and a controller
        // watching the changeover allows about one heartbeat interval for it,
        // so a failover that starts by opening a socket and waiting for
        // answers has already taken too long. Keeping the browser open means
        // the candidates are known before they are needed and the failover
        // costs one HTTP round trip.
        std::string interfaceName;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            interfaceName = node_.interfaceName;
        }
        std::unique_ptr<Ravenna::RegistryBrowser> browser;
        if (!interfaceName.empty()) {
            uint32_t addressV4 = 0;
            const std::string address =
                NetworkInterfaceDetection::getInterfaceIPAddress(interfaceName);
            struct in_addr parsed {};
            if (!address.empty() && ::inet_pton(AF_INET, address.c_str(), &parsed) == 1) {
                addressV4 = ntohl(parsed.s_addr);
            }
            browser = std::make_unique<Ravenna::RegistryBrowser>(interfaceName, addressV4);
            std::string error;
            if (!browser->start(error)) {
                AES67_LOGF("NMOS: no registry browser, so no failover: %s", error.c_str());
                browser.reset();
            }
        }

        while (running_.load(std::memory_order_acquire)) {
            if (!heartbeat()) {
                // One lost beat is enough. There is nothing to be learnt from
                // losing a second: a registry that refused the connection is
                // not going to take the next one either, and the node has
                // twelve seconds before it is forgotten.
                if (browser && running_.load(std::memory_order_acquire) &&
                    failOverTo(browser->registries())) {
                    // Beat the new one at once rather than at the next tick,
                    // so the changeover is one interval and not two.
                    heartbeat();
                }
            }

            // Waited against the clock and not by counting slices. The
            // browser's service() returns as soon as a packet arrives, which
            // on a link with any mDNS traffic is far short of the slice it was
            // given; counting fifty of those is not five seconds, and the beat
            // rate rose with the chatter on the link.
            //
            // The slice is what makes stop() prompt. It is a duration in its
            // own right and not a division of the period: kHeartbeatPeriod is
            // std::chrono::seconds, whose representation is integral, so
            // `kHeartbeatPeriod / 50` is seconds(0) and the loop it was
            // written for did not sleep at all -- it beat as fast as the
            // socket would go, two hundred thousand times in the twenty-four
            // seconds it took to notice.
            constexpr auto slice = std::chrono::milliseconds(100);
            const auto due = std::chrono::steady_clock::now() + kHeartbeatPeriod;
            while (running_.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < due) {
                // The wait doubles as the browser's service: what a responder
                // announces while this is asleep is what the next failover
                // needs to already know.
                if (browser) {
                    browser->service(static_cast<int>(slice.count()));
                } else {
                    std::this_thread::sleep_for(slice);
                }
            }
        }
        if (browser) browser->stop();
    });
}

bool NMOSRegistrationClient::failOverTo(const std::vector<Ravenna::NmosRegistry>& candidates) {
    std::string current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current = registry_.host + ":" + std::to_string(registry_.port);
    }

    // Lowest priority first, which is the order the browser keeps them in.
    // Whether this is still wanted is the caller's question, not this one's:
    // the heartbeat thread asks before calling, and anything else that asks
    // for a failover wants one.
    for (const Ravenna::NmosRegistry& candidate : candidates) {
        // The one that just went quiet is not tried again here: it is the one
        // that failed, and a node that keeps choosing it never moves.
        if (candidate.endpoint() == current) continue;

        NMOSRegistry moved;
        moved.host = candidate.host;
        moved.port = candidate.port;
        moved.apiVersion = candidate.apiVersion;
        if (registerWith(moved)) {
            AES67_LOGF("NMOS: the registry at %s stopped answering; moved to %s", current.c_str(),
                       candidate.endpoint().c_str());
            return true;
        }
    }
    return false;
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
                                           const std::vector<NMOSReceiverResource>& receivers,
                                           const std::string& controlHref) {
    std::string nodeId;
    std::string nodeLabel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        nodeId = node_.id;
        nodeLabel = node_.label;
    }
    if (nodeId.empty()) return false;

    int64_t versionSeconds = 0;
    int32_t versionNanos = 0;
    versionNow(versionSeconds, versionNanos);

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
                                                    receiverIds, controlHref, versionSeconds,
                                                    versionNanos));

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
                                                    versionSeconds, versionNanos,
                                                    node_.interfaceName));

        published.emplace_back("sources", sourceId);
        published.emplace_back("flows", flowId);
        published.emplace_back("senders", senderIds[i]);
    }

    for (size_t i = 0; i < receivers.size(); i++) {
        allAccepted &= postResource(buildReceiverBody(receiverIds[i], deviceId, receivers[i],
                                                      versionSeconds, versionNanos,
                                                      node_.interfaceName));
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
    for (const auto& resource : std::ranges::reverse_view(previous)) {
        const bool stillThere =
            std::find(published.begin(), published.end(), resource) != published.end();
        if (!stillThere) deleteResource(resource.first, resource.second);
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
    for (const auto& resource : std::ranges::reverse_view(published)) {
        deleteResource(resource.first, resource.second);
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
