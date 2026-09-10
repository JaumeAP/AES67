#include <algorithm>
#include "Ravenna/NodeApi.h"

#include <arpa/inet.h>

#include <cstdio>
#include <functional>

namespace AES67::Ravenna {
namespace {

ApiResponse jsonResponse(int status, const JsonValue& value) {
    ApiResponse response;
    response.status = status;
    response.body = value.serialise();
    return response;
}

ApiResponse errorResponse(int status, const std::string& detail) {
    JsonObject error;
    error["code"] = JsonValue(status);
    error["error"] = JsonValue(status == 404 ? "Not Found" : "Method Not Allowed");
    error["debug"] = JsonValue(detail);
    return jsonResponse(status, JsonValue(error));
}

std::vector<std::string> segmentsOf(const std::string& path) {
    std::vector<std::string> segments;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string segment =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!segment.empty()) segments.push_back(segment);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return segments;
}

std::string addressText(uint32_t addressV4) {
    struct in_addr address {};
    address.s_addr = htonl(addressV4);
    char text[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &address, text, sizeof(text));
    return text;
}

/// IS-04 wants a version on every resource: TAI seconds and nanoseconds,
/// bumped whenever the resource changes. These are built fresh on every read,
/// so the version is when they were built.
std::string versionNow() {
    struct timespec now {};
    ::clock_gettime(CLOCK_REALTIME, &now);
    return std::to_string(static_cast<long long>(now.tv_sec)) + ":" +
           std::to_string(static_cast<long long>(now.tv_nsec));
}

JsonValue tagsEmpty() { return JsonValue(JsonObject{}); }

/// DNS-SD allows one instance label in front of the service type, and a dot
/// is what separates labels rather than a character inside one. A name with a
/// dot in it -- a host name used as a label -- becomes two, and a responder
/// drops the record instead of correcting it. So the dot goes, and what is
/// left is still the name a person picked.
std::string oneLabel(const std::string& name) {
    std::string label = name;
    std::replace(label.begin(), label.end(), '.', ' ');
    return label;
}

}  // namespace

std::string stableUuidFrom(const std::string& name) {
    // Two independent hashes give the 128 bits a UUID needs. What matters is
    // that the same name always gives the same id and two names do not
    // collide; a controller never does anything with a UUID but compare it.
    const uint64_t high = std::hash<std::string>{}("aes67-ravenna/" + name);
    const uint64_t low = std::hash<std::string>{}(name + "/aes67-ravenna");

    char text[37] = {};
    (void)std::snprintf(text, sizeof(text), "%08llx-%04llx-5%03llx-a%03llx-%012llx", // 36 chars into 37, always
                  static_cast<unsigned long long>(high >> 32),
                  static_cast<unsigned long long>((high >> 16) & 0xFFFF),
                  static_cast<unsigned long long>(high & 0x0FFF),
                  static_cast<unsigned long long>((low >> 52) & 0x0FFF),
                  static_cast<unsigned long long>(low & 0xFFFFFFFFFFFFULL));
    return text;
}

std::string NodeApi::senderIdFor(const std::string& sessionName) const {
    // The connection API holds the id this sender is routed by; the session
    // catalogue holds the name it is known by. The label is what joins them.
    for (const std::string& id : connections_.senderIds()) {
        const auto sender = connections_.sender(id);
        if (sender && sender->label == sessionName) return id;
    }
    // Nothing offers this session over IS-05, so nobody can address it and a
    // derived id is as good as any: it still has to be stable and unique.
    return stableUuidFrom(identity_.nodeId + "/sender/" + sessionName);
}

std::string NodeApi::sourceIdFor(const std::string& sessionName) const {
    return stableUuidFrom(identity_.nodeId + "/source/" + sessionName);
}

std::string NodeApi::flowIdFor(const std::string& sessionName) const {
    return stableUuidFrom(identity_.nodeId + "/flow/" + sessionName);
}

JsonValue NodeApi::self() const {
    const std::string base = "http://" + addressText(identity_.addressV4) + ":" +
                             std::to_string(identity_.apiPort);

    JsonObject api;
    api["versions"] = JsonValue(JsonArray{JsonValue("v1.3")});
    api["endpoints"] = JsonValue(JsonArray{JsonValue(JsonObject{
        {"host", JsonValue(addressText(identity_.addressV4))},
        {"port", JsonValue(static_cast<int>(identity_.apiPort))},
        {"protocol", JsonValue("http")},
        {"authorization", JsonValue(false)}})});

    // What this device's media clock follows. "internal" is the honest answer
    // when nothing disciplines it, and a controller that sees ptp with a
    // grandmaster named can tell whether two devices share a clock.
    JsonObject clock;
    clock["name"] = JsonValue("clk0");
    if (identity_.ptpGrandmaster.empty()) {
        clock["ref_type"] = JsonValue("internal");
    } else {
        clock["ref_type"] = JsonValue("ptp");
        clock["traceable"] = JsonValue(false);
        clock["version"] = JsonValue("IEEE1588-2008");
        clock["gmid"] = JsonValue(identity_.ptpGrandmaster);
        clock["locked"] = JsonValue(true);
    }

    JsonObject node;
    node["id"] = JsonValue(identity_.nodeId);
    node["version"] = JsonValue(versionNow());
    node["label"] = JsonValue(identity_.label);
    node["description"] = JsonValue(identity_.description);
    node["tags"] = tagsEmpty();
    node["href"] = JsonValue(base + "/");
    node["hostname"] = JsonValue(identity_.hostName);
    node["api"] = JsonValue(api);
    node["caps"] = JsonValue(JsonObject{});
    node["services"] = JsonValue(JsonArray{});
    node["clocks"] = JsonValue(JsonArray{JsonValue(clock)});
    node["interfaces"] = JsonValue(JsonArray{JsonValue(JsonObject{
        {"name", JsonValue("eth0")},
        {"chassis_id", JsonValue()},
        {"port_id", JsonValue()}})});
    return JsonValue(node);
}

