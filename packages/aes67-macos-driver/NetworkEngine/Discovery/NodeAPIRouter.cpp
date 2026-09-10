//
// NodeAPIRouter.cpp
// AES67 macOS Driver
//

#include "NetworkEngine/Discovery/NodeAPIRouter.h"

#include <chrono>

namespace AES67 {

namespace {

std::vector<std::string> pathPieces(const std::string& path) {
    std::vector<std::string> pieces;
    std::string piece;
    for (const char c : path) {
        if (c == '/') {
            if (!piece.empty()) pieces.push_back(piece);
            piece.clear();
        } else {
            piece += c;
        }
    }
    if (!piece.empty()) pieces.push_back(piece);
    return pieces;
}

std::string jsonStringList(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ",";
        out += "\"" + items[i] + "\"";
    }
    return out + "]";
}

std::string jsonArrayOf(const std::vector<std::string>& objects) {
    std::string out = "[";
    for (size_t i = 0; i < objects.size(); ++i) {
        if (i) out += ",";
        out += objects[i];
    }
    return out + "]";
}

}  // namespace

NodeAPIRouter::NodeAPIRouter(NMOSNodeInfo node, std::string controlHref,
                             NMOSSenderLister senders, NMOSReceiverLister receivers)
    : node_(std::move(node)), controlHref_(std::move(controlHref)),
      senders_(std::move(senders)), receivers_(std::move(receivers)) {
    touchLocked();
}

void NodeAPIRouter::touchLocked() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
    versionSeconds_ = seconds.count();
    versionNanos_ = static_cast<int32_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds).count());
}

void NodeAPIRouter::touch() {
    std::lock_guard<std::mutex> lock(mutex_);
    touchLocked();
}

void NodeAPIRouter::setEndpoint(const std::string& apiHost, uint16_t apiPort,
                                const std::string& controlHref) {
    std::lock_guard<std::mutex> lock(mutex_);
    node_.apiHost = apiHost;
    node_.apiPort = apiPort;
    node_.href = "http://" + apiHost + ":" + std::to_string(apiPort) + "/";
    controlHref_ = controlHref;
    touchLocked();
}

std::string NodeAPIRouter::deviceId() const {
    return NMOSRegistrationClient::deriveId(node_.id, "device");
}

ConnectionAPIServer::Reply NodeAPIRouter::error(int status, const std::string& text) const {
    return {status, "application/json",
            "{\"code\": " + std::to_string(status) + ", \"error\": \"" + text +
                "\", \"debug\": null}"};
}

std::string NodeAPIRouter::nodeData() const {
    return NMOSRegistrationClient::buildNodeData(node_, versionSeconds_, versionNanos_);
}

std::string NodeAPIRouter::deviceData() const {
    std::vector<std::string> senderIds;
    for (const NMOSSenderResource& sender : senders_()) {
        senderIds.push_back(NMOSRegistrationClient::deriveId(node_.id, "sender:" + sender.name));
    }
    std::vector<std::string> receiverIds;
    for (const NMOSReceiverResource& receiver : receivers_()) {
        receiverIds.push_back(
            NMOSRegistrationClient::deriveId(node_.id, "receiver:" + receiver.name));
    }
    return NMOSRegistrationClient::buildDeviceData(deviceId(), node_.id, node_.label, senderIds,
                                                   receiverIds, controlHref_, versionSeconds_,
                                                   versionNanos_);
}

ConnectionAPIServer::Reply NodeAPIRouter::route(const std::string& method,
                                                const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::vector<std::string> pieces = pathPieces(path);
    if (pieces.size() < 2 || pieces[0] != "x-nmos" || pieces[1] != "node") {
        return error(404, "not found");
    }
    if (method != "GET") return error(405, "method not allowed");
    if (pieces.size() == 2) return {200, "application/json", jsonStringList({"v1.3/"})};
    if (pieces[2] != kApiVersion) return error(404, "version not served");
    if (pieces.size() == 3) {
        return {200, "application/json",
                jsonStringList({"self/", "devices/", "sources/", "flows/", "senders/",
                                "receivers/", "subscriptions/"})};
    }

    const std::string& collection = pieces[3];
    const bool wantsOne = pieces.size() >= 5;
    const std::string wantedId = wantsOne ? pieces[4] : std::string{};

    if (collection == "self") return {200, "application/json", nodeData()};
    if (collection == "subscriptions") return error(501, "subscriptions are not served");

    // Every collection is built the same way: the objects with their ids,
    // then either the whole list or the one asked for.
    std::vector<std::pair<std::string, std::string>> objects;  // id, data

    if (collection == "devices") {
        objects.emplace_back(deviceId(), deviceData());
    } else if (collection == "sources" || collection == "flows" || collection == "senders") {
        for (const NMOSSenderResource& sender : senders_()) {
            const std::string sourceId =
                NMOSRegistrationClient::deriveId(node_.id, "source:" + sender.name);
            const std::string flowId =
                NMOSRegistrationClient::deriveId(node_.id, "flow:" + sender.name);
            const std::string senderId =
                NMOSRegistrationClient::deriveId(node_.id, "sender:" + sender.name);
            if (collection == "sources") {
                objects.emplace_back(sourceId, NMOSRegistrationClient::buildSourceData(
                                                   sourceId, deviceId(), sender,
                                                   versionSeconds_, versionNanos_));
            } else if (collection == "flows") {
                objects.emplace_back(flowId, NMOSRegistrationClient::buildFlowData(
                                                 flowId, sourceId, deviceId(), sender,
                                                 versionSeconds_, versionNanos_));
            } else {
                objects.emplace_back(senderId, NMOSRegistrationClient::buildSenderData(
                                                   senderId, flowId, deviceId(), sender,
                                                   versionSeconds_, versionNanos_));
            }
        }
    } else if (collection == "receivers") {
        for (const NMOSReceiverResource& receiver : receivers_()) {
            const std::string receiverId =
                NMOSRegistrationClient::deriveId(node_.id, "receiver:" + receiver.name);
            objects.emplace_back(receiverId, NMOSRegistrationClient::buildReceiverData(
                                                 receiverId, deviceId(), receiver,
                                                 versionSeconds_, versionNanos_));
        }
    } else {
        return error(404, "not found");
    }

    if (!wantsOne) {
        std::vector<std::string> datas;
        datas.reserve(objects.size());
        for (const auto& object : objects) datas.push_back(object.second);
        return {200, "application/json", jsonArrayOf(datas)};
    }
    for (const auto& object : objects) {
        if (object.first == wantedId) return {200, "application/json", object.second};
    }
    return error(404, "no such resource");
}

}  // namespace AES67
