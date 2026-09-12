#include "Ravenna/ConnectionApi.h"

#include "Driver/SDPParser.h"

#include <algorithm>
#include <iterator>
#include <optional>
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
    std::transform(entries.begin(), entries.end(), std::back_inserter(items),
                   [](const std::string& entry) { return JsonValue(entry); });
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

/// The transport parameters of one leg, in the names IS-05 gives them for
/// urn:x-nmos:transport:rtp.mcast -- the same names `constraints` publishes --
/// read out of the transport file. An empty object here told a controller
/// nothing, and these are what it reads back to see where a connection went.
///
/// "auto" is IS-05's own word for a value the device picks. A sender's source
/// port and a receiver's interface are not in an SDP, and this device gives a
/// controller no way to choose either.
JsonObject transportParamsFromSdp(const std::string& sdp, bool forSender, bool rtpEnabled) {
    std::optional<SDPSession> session;
    if (!sdp.empty()) session = SDPParser::parseString(sdp);

    // An address the session does not carry is null, not an empty string: the
    // schema takes null for "this device has not been told", and "" for an
    // address that is not one.
    const auto address = [](const std::string& value) {
        return value.empty() ? JsonValue() : JsonValue(value);
    };

    JsonObject leg;
    leg["rtp_enabled"] = JsonValue(rtpEnabled);
    if (forSender) {
        leg["source_ip"] = session ? address(session->originAddress) : JsonValue();
        leg["source_port"] = JsonValue("auto");
        leg["destination_ip"] = session ? address(session->connectionAddress) : JsonValue();
    } else {
        // The source this receiver filters on, a=source-filter, and null when
        // the sender named none, which means any source on the group.
        leg["source_ip"] = session ? address(session->sourceAddress) : JsonValue();
        leg["interface_ip"] = JsonValue("auto");
        leg["multicast_ip"] = session ? address(session->connectionAddress) : JsonValue();
    }
    leg["destination_port"] =
        session ? JsonValue(static_cast<int>(session->port)) : JsonValue();
    return leg;
}

/// Whether this transport has a parameter of that name on that side. What
/// `constraints` publishes and what a PATCH may name are the same list, and a
/// controller naming anything else has misunderstood the device.
bool isKnownTransportParam(const std::string& name, bool forSender) {
    if (name == "rtp_enabled" || name == "source_ip" || name == "destination_port") return true;
    return forSender ? (name == "source_port" || name == "destination_ip")
                     : (name == "interface_ip" || name == "multicast_ip");
}

/// Reads a PATCHed `transport_params` and folds it into the overrides a state
/// carries. False, with `error` filled, when it is not something this
/// transport can take -- IS-05 sec 5 answers that with a 400 rather than
/// taking the request and dropping what it did not understand.
bool applyTransportParams(const JsonValue& params, bool forSender, JsonObject& overrides,
                          std::string& error) {
    if (!params.isArray()) {
        error = "transport_params has to be an array";
        return false;
    }
    // One leg, because one stream. A controller staging two has the wrong
    // device, and taking the first would connect half of what it asked for.
    if (params.asArray().size() != 1) {
        error = "this device has one leg, not " + std::to_string(params.asArray().size());
        return false;
    }
    const JsonValue& leg = params.asArray().front();
    if (!leg.isObject()) {
        error = "each leg has to be an object";
        return false;
    }

    for (const auto& [name, value] : leg.asObject()) {
        if (!isKnownTransportParam(name, forSender)) {
            error = "no transport parameter called " + name;
            return false;
        }
        // Null is a controller dropping what it had fixed, whichever name it
        // is: the value goes back to the transport file's, so there is no
        // type to check against.
        if (value.isNull()) continue;

        if (name == "rtp_enabled") {
            if (!value.isBool()) {
                error = "rtp_enabled has to be a boolean";
                return false;
            }
        } else if (name == "destination_port" || name == "source_port") {
            // A port is a number or the word "auto", which asks the device to
            // pick one.
            const bool picksItself = value.isString() && value.asString() == "auto";
            if (!picksItself && !value.isNumber()) {
                error = name + " has to be a port number or \"auto\"";
                return false;
            }
            if (value.isNumber() && (value.asNumber() < 0 || value.asNumber() > 65535)) {
                error = name + " is outside the port range";
                return false;
            }
        } else if (!value.isString()) {
            error = name + " has to be an address, or null to let the file say";
            return false;
        }
    }

    // Written only once the whole leg has been read, so a leg with one bad
    // name leaves nothing half-applied.
    for (const auto& [name, value] : leg.asObject()) {
        // Null is how a controller drops what it fixed: the parameter goes
        // back to whatever the transport file says.
        if (value.isNull()) {
            overrides.erase(name);
        } else {
            overrides[name] = value;
        }
    }
    return true;
}

}  // namespace