JsonValue NodeApi::devices() const {
    JsonArray senderIds;
    for (const std::string& name : catalogue_.names()) {
        senderIds.emplace_back(senderIdFor(name));
    }

    JsonArray receiverIds;
    for (const std::string& id : connections_.receiverIds()) {
        receiverIds.emplace_back(id);
    }

    const std::string base = "http://" + addressText(identity_.addressV4) + ":" +
                             std::to_string(identity_.apiPort);

    JsonObject device;
    device["id"] = JsonValue(identity_.deviceId);
    device["version"] = JsonValue(versionNow());
    device["label"] = JsonValue(identity_.label);
    device["description"] = JsonValue(identity_.description);
    device["tags"] = tagsEmpty();
    device["type"] = JsonValue("urn:x-nmos:device:audio");
    device["node_id"] = JsonValue(identity_.nodeId);
    device["senders"] = JsonValue(senderIds);
    device["receivers"] = JsonValue(receiverIds);
    // Where the connection API for this device is. It is what a controller
    // follows to go from "this device exists" to "take that stream".
    device["controls"] = JsonValue(JsonArray{
        JsonValue(JsonObject{{"href", JsonValue(base + std::string(kConnectionApiRoot) + "/")},
                             {"type", JsonValue("urn:x-nmos:control:sr-ctrl/v1.1")},
                             {"authorization", JsonValue(false)}}),
        JsonValue(JsonObject{{"href", JsonValue(base + "/x-nmos/channelmapping/v1.0/")},
                             {"type", JsonValue("urn:x-nmos:control:cm-ctrl/v1.0")},
                             {"authorization", JsonValue(false)}})});
    return JsonValue(JsonArray{JsonValue(device)});
}

JsonValue NodeApi::sources() const {
    JsonArray items;
    for (const std::string& name : catalogue_.names()) {
        const auto session = catalogue_.session(name);
        if (!session) continue;

        JsonArray channels;
        for (uint16_t i = 0; i < session->sdp.numChannels; ++i) {
            channels.emplace_back(JsonObject{
                {"label", JsonValue("Channel " + std::to_string(i + 1))}});
        }

        JsonObject source;
        source["id"] = JsonValue(sourceIdFor(name));
        source["version"] = JsonValue(versionNow());
        source["label"] = JsonValue(name);
        source["description"] = JsonValue("");
        source["tags"] = tagsEmpty();
        source["caps"] = JsonValue(JsonObject{});
        source["device_id"] = JsonValue(identity_.deviceId);
        source["parents"] = JsonValue(JsonArray{});
        source["clock_name"] = JsonValue("clk0");
        source["format"] = JsonValue("urn:x-nmos:format:audio");
        source["channels"] = JsonValue(channels);
        items.emplace_back(source);
    }
    return JsonValue(items);
}

JsonValue NodeApi::flows() const {
    JsonArray items;
    for (const std::string& name : catalogue_.names()) {
        const auto session = catalogue_.session(name);
        if (!session) continue;

        // The encoding as IS-04 names it: L24 is audio/L24, which is the same
        // thing the SDP's rtpmap says and has to stay the same thing.
        JsonObject flow;
        flow["id"] = JsonValue(flowIdFor(name));
        flow["version"] = JsonValue(versionNow());
        flow["label"] = JsonValue(name);
        flow["description"] = JsonValue("");
        flow["tags"] = tagsEmpty();
        flow["device_id"] = JsonValue(identity_.deviceId);
        flow["source_id"] = JsonValue(sourceIdFor(name));
        flow["parents"] = JsonValue(JsonArray{});
        flow["format"] = JsonValue("urn:x-nmos:format:audio");
        flow["media_type"] = JsonValue("audio/" + session->sdp.encoding);
        flow["sample_rate"] = JsonValue(JsonObject{
            {"numerator", JsonValue(static_cast<int>(session->sdp.sampleRate))},
            {"denominator", JsonValue(1)}});
        flow["bit_depth"] =
            JsonValue(session->sdp.encoding == "L16" ? 16 : 24);
        items.emplace_back(flow);
    }
    return JsonValue(items);
}

