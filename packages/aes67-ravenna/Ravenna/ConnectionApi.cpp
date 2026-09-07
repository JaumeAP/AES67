#include "Ravenna/ConnectionApi.h"

#include <algorithm>
#include <ctime>

namespace AES67::Ravenna {
namespace {

/// IS-05 carries times as TAI seconds and nanoseconds, "<seconds>:<nanos>".
/// The clock underneath is the system's, which is UTC: the difference is the
/// leap seconds, and a device with no traceable time cannot know them. So
/// this reports what it has and the field means "when this took effect here",
/// which is what a controller uses it for.
std::string nowAsTaiString() {
    struct timespec now {};
    ::clock_gettime(CLOCK_REALTIME, &now);
    return std::to_string(static_cast<long long>(now.tv_sec)) + ":" +
           std::to_string(static_cast<long long>(now.tv_nsec));
}

ApiResponse jsonResponse(int status, const JsonValue& value) {
    ApiResponse response;
    response.status = status;
    response.body = value.serialise();
    return response;
}

ApiResponse errorResponse(int status, const std::string& detail) {
    // IS-05 sec 5: an error is an object with the code, a summary and the
    // detail, and a controller shows the detail to a person.
    JsonObject error;
    error["code"] = JsonValue(status);
    error["error"] = JsonValue(status == 404 ? "Not Found"
                               : status == 501 ? "Not Implemented"
                                               : "Bad Request");
    error["debug"] = JsonValue(detail);
    return jsonResponse(status, JsonValue(error));
}

ApiResponse listResponse(const std::vector<std::string>& entries) {
    JsonArray items;
    items.reserve(entries.size());
    for (const std::string& entry : entries) items.push_back(JsonValue(entry));
    return jsonResponse(200, JsonValue(items));
}

/// Splits a path into its segments, dropping the empty ones a trailing slash
/// leaves behind.
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

}  // namespace

JsonValue stateAsJson(const ConnectionState& state, bool includeTransportFile) {
    JsonObject activation;
    activation["mode"] = state.activationMode == "null" ? JsonValue()
                                                        : JsonValue(state.activationMode);
    activation["requested_time"] = JsonValue();
    activation["activation_time"] =
        state.activationTime.empty() ? JsonValue() : JsonValue(state.activationTime);

    JsonObject object;
    object["master_enable"] = JsonValue(state.masterEnable);
    object["activation"] = JsonValue(activation);
    // One leg, because one stream: IS-05 carries an array here so a device
    // with a redundant pair can describe both, and saying two when there is
    // one is how a controller ends up waiting for a stream nobody sends.
    object["transport_params"] = JsonValue(JsonArray{JsonValue(JsonObject{})});

    if (includeTransportFile) {
        JsonObject file;
        if (state.transportFile.empty()) {
            file["data"] = JsonValue();
            file["type"] = JsonValue();
        } else {
            file["data"] = JsonValue(state.transportFile);
            file["type"] = JsonValue("application/sdp");
        }
        object["transport_file"] = JsonValue(file);
    }
    return JsonValue(object);
}

void ConnectionApi::addSender(const ConnectionSender& sender) {
    ConnectionSender stored = sender;
    stored.staged.transportFile = stored.sdp;
    stored.active.transportFile = stored.sdp;
    senders_[stored.id] = stored;
}

void ConnectionApi::addReceiver(const ConnectionReceiver& receiver) {
    receivers_[receiver.id] = receiver;
}

std::optional<ConnectionSender> ConnectionApi::sender(const std::string& id) const {
    const auto found = senders_.find(id);
    if (found == senders_.end()) return std::nullopt;
    return found->second;
}

std::optional<ConnectionReceiver> ConnectionApi::receiver(const std::string& id) const {
    const auto found = receivers_.find(id);
    if (found == receivers_.end()) return std::nullopt;
    return found->second;
}

std::vector<std::string> ConnectionApi::senderIds() const {
    std::vector<std::string> ids;
    ids.reserve(senders_.size());
    for (const auto& [id, sender] : senders_) ids.push_back(id);
    return ids;
}

std::vector<std::string> ConnectionApi::receiverIds() const {
    std::vector<std::string> ids;
    ids.reserve(receivers_.size());
    for (const auto& [id, receiver] : receivers_) ids.push_back(id);
    return ids;
}

ApiResponse ConnectionApi::patchStagedReceiver(const std::string& id, const std::string& body) {
    const auto found = receivers_.find(id);
    if (found == receivers_.end()) return errorResponse(404, "no receiver called " + id);

    JsonValue patch;
    std::string error;
    if (!parseJson(body, patch, error)) return errorResponse(400, "the body is not JSON: " + error);
    if (!patch.isObject()) return errorResponse(400, "the body has to be an object");

    ConnectionState staged = found->second.staged;

    if (patch.has("master_enable")) {
        const JsonValue& enable = patch["master_enable"];
        if (!enable.isBool()) return errorResponse(400, "master_enable has to be a boolean");
        staged.masterEnable = enable.asBool();
    }

    if (patch.has("transport_file")) {
        const JsonValue& file = patch["transport_file"];
        if (!file.isObject()) return errorResponse(400, "transport_file has to be an object");

        const JsonValue& type = file["type"];
        const JsonValue& data = file["data"];
        if (data.isNull()) {
            staged.transportFile.clear();
        } else {
            if (!data.isString()) return errorResponse(400, "transport_file.data has to be a string");
            // A controller that sends something else has misunderstood what
            // this receiver is; accepting it would mean failing later, on the
            // wire, where nobody is watching.
            if (!type.isString() || type.asString() != "application/sdp") {
                return errorResponse(400,
                                     "this receiver takes application/sdp transport files");
            }
            staged.transportFile = data.asString();
        }
    }

    std::string activationMode;
    if (patch.has("activation")) {
        const JsonValue& activation = patch["activation"];
        if (!activation.isObject()) return errorResponse(400, "activation has to be an object");

        const JsonValue& mode = activation["mode"];
        if (!mode.isNull()) {
            if (!mode.isString()) return errorResponse(400, "activation.mode has to be a string");
            activationMode = mode.asString();
            if (activationMode != "activate_immediate") {
                // Said plainly rather than accepted and dropped: a scheduled
                // activation that never happens is worse than one refused.
                return errorResponse(501, "only activate_immediate is implemented, not " +
                                              activationMode);
            }
        }
    }

    found->second.staged = staged;

    if (activationMode.empty()) {
        // Staged and not activated, which is the normal first half of the
        // exchange: a controller stages, checks, then activates.
        return jsonResponse(200, stateAsJson(found->second.staged, true));
    }

    if (staged.masterEnable && staged.transportFile.empty()) {
        return errorResponse(400, "cannot enable a receiver with no transport file");
    }

    if (onActivation_) {
        std::string reason;
        if (!onActivation_(id, staged.transportFile, staged.masterEnable, reason)) {
            return errorResponse(400, "the receiver refused it: " + reason);
        }
    }

    ConnectionState active = staged;
    active.activationMode = "activate_immediate";
    active.activationTime = nowAsTaiString();
    found->second.active = active;

    // IS-05: what was staged moves to active and the staged activation is
    // cleared, so a controller reading staged afterwards does not see an
    // activation waiting to happen again.
    found->second.staged.activationMode = "null";
    found->second.staged.activationTime.clear();

    return jsonResponse(200, stateAsJson(found->second.active, true));
}

ApiResponse ConnectionApi::patchStagedSender(const std::string& id, const std::string& body) {
    const auto found = senders_.find(id);
    if (found == senders_.end()) return errorResponse(404, "no sender called " + id);

    JsonValue patch;
    std::string error;
    if (!parseJson(body, patch, error)) return errorResponse(400, "the body is not JSON: " + error);

    ConnectionState staged = found->second.staged;
    if (patch.has("master_enable")) {
        const JsonValue& enable = patch["master_enable"];
        if (!enable.isBool()) return errorResponse(400, "master_enable has to be a boolean");
        staged.masterEnable = enable.asBool();
    }

    std::string activationMode;
    if (patch.has("activation") && patch["activation"].isObject()) {
        const JsonValue& mode = patch["activation"]["mode"];
        if (!mode.isNull()) {
            if (!mode.isString()) return errorResponse(400, "activation.mode has to be a string");
            activationMode = mode.asString();
            if (activationMode != "activate_immediate") {
                return errorResponse(501, "only activate_immediate is implemented, not " +
                                              activationMode);
            }
        }
    }

    // A sender's transport file is its own: it describes the stream this
    // device sends, and a controller does not get to rewrite it.
    staged.transportFile = found->second.sdp;
    found->second.staged = staged;

    if (activationMode.empty()) return jsonResponse(200, stateAsJson(found->second.staged, true));

    ConnectionState active = staged;
    active.activationMode = "activate_immediate";
    active.activationTime = nowAsTaiString();
    found->second.active = active;
    found->second.staged.activationMode = "null";
    found->second.staged.activationTime.clear();

    return jsonResponse(200, stateAsJson(found->second.active, true));
}

ApiResponse ConnectionApi::handle(const std::string& method, const std::string& path,
                                  const std::string& body) {
    const std::string root = kConnectionApiRoot;
    if (path.rfind(root, 0) != 0) {
        return errorResponse(404, "this device serves " + root);
    }

    const std::vector<std::string> segments = segmentsOf(path.substr(root.size()));

    // /x-nmos/connection/v1.1/
    if (segments.empty()) {
        if (method != "GET") return errorResponse(405, "only GET here");
        return listResponse({"bulk/", "single/"});
    }

    if (segments[0] == "bulk") {
        // Not implemented, and said so: a controller that gets a 501 uses the
        // single endpoints, while one that gets a 404 may decide the device
        // is broken.
        return errorResponse(501, "bulk is not implemented; use single/");
    }

    if (segments[0] != "single") return errorResponse(404, "no such resource");

    // /single/
    if (segments.size() == 1) {
        if (method != "GET") return errorResponse(405, "only GET here");
        return listResponse({"receivers/", "senders/"});
    }

    const bool isSender = segments[1] == "senders";
    const bool isReceiver = segments[1] == "receivers";
    if (!isSender && !isReceiver) return errorResponse(404, "no such resource");

    // /single/senders/ or /single/receivers/
    if (segments.size() == 2) {
        if (method != "GET") return errorResponse(405, "only GET here");
        std::vector<std::string> ids;
        if (isSender) {
            for (const auto& [id, sender] : senders_) ids.push_back(id + "/");
        } else {
            for (const auto& [id, receiver] : receivers_) ids.push_back(id + "/");
        }
        return listResponse(ids);
    }

    const std::string& id = segments[2];
    if (isSender && senders_.find(id) == senders_.end()) {
        return errorResponse(404, "no sender called " + id);
    }
    if (isReceiver && receivers_.find(id) == receivers_.end()) {
        return errorResponse(404, "no receiver called " + id);
    }

    // /single/senders/<id>/
    if (segments.size() == 3) {
        if (method != "GET") return errorResponse(405, "only GET here");
        if (isSender) {
            return listResponse({"active/", "constraints/", "staged/", "transportfile/",
                                 "transporttype/"});
        }
        return listResponse({"active/", "constraints/", "staged/", "transporttype/"});
    }

    const std::string& leaf = segments[3];

    if (leaf == "transporttype") {
        if (method != "GET") return errorResponse(405, "only GET here");
        return jsonResponse(200, JsonValue(std::string(kTransportRtpMulticast)));
    }

    if (leaf == "constraints") {
        if (method != "GET") return errorResponse(405, "only GET here");
        // One leg, and no constraints on it beyond that. Empty is a valid
        // answer and an honest one: this device has nothing to restrict that
        // the transport file does not already say.
        return jsonResponse(200, JsonValue(JsonArray{JsonValue(JsonObject{})}));
    }

    if (leaf == "transportfile") {
        if (!isSender) return errorResponse(404, "receivers have no transport file to serve");
        if (method != "GET") return errorResponse(405, "only GET here");

        ApiResponse response;
        response.contentType = "application/sdp";
        response.body = senders_[id].sdp;
        return response;
    }

    if (leaf == "active" || leaf == "staged") {
        const bool wantStaged = leaf == "staged";

        if (method == "GET") {
            if (isSender) {
                const ConnectionSender& sender = senders_[id];
                return jsonResponse(200, stateAsJson(wantStaged ? sender.staged : sender.active,
                                                     true));
            }
            const ConnectionReceiver& receiver = receivers_[id];
            return jsonResponse(200,
                                stateAsJson(wantStaged ? receiver.staged : receiver.active, true));
        }

        if (method == "PATCH") {
            // Active is what happened; it is changed by activating something
            // staged, not by being written to.
            if (!wantStaged) return errorResponse(405, "active is read-only; PATCH staged");
            return isSender ? patchStagedSender(id, body) : patchStagedReceiver(id, body);
        }

        return errorResponse(405, "GET or PATCH here");
    }

    return errorResponse(404, "no such resource");
}

}  // namespace AES67::Ravenna