/// One resource's state, in the shape IS-05 defines for it.
///
/// The two shapes are not the same and the schema says so: a SENDER carries
/// `receiver_id` and no transport file -- it is the thing being described,
/// not the thing being told -- and a RECEIVER carries `sender_id` and the
/// transport file it was given. Writing a sender's own SDP into its staged
/// response failed validation for "transport_file was unexpected", and its
/// missing receiver_id failed another test outright.
JsonValue stateAsJson(const ConnectionState& state, bool forSender) {
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
    JsonObject leg = transportParamsFromSdp(state.transportFile, forSender, state.masterEnable);
    // What a controller fixed by PATCH wins over what the transport file
    // says, which is the order IS-05 sets: the file fills the parameters in
    // and the controller corrects them afterwards.
    for (const auto& [name, value] : state.transportParams) leg[name] = value;
    object["transport_params"] = JsonValue(JsonArray{JsonValue(leg)});

    if (forSender) {
        // Which receiver asked for this sender, when a controller said. Null
        // is the normal answer and a legal one.
        object["receiver_id"] =
            state.receiverId.empty() ? JsonValue() : JsonValue(state.receiverId);
    } else {
        object["sender_id"] = state.senderId.empty() ? JsonValue() : JsonValue(state.senderId);
    }

    if (!forSender) {
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

    if (patch.has("sender_id")) {
        // Null is how a controller says "take nothing", which is what a
        // crosspoint being cleared sends.
        const JsonValue& senderId = patch["sender_id"];
        if (senderId.isNull()) {
            staged.senderId.clear();
        } else if (senderId.isString()) {
            staged.senderId = senderId.asString();
        } else {
            return errorResponse(400, "sender_id has to be a string or null");
        }
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

    if (patch.has("transport_params")) {
        std::string reason;
        if (!applyTransportParams(patch["transport_params"], /*forSender=*/false,
                                  staged.transportParams, reason)) {
            return errorResponse(400, reason);
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
        return jsonResponse(200, stateAsJson(found->second.staged, /*forSender=*/false));
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

    return jsonResponse(200, stateAsJson(found->second.active, /*forSender=*/false));
}

ApiResponse ConnectionApi::patchStagedSender(const std::string& id, const std::string& body) {
    const auto found = senders_.find(id);
    if (found == senders_.end()) return errorResponse(404, "no sender called " + id);

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

    if (patch.has("receiver_id")) {
        // The far end of the subscription, which a controller sets when it
        // routes this sender somewhere. Null is how it says "nobody", the
        // same way a receiver is cleared with a null sender_id.
        const JsonValue& receiverId = patch["receiver_id"];
        if (receiverId.isNull()) {
            staged.receiverId.clear();
        } else if (receiverId.isString()) {
            staged.receiverId = receiverId.asString();
        } else {
            return errorResponse(400, "receiver_id has to be a string or null");
        }
    }

    if (patch.has("transport_params")) {
        std::string reason;
        if (!applyTransportParams(patch["transport_params"], /*forSender=*/true,
                                  staged.transportParams, reason)) {
            return errorResponse(400, reason);
        }
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

    if (activationMode.empty()) return jsonResponse(200, stateAsJson(found->second.staged, /*forSender=*/true));

    ConnectionState active = staged;
    active.activationMode = "activate_immediate";
    active.activationTime = nowAsTaiString();
    found->second.active = active;
    found->second.staged.activationMode = "null";
    found->second.staged.activationTime.clear();

    return jsonResponse(200, stateAsJson(found->second.active, /*forSender=*/true));
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
        // The resource exists and the method is what is not supported, which
        // is 405 and not 501: IS-05's bulk endpoints are defined, this device
        // serves no POST to them, and a 501 there says the whole resource is
        // unimplemented. GET is answered as the listing it is.
        if (segments.size() == 1) return listResponse({"senders/", "receivers/"});
        if (segments.size() == 2 &&
            (segments[1] == "senders" || segments[1] == "receivers")) {
            // GET is a listing of what has been staged in bulk, which here is
            // nothing: an empty array is the true answer and the one a
            // controller can read. POST is the method this device does not
            // serve, and 405 says so without claiming the resource is absent.
            if (method == "GET") return jsonResponse(200, JsonValue(JsonArray{}));
            return errorResponse(405, "bulk staging is not served here; use single/");
        }
        return errorResponse(404, "no such resource");
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
        // IS-05 SS 4.2: the constraints of a leg name every transport
        // parameter that leg has, even when the device constrains none of
        // them -- a controller reads this to know what it may stage, and an
        // empty object told it nothing existed. The values here are the
        // parameters this transport actually carries, with no bounds on them
        // beyond the ones the schema already sets.
        JsonObject leg;
        leg["destination_port"] = JsonValue(JsonObject{});
        leg["rtp_enabled"] = JsonValue(JsonObject{});
        if (isSender) {
            leg["source_ip"] = JsonValue(JsonObject{});
            leg["source_port"] = JsonValue(JsonObject{});
            leg["destination_ip"] = JsonValue(JsonObject{});
        } else {
            leg["source_ip"] = JsonValue(JsonObject{});
            leg["interface_ip"] = JsonValue(JsonObject{});
            leg["multicast_ip"] = JsonValue(JsonObject{});
        }
        return jsonResponse(200, JsonValue(JsonArray{JsonValue(leg)}));
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
                                                    /*forSender=*/true));
            }
            const ConnectionReceiver& receiver = receivers_[id];
            return jsonResponse(
                200, stateAsJson(wantStaged ? receiver.staged : receiver.active,
                                 /*forSender=*/false));
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
