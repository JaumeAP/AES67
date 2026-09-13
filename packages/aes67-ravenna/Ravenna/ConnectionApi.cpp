#include "Ravenna/ConnectionApi.h"
#include "Ravenna/ApiReplies.h"

#include "Driver/SDPParser.h"
#include "Ravenna/TaiClock.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <iterator>
#include <optional>
#include <ctime>

namespace AES67::Ravenna {
namespace {





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
    // Not null: the schema takes a port number or the word "auto" and
    // nothing else, so a device with no transport file yet says it has not
    // chosen rather than saying nothing.
    leg["destination_port"] =
        session ? JsonValue(static_cast<int>(session->port)) : JsonValue("auto");
    return leg;
}

/// The names a staged PATCH may carry at its top level. IS-05 sec 5 refuses a
/// request it does not understand rather than taking it and applying the part
/// it recognised, which would leave a controller believing it set something
/// this device never read.
bool isKnownPatchKey(const std::string& name, bool forSender) {
    if (name == "master_enable" || name == "activation" || name == "transport_params") return true;
    return forSender ? name == "receiver_id" : (name == "sender_id" || name == "transport_file");
}

/// Replaces every "auto" in a leg with the value this device actually uses.
/// IS-05 sec 4: "auto" is a request, and what is active has to say what was
/// chosen -- a controller reading "auto" back off /active learns nothing
/// about where the stream went.
void resolveAutoLeg(JsonObject& leg, bool forSender, const std::string& sdp,
                    const std::string& interfaceAddress) {
    std::optional<SDPSession> session;
    if (!sdp.empty()) session = SDPParser::parseString(sdp);
    const int mediaPort = session ? static_cast<int>(session->port) : 5004;
    // The interface is unset only when nobody told this API which one the host
    // receives on, and 0.0.0.0 is the honest answer for "any of them".
    const std::string ownAddress =
        interfaceAddress.empty() ? std::string("0.0.0.0") : interfaceAddress;

    const auto isAuto = [&leg](const std::string& name) {
        const auto found = leg.find(name);
        return found != leg.end() && found->second.isString() &&
               found->second.asString() == "auto";
    };
    const auto chose = [&leg](const std::string& name, const std::string& value) {
        leg[name] = value.empty() ? JsonValue() : JsonValue(value);
    };

    if (isAuto("destination_port")) leg["destination_port"] = JsonValue(mediaPort);
    if (forSender) {
        // This device sends from the same port it sends to, which is what the
        // SDP describes and what the wire carries.
        if (isAuto("source_port")) leg["source_port"] = JsonValue(mediaPort);
        if (isAuto("source_ip")) chose("source_ip", session ? session->originAddress : ownAddress);
        if (isAuto("destination_ip") && session) chose("destination_ip", session->connectionAddress);
    } else {
        if (isAuto("interface_ip")) leg["interface_ip"] = JsonValue(ownAddress);
        if (isAuto("multicast_ip")) chose("multicast_ip", session ? session->connectionAddress : "");
        // A receiver asked to pick its own source filter takes any source on
        // the group, which is what null means here.
        if (isAuto("source_ip")) chose("source_ip", session ? session->sourceAddress : "");
    }
}

/// Rewrites a sender's transport file so it describes where the stream is
/// actually going. A controller is entitled to move a sender's destination
/// and then read the SDP; one that still named the old group would send every
/// receiver somewhere nothing arrives.
void followSdpToLeg(std::string& sdp, const JsonObject& leg) {
    if (sdp.empty()) return;
    std::optional<SDPSession> session = SDPParser::parseString(sdp);
    if (!session) return;

    bool changed = false;
    const auto destination = leg.find("destination_ip");
    if (destination != leg.end() && destination->second.isString() &&
        destination->second.asString() != "auto" &&
        destination->second.asString() != session->connectionAddress) {
        session->connectionAddress = destination->second.asString();
        changed = true;
    }
    const auto port = leg.find("destination_port");
    if (port != leg.end() && port->second.isNumber() &&
        static_cast<uint16_t>(port->second.asNumber()) != session->port) {
        session->port = static_cast<uint16_t>(port->second.asNumber());
        changed = true;
    }
    if (!changed) return;

    // The version moves with the content, which is how a receiver holding the
    // old file knows this one replaced it (RFC 4566 sec 5.2).
    ++session->sessionVersion;
    sdp = SDPParser::generate(*session);
}

/// What an activation request asked for, once it has been read.
struct ActivationRequest {
    std::string mode;           ///< empty when the PATCH stages without activating
    std::string requestedTime;  ///< only a scheduled activation carries one
    TaiTime due;                ///< when a scheduled one happens
};

/// Reads the activation object of a staged PATCH. False, with `error` filled,
/// when it is not an activation this device can carry out.
bool readActivation(const JsonValue& patch, ActivationRequest& request, std::string& error) {
    if (!patch.has("activation")) return true;

    const JsonValue& activation = patch["activation"];
    if (!activation.isObject()) {
        error = "activation has to be an object";
        return false;
    }

    const JsonValue& mode = activation["mode"];
    if (mode.isNull()) return true;
    if (!mode.isString()) {
        error = "activation.mode has to be a string";
        return false;
    }
    request.mode = mode.asString();
    // An immediate activation carries no requested time, and the one it
    // reports back is null.
    if (request.mode == kActivateImmediate) return true;

    const bool relative = request.mode == kActivateRelative;
    if (!relative && request.mode != kActivateAbsolute) {
        error = "no activation mode called " + request.mode;
        return false;
    }

    const JsonValue& requested = activation["requested_time"];
    if (!requested.isString()) {
        error = request.mode + " needs a requested_time of \"<seconds>:<nanoseconds>\"";
        return false;
    }
    TaiTime asked;
    if (!parseTai(requested.asString(), asked)) {
        error = "requested_time is not \"<seconds>:<nanoseconds>\": " + requested.asString();
        return false;
    }
    request.requestedTime = requested.asString();
    // Relative is an offset from now, absolute is the instant itself.
    request.due = relative ? taiSum(taiNow(), asked) : asked;
    return true;
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
    activation["requested_time"] = state.activationRequestedTime.empty()
                                       ? JsonValue()
                                       : JsonValue(state.activationRequestedTime);
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

JsonValue ConnectionApi::activeAsJson(const ConnectionState& state, bool forSender) const {
    ConnectionState resolved = state;
    JsonObject leg = transportParamsFromSdp(state.transportFile, forSender, state.masterEnable);
    for (const auto& [name, value] : state.transportParams) leg[name] = value;
    resolveAutoLeg(leg, forSender, state.transportFile, interfaceAddress_);
    resolved.transportParams = leg;
    return stateAsJson(resolved, forSender);
}

void ConnectionApi::activateSender(ConnectionSender& sender, ConnectionState state) {
    // What is active has to say what this device chose, so the leg is worked
    // out once here -- the file's values, the controller's on top, every
    // "auto" resolved -- and stored whole.
    JsonObject leg = transportParamsFromSdp(sender.sdp, /*forSender=*/true, state.masterEnable);
    for (const auto& [name, value] : state.transportParams) leg[name] = value;
    resolveAutoLeg(leg, /*forSender=*/true, sender.sdp, interfaceAddress_);
    state.transportParams = leg;

    // A sender's transport file describes where it sends, so it follows.
    followSdpToLeg(sender.sdp, leg);
    state.transportFile = sender.sdp;

    sender.active = state;
    sender.staged.transportFile = sender.sdp;
    sender.staged.activationMode = "null";
    sender.staged.activationRequestedTime.clear();
    sender.staged.activationTime.clear();
}

bool ConnectionApi::activateReceiver(ConnectionReceiver& receiver, ConnectionState state,
                                     std::string& error) {
    JsonObject leg =
        transportParamsFromSdp(state.transportFile, /*forSender=*/false, state.masterEnable);
    for (const auto& [name, value] : state.transportParams) leg[name] = value;
    resolveAutoLeg(leg, /*forSender=*/false, state.transportFile, interfaceAddress_);
    state.transportParams = leg;

    if (onActivation_ && !onActivation_(receiver.id, state.transportFile, state.masterEnable,
                                        error)) {
        return false;
    }

    receiver.active = state;
    receiver.staged.activationMode = "null";
    receiver.staged.activationRequestedTime.clear();
    receiver.staged.activationTime.clear();
    return true;
}

void ConnectionApi::applyDueActivations() {
    const TaiTime now = taiNow();

    for (auto& [id, sender] : senders_) {
        if (!sender.pending.waiting) continue;
        if (!taiReached({sender.pending.dueSeconds, sender.pending.dueNanos}, now)) continue;
        const ConnectionState promised = sender.pending.state;
        sender.pending = PendingActivation{};
        activateSender(sender, promised);
    }

    for (auto& [id, receiver] : receivers_) {
        if (!receiver.pending.waiting) continue;
        if (!taiReached({receiver.pending.dueSeconds, receiver.pending.dueNanos}, now)) continue;
        const ConnectionState promised = receiver.pending.state;
        receiver.pending = PendingActivation{};
        // A scheduled activation the host refuses has nobody left to tell:
        // the answer went out with the 202. What was active stays active.
        std::string refused;
        activateReceiver(receiver, promised, refused);
    }
}

ApiResponse ConnectionApi::patchStagedReceiver(const std::string& id, const std::string& body) {
    const auto found = receivers_.find(id);
    if (found == receivers_.end()) return errorResponse(404, "no receiver called " + id);

    JsonValue patch;
    std::string error;
    if (!parseJson(body, patch, error)) return errorResponse(400, "the body is not JSON: " + error);
    if (!patch.isObject()) return errorResponse(400, "the body has to be an object");
    for (const auto& [name, value] : patch.asObject()) {
        if (!isKnownPatchKey(name, /*forSender=*/false)) {
            return errorResponse(400, "a receiver has nothing called " + name);
        }
    }

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

    ActivationRequest activation;
    if (!readActivation(patch, activation, error)) return errorResponse(400, error);

    found->second.staged = staged;

    if (activation.mode.empty()) {
        // Staged and not activated, which is the normal first half of the
        // exchange: a controller stages, checks, then activates.
        return jsonResponse(200, stateAsJson(found->second.staged, /*forSender=*/false));
    }

    if (staged.masterEnable && staged.transportFile.empty()) {
        return errorResponse(400, "cannot enable a receiver with no transport file");
    }

    if (activation.mode != kActivateImmediate) {
        // Promised, not done: IS-05 sec 4 answers 202, and the staged state
        // carries the activation until its time comes.
        ConnectionState promised = staged;
        promised.activationMode = activation.mode;
        promised.activationRequestedTime = activation.requestedTime;
        promised.activationTime = taiText(activation.due);
        found->second.staged = promised;
        found->second.pending = {true, activation.due.seconds, activation.due.nanos, promised};
        return jsonResponse(202, stateAsJson(promised, /*forSender=*/false));
    }

    ConnectionState immediate = staged;
    immediate.activationMode = kActivateImmediate;
    immediate.activationRequestedTime.clear();
    immediate.activationTime = taiText(taiNow());
    if (!activateReceiver(found->second, immediate, error)) {
        return errorResponse(400, "the receiver refused it: " + error);
    }

    return jsonResponse(200, activeAsJson(found->second.active, /*forSender=*/false));
}

ApiResponse ConnectionApi::patchStagedSender(const std::string& id, const std::string& body) {
    const auto found = senders_.find(id);
    if (found == senders_.end()) return errorResponse(404, "no sender called " + id);

    JsonValue patch;
    std::string error;
    if (!parseJson(body, patch, error)) return errorResponse(400, "the body is not JSON: " + error);
    if (!patch.isObject()) return errorResponse(400, "the body has to be an object");
    for (const auto& [name, value] : patch.asObject()) {
        if (!isKnownPatchKey(name, /*forSender=*/true)) {
            return errorResponse(400, "a sender has nothing called " + name);
        }
    }

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

    ActivationRequest activation;
    if (!readActivation(patch, activation, error)) return errorResponse(400, error);

    // A sender's transport file is its own: it describes the stream this
    // device sends, and a controller does not get to rewrite it.
    staged.transportFile = found->second.sdp;
    found->second.staged = staged;

    if (activation.mode.empty()) {
        return jsonResponse(200, stateAsJson(found->second.staged, /*forSender=*/true));
    }

    if (activation.mode != kActivateImmediate) {
        ConnectionState promised = staged;
        promised.activationMode = activation.mode;
        promised.activationRequestedTime = activation.requestedTime;
        promised.activationTime = taiText(activation.due);
        found->second.staged = promised;
        found->second.pending = {true, activation.due.seconds, activation.due.nanos, promised};
        return jsonResponse(202, stateAsJson(promised, /*forSender=*/true));
    }

    ConnectionState immediate = staged;
    immediate.activationMode = kActivateImmediate;
    immediate.activationRequestedTime.clear();
    immediate.activationTime = taiText(taiNow());
    activateSender(found->second, immediate);

    return jsonResponse(200, activeAsJson(found->second.active, /*forSender=*/true));
}

ApiResponse ConnectionApi::patchInBulk(bool forSenders, const std::string& body) {
    JsonValue request;
    std::string error;
    if (!parseJson(body, request, error)) return errorResponse(400, "the body is not JSON: " + error);
    // IS-05 sec 4: a bulk request is an array of the same patches the single
    // endpoints take, each with the id it goes to.
    if (!request.isArray()) return errorResponse(400, "a bulk request is an array");

    JsonArray answers;
    answers.reserve(request.asArray().size());
    for (const JsonValue& entry : request.asArray()) {
        if (!entry.isObject() || !entry["id"].isString()) {
            return errorResponse(400, "every entry in a bulk request needs an id and its params");
        }
        const std::string id = entry["id"].asString();
        const std::string params = entry["params"].serialise();
        const ApiResponse one = forSenders ? patchStagedSender(id, params)
                                           : patchStagedReceiver(id, params);

        // The answer is per resource: what the single endpoint would have
        // said, so a controller can see which of them refused and why.
        JsonObject answer;
        answer["id"] = JsonValue(id);
        answer["code"] = JsonValue(one.status);
        answers.emplace_back(answer);
    }
    return jsonResponse(200, JsonValue(answers));
}

ApiResponse ConnectionApi::handle(const std::string& method, const std::string& path,
                                  const std::string& body) {
    // Every scheduled activation whose time has come happens here, before
    // anything is read or written. This API has no thread of its own, and a
    // controller only learns what is active by asking, so the request that
    // asks is the moment to catch up.
    applyDueActivations();

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
        if (segments.size() == 1) return listResponse({"senders/", "receivers/"});
        if (segments.size() == 2 &&
            (segments[1] == "senders" || segments[1] == "receivers")) {
            // IS-05 sec 4: /bulk/senders and /bulk/receivers take a POST and
            // nothing else. There is no representation to GET -- a bulk
            // request is a batch of patches, not a resource -- so GET is a
            // 405 on a resource that is plainly there.
            if (method != "POST") return errorResponse(405, "only POST here");
            return patchInBulk(segments[1] == "senders", body);
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
        // The base URN, with the subclassification taken off: this endpoint's
        // schema is an enum of the four bases and rtp.mcast is not one of
        // them. IS-04's own `transport` field is where the multicast half is
        // published.
        return jsonResponse(200, JsonValue(std::string(kTransportRtp)));
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
                return jsonResponse(200, wantStaged ? stateAsJson(sender.staged, true)
                                                    : activeAsJson(sender.active, true));
            }
            const ConnectionReceiver& receiver = receivers_[id];
            return jsonResponse(200, wantStaged ? stateAsJson(receiver.staged, false)
                                                : activeAsJson(receiver.active, false));
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