JsonValue NodeApi::senders() const {
    const std::string base = "http://" + addressText(identity_.addressV4) + ":" +
                             std::to_string(identity_.apiPort);

    JsonArray items;
    for (const std::string& name : catalogue_.names()) {
        const auto session = catalogue_.session(name);
        if (!session) continue;

        JsonObject sender;
        sender["id"] = JsonValue(senderIdFor(name));
        sender["version"] = JsonValue(versionNow());
        sender["label"] = JsonValue(name);
        sender["description"] = JsonValue("");
        sender["tags"] = tagsEmpty();
        sender["device_id"] = JsonValue(identity_.deviceId);
        sender["flow_id"] = JsonValue(flowIdFor(name));
        sender["transport"] = JsonValue(kTransportRtpMulticast);
        sender["interface_bindings"] = JsonValue(JsonArray{JsonValue("eth0")});
        // The SDP, at the address the connection API serves it from. A
        // controller takes it from here and gives it to a receiver.
        sender["manifest_href"] =
            JsonValue(base + std::string(kConnectionApiRoot) + "/single/senders/" +
                      senderIdFor(name) + "/transportfile/");
        sender["subscription"] = JsonValue(JsonObject{
            {"receiver_id", JsonValue()}, {"active", JsonValue(true)}});
        items.emplace_back(sender);
    }
    return JsonValue(items);
}

JsonValue NodeApi::receivers() const {
    JsonArray items;
    for (const std::string& id : connections_.receiverIds()) {
        const auto connection = connections_.receiver(id);
        if (!connection) continue;

        const bool active = connection->active.masterEnable;

        JsonObject receiver;
        receiver["id"] = JsonValue(id);
        receiver["version"] = JsonValue(versionNow());
        receiver["label"] = JsonValue(connection->label.empty() ? id : connection->label);
        receiver["description"] = JsonValue("");
        receiver["tags"] = tagsEmpty();
        receiver["device_id"] = JsonValue(identity_.deviceId);
        receiver["transport"] = JsonValue(kTransportRtpMulticast);
        receiver["interface_bindings"] = JsonValue(JsonArray{JsonValue("eth0")});
        receiver["format"] = JsonValue("urn:x-nmos:format:audio");
        receiver["caps"] = JsonValue(JsonObject{
            {"media_types", JsonValue(JsonArray{JsonValue("audio/L24"), JsonValue("audio/L16")})}});
        receiver["subscription"] = JsonValue(JsonObject{
            {"sender_id", JsonValue()}, {"active", JsonValue(active)}});
        items.emplace_back(receiver);
    }
    return JsonValue(items);
}

SessionAdvertisement NodeApi::advertisement() const {
    SessionAdvertisement node;
    node.instanceName = oneLabel(identity_.label);
    node.hostName = identity_.hostName;
    node.port = identity_.apiPort;
    node.addressV4 = identity_.addressV4;
    node.serviceType = kNmosNodeService;
    node.subtype.clear();  // an NMOS node has no subtype to browse for
    // What IS-04 sec 3 says a node's TXT carries, and what a controller reads
    // before it opens a single connection.
    node.txtEntries = {"api_ver=v1.3", "api_proto=http", "api_auth=false", "ver_slf=0"};
    return node;
}

ApiResponse NodeApi::handle(const std::string& method, const std::string& path,
                            const std::string& body) {
    (void)body;
    const std::string root = kNodeApiRoot;
    if (path.rfind(root, 0) != 0) return errorResponse(404, "this device serves " + root);
    if (method != "GET") return errorResponse(405, "the node API is read-only");

    const std::vector<std::string> segments = segmentsOf(path.substr(root.size()));

    if (segments.empty()) {
        return jsonResponse(200, JsonValue(JsonArray{
                                     JsonValue("devices/"), JsonValue("flows/"),
                                     JsonValue("receivers/"), JsonValue("self/"),
                                     JsonValue("senders/"), JsonValue("sources/")}));
    }

    const std::string& collection = segments[0];
    JsonValue found;

    if (collection == "self") return jsonResponse(200, self());
    if (collection == "devices") found = devices();
    else if (collection == "sources") found = sources();
    else if (collection == "flows") found = flows();
    else if (collection == "senders") found = senders();
    else if (collection == "receivers") found = receivers();
    else return errorResponse(404, "no such collection");

    if (segments.size() == 1) return jsonResponse(200, found);

    // One resource by id.
    const JsonArray& items = found.asArray();
    const auto match = std::find_if(items.begin(), items.end(), [&](const JsonValue& item) {
        return item["id"].asString() == segments[1];
    });
    if (match != items.end()) return jsonResponse(200, *match);
    return errorResponse(404, "no " + collection + " with id " + segments[1]);
}

}  // namespace AES67::Ravenna
