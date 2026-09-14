#include "Ravenna/ChannelMappingApi.h"
#include "Ravenna/ApiReplies.h"

#include "Ravenna/TaiClock.h"

#include <algorithm>
#include <map>
#include <vector>

namespace AES67::Ravenna {
namespace {




/// A mapping's routes written out, so the block it was given and a grid a
/// controller set are read the same way.
std::vector<ChannelRoute> routesOf(const ChannelMapping& mapping) {
    if (!mapping.routes.empty()) return mapping.routes;

    std::vector<ChannelRoute> expanded;
    for (uint16_t i = 0; i < mapping.deviceChannelCount && i < mapping.streamChannelCount; ++i) {
        expanded.push_back({i, static_cast<uint16_t>(mapping.deviceChannelStart + i)});
    }
    return expanded;
}

/// Takes whatever fed this device channel off it. Every cell of a grid names
/// at most one input, and two inputs on one output channel is not a mix.
void unfeed(std::vector<ChannelRoute>& routes, uint16_t deviceChannel) {
    routes.erase(std::remove_if(routes.begin(), routes.end(),
                                [deviceChannel](const ChannelRoute& route) {
                                    return route.deviceChannel == deviceChannel;
                                }),
                 routes.end());
}

JsonValue channelLabels(uint16_t count, const char* prefix) {
    JsonArray channels;
    for (uint16_t i = 0; i < count; ++i) {
        JsonObject channel;
        channel["label"] = JsonValue(std::string(prefix) + " " + std::to_string(i + 1));
        channels.emplace_back(channel);
    }
    return JsonValue(channels);
}

}  // namespace

bool ChannelMappingApi::hasInput(const std::string& id) const {
    const std::vector<std::string> ids = connections_.receiverIds();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

JsonValue ChannelMappingApi::inputProperties(const std::string& id) const {
    const auto mapping = routing_.mappingFor(id);
    const auto receiver = connections_.receiver(id);

    // What the stream calls itself once one is connected, and what the
    // receiver calls itself before that: a controller draws a row per input
    // and needs a name for it from the moment the input exists.
    std::string name;
    if (mapping && !mapping->streamName.empty()) {
        name = mapping->streamName;
    } else if (receiver && !receiver->label.empty()) {
        name = receiver->label;
    } else {
        name = id;
    }

    return JsonValue(JsonObject{
        {"name", JsonValue(name)},
        {"description", JsonValue(mapping ? "the stream receiver " + id + " took"
                                          : "receiver " + id + ", taking nothing yet")}});
}

JsonValue ChannelMappingApi::inputChannels(const std::string& id) const {
    // The stream's channels once there is one. Before that, what this input
    // could take, which is as many as the device carries: the schema wants at
    // least one channel on every input, and an input with none would be a row
    // a controller cannot draw.
    const auto mapping = routing_.mappingFor(id);
    const uint16_t count = mapping && mapping->streamChannelCount > 0
                               ? mapping->streamChannelCount
                               : static_cast<uint16_t>(mapper_.getUsableChannelCount());
    return channelLabels(count, "Channel");
}

JsonValue ChannelMappingApi::inputParent(const std::string& id) const {
    // IS-08 sec 4: which resource this input belongs to. It is the receiver,
    // and its id is the connection API's. Null on both when nothing is
    // connected, which is what the schema takes for "not from anywhere yet".
    const auto mapping = routing_.mappingFor(id);
    if (!mapping) {
        return JsonValue(JsonObject{{"type", JsonValue()}, {"id", JsonValue()}});
    }
    return JsonValue(JsonObject{{"type", JsonValue("receiver")}, {"id", JsonValue(id)}});
}

JsonValue ChannelMappingApi::inputCaps() const {
    // One channel at a time, and in any order: that is what the matrix does,
    // and claiming a block size it does not have would make a controller draw
    // a grid this device cannot honour.
    return JsonValue(JsonObject{{"block_size", JsonValue(1)}, {"reordering", JsonValue(true)}});
}

JsonValue ChannelMappingApi::outputProperties() const {
    return JsonValue(JsonObject{{"name", JsonValue("Device channels")},
                                {"description", JsonValue("the channels this device carries")}});
}

JsonValue ChannelMappingApi::outputChannels() const {
    return channelLabels(static_cast<uint16_t>(mapper_.getUsableChannelCount()), "Device");
}

JsonValue ChannelMappingApi::outputCaps() const {
    // Null rather than a list: any input can feed any of these, which is what
    // the matrix allows, and null is the schema's own word for "no such
    // restriction". It belongs here and not beside the channels -- an output
    // with routable_inputs at its top level is not an output the schema
    // recognises at all.
    return JsonValue(JsonObject{{"routable_inputs", JsonValue()}});
}

JsonValue ChannelMappingApi::outputSourceId() const {
    // The source this output feeds, when it feeds one. This device's channels
    // are not a source of their own, so null.
    return JsonValue();
}

ApiResponse ChannelMappingApi::describeIo() const {
    JsonObject inputs;
    for (const std::string& receiverId : connections_.receiverIds()) {
        inputs[receiverId] = JsonValue(JsonObject{{"properties", inputProperties(receiverId)},
                                                  {"parent", inputParent(receiverId)},
                                                  {"channels", inputChannels(receiverId)},
                                                  {"caps", inputCaps()}});
    }

    JsonObject outputs;
    outputs[kDeviceOutputId] = JsonValue(JsonObject{{"properties", outputProperties()},
                                                    {"source_id", outputSourceId()},
                                                    {"channels", outputChannels()},
                                                    {"caps", outputCaps()}});

    JsonObject io;
    io["inputs"] = JsonValue(inputs);
    io["outputs"] = JsonValue(outputs);
    return jsonResponse(200, JsonValue(io));
}

ApiResponse ChannelMappingApi::activeMap() const {
    // Which input and which of its channels feeds each device channel. Built
    // by walking what the matrix holds rather than by keeping a second copy:
    // a grid that disagrees with the routing is worse than no grid.
    std::map<int, std::pair<std::string, int>> feeding;

    for (const std::string& receiverId : connections_.receiverIds()) {
        const auto mapping = routing_.mappingFor(receiverId);
        // The stream's grid where there is one, and the one a controller set
        // beforehand where there is not: the active map is what this device
        // will do with a channel, not only what it is doing this instant.
        const std::vector<ChannelRoute> routes =
            mapping ? routesOf(*mapping) : routing_.rememberedRoutes(receiverId);
        for (const ChannelRoute& route : routes) {
            feeding[route.deviceChannel] = {receiverId, route.streamChannel};
        }
    }

    JsonObject outputChannels;
    for (size_t channel = 0; channel < mapper_.getUsableChannelCount(); ++channel) {
        JsonObject cell;
        const auto found = feeding.find(static_cast<int>(channel));
        if (found == feeding.end()) {
            cell["input"] = JsonValue();
            cell["channel_index"] = JsonValue();
        } else {
            cell["input"] = JsonValue(found->second.first);
            cell["channel_index"] = JsonValue(found->second.second);
        }
        outputChannels[std::to_string(channel)] = JsonValue(cell);
    }

    JsonObject action;
    action[kDeviceOutputId] = JsonValue(outputChannels);

    JsonObject activation;
    activation["mode"] = lastActivationTime_.empty() ? JsonValue()
                                                     : JsonValue("activate_immediate");
    activation["requested_time"] = JsonValue();
    activation["activation_time"] =
        lastActivationTime_.empty() ? JsonValue() : JsonValue(lastActivationTime_);

    // "map", not "action": IS-08's map-active response holds the current map
    // under that name, and "action" is what a POST to map/activate SENDS.
    // Writing the request's name into the response left a controller reading
    // an empty grid on a device whose channels were all connected.
    JsonObject body;
    body["map"] = JsonValue(action);
    body["activation"] = JsonValue(activation);
    return jsonResponse(200, JsonValue(body));
}

ApiResponse ChannelMappingApi::applyAction(const JsonValue& action, bool commit) {
    if (!action.isObject()) return errorResponse(400, "action has to be an object");
    for (const auto& [outputId, cells] : action.asObject()) {
        if (outputId != kDeviceOutputId) {
            return errorResponse(404, "this device has one output block, called " +
                                          std::string(kDeviceOutputId));
        }
        if (!cells.isObject()) return errorResponse(400, "an output's action has to be an object");
    }

    // The whole change is worked out before any of it is applied: half a grid
    // is a device carrying channels nobody asked for on the ones that took.
    //
    // Every input, not only the ones carrying something: IS-08 routes ports,
    // and a plant is patched before its streams arrive. What is set on an
    // input with no stream is remembered until one comes.
    std::map<std::string, ChannelMapping> updated;
    std::map<std::string, std::vector<ChannelRoute>> waiting;
    for (const std::string& receiverId : connections_.receiverIds()) {
        const auto mapping = routing_.mappingFor(receiverId);
        if (mapping) {
            ChannelMapping working = *mapping;
            working.routes = routesOf(working);
            updated[receiverId] = working;
            continue;
        }
        waiting[receiverId] = routing_.rememberedRoutes(receiverId);
    }

    const JsonValue& cells = action[kDeviceOutputId];
    for (const auto& [channelText, cell] : cells.asObject()) {
        int deviceChannel = 0;
        try {
            deviceChannel = std::stoi(channelText);
        } catch (...) {
            return errorResponse(400, "output channel \"" + channelText + "\" is not a number");
        }
        if (deviceChannel < 0 ||
            deviceChannel >= static_cast<int>(mapper_.getUsableChannelCount())) {
            return errorResponse(400, "output channel " + channelText + " is not on this device");
        }
        if (!cell.isObject()) return errorResponse(400, "a cell has to be an object");

        // Whatever fed this device channel stops feeding it, whether the cell
        // names a new input or empties it. Two inputs on one output channel
        // is not a mix, it is a fault.
        for (auto& [receiverId, mapping] : updated) {
            unfeed(mapping.routes, static_cast<uint16_t>(deviceChannel));
        }
        for (auto& [receiverId, routes] : waiting) {
            unfeed(routes, static_cast<uint16_t>(deviceChannel));
        }

        const JsonValue& input = cell["input"];
        if (input.isNull()) continue;  // the cell was emptied
        if (!input.isString()) return errorResponse(400, "a cell's input has to be a string");

        const auto connected = updated.find(input.asString());
        const auto pending = waiting.find(input.asString());
        if (connected == updated.end() && pending == waiting.end()) {
            return errorResponse(404, "no input called " + input.asString() +
                                          "; io/ lists the ones there are");
        }

        const JsonValue& index = cell["channel_index"];
        if (!index.isNumber()) {
            return errorResponse(400, "a cell that names an input needs a channel_index");
        }
        const int streamChannel = static_cast<int>(index.asNumber());
        // What that input has to offer: the stream's channels once one is
        // connected, and what the input could take before that, which is what
        // io reports for it.
        const int channels = connected != updated.end()
                                 ? static_cast<int>(connected->second.streamChannelCount)
                                 : static_cast<int>(mapper_.getUsableChannelCount());
        if (streamChannel < 0 || streamChannel >= channels) {
            return errorResponse(400, "input " + input.asString() + " has no channel " +
                                          std::to_string(streamChannel));
        }

        std::vector<ChannelRoute>& routes =
            connected != updated.end() ? connected->second.routes : pending->second;
        routes.push_back({static_cast<uint16_t>(streamChannel),
                          static_cast<uint16_t>(deviceChannel)});
    }

    // Applied by taking every mapping out and putting the new ones back: an
    // update at a time would be refused for overlapping a state that is on
    // its way out.
    std::vector<ChannelMapping> previous = mapper_.getAllMappings();
    mapper_.clearAll();

    for (const auto& [receiverId, mapping] : updated) {
        std::string why;
        if (!mapper_.validateMapping(mapping, &why) || !mapper_.addMapping(mapping)) {
            // Put back what was there. A refused activation has to leave the
            // device carrying what it was carrying.
            mapper_.clearAll();
            for (const ChannelMapping& old : previous) mapper_.addMapping(old);
            return errorResponse(400, "the matrix refused the grid: " +
                                          (why.empty() ? "it overlaps itself" : why));
        }
    }

    if (!commit) {
        // The grid takes; this call only asked whether it would. Restored
        // exactly like the refusal path just above -- the device has to be
        // left carrying what it was carrying either way.
        mapper_.clearAll();
        for (const ChannelMapping& old : previous) mapper_.addMapping(old);
        return activeMap();
    }

    // And what was set on an input with nothing flowing is kept for the
    // stream that has not arrived yet.
    for (const auto& [receiverId, routes] : waiting) routing_.rememberRoutes(receiverId, routes);

    lastActivationTime_ = taiText(taiNow());
    return activeMap();
}

JsonValue ChannelMappingApi::activationEnvelope(const PendingActivation& activation) const {
    JsonObject reported;
    reported["mode"] = JsonValue(activation.mode);
    reported["requested_time"] = activation.requestedTime.empty()
                                     ? JsonValue()
                                     : JsonValue(activation.requestedTime);
    reported["activation_time"] = activation.activationTime.empty()
                                      ? JsonValue()
                                      : JsonValue(activation.activationTime);

    JsonObject one;
    one["activation"] = JsonValue(reported);
    one["action"] = activation.action;
    return JsonValue(one);
}

void ChannelMappingApi::applyDueActivations() {
    const TaiTime now = taiNow();

    for (auto activation = pending_.begin(); activation != pending_.end();) {
        if (!taiReached(activation->due, now)) {
            ++activation;
            continue;
        }
        // postActivation() already dry-ran this against the state at the
        // time it was scheduled, so a refusal here means something changed
        // in the meantime -- and there is still nobody left to tell: the
        // 202 already went out. The grid stays as it was either way.
        (void)applyAction(activation->action);
        activation = pending_.erase(activation);
    }
}

ApiResponse ChannelMappingApi::postActivation(const std::string& body) {
    JsonValue request;
    std::string parseError;
    if (!parseJson(body, request, parseError)) {
        return errorResponse(400, "the body is not JSON: " + parseError);
    }
    if (!request.isObject()) return errorResponse(400, "the body has to be an object");

    const JsonValue& activation = request["activation"];
    if (!activation.isObject()) return errorResponse(400, "activation has to be an object");

    const JsonValue& mode = activation["mode"];
    if (!mode.isString()) return errorResponse(400, "activation.mode has to be a string");
    const std::string wanted = mode.asString();
    const bool immediate = wanted == kActivateImmediate;
    const bool relative = wanted == kActivateRelative;
    const bool absolute = wanted == kActivateAbsolute;
    if (!immediate && !relative && !absolute) {
        return errorResponse(400, "no activation mode called " + wanted);
    }

    // IS-08 sec 5: a grid with a change already promised is locked until that
    // change happens or is deleted. Taking a second one would leave two
    // answers about where the same channel is going.
    if (!pending_.empty()) {
        return errorResponse(423, "an activation is already waiting; delete it first");
    }

    PendingActivation queued;
    queued.id = std::to_string(nextActivationId_++);
    queued.mode = wanted;
    queued.action = request["action"];

    if (immediate) {
        const ApiResponse applied = applyAction(queued.action);
        if (applied.status < 200 || applied.status > 299) return applied;
        queued.activationTime = lastActivationTime_;
        return jsonResponse(200,
                            JsonValue(JsonObject{{queued.id, activationEnvelope(queued)}}));
    }

    const JsonValue& requested = activation["requested_time"];
    if (!requested.isString()) {
        return errorResponse(400, wanted + " needs a requested_time of \"<seconds>:<nanoseconds>\"");
    }
    TaiTime asked;
    if (!parseTai(requested.asString(), asked)) {
        return errorResponse(400, "requested_time is not \"<seconds>:<nanoseconds>\": " +
                                      requested.asString());
    }
    queued.requestedTime = requested.asString();
    // Relative is an offset from now, absolute is the instant itself.
    queued.due = relative ? taiSum(taiNow(), asked) : asked;
    queued.activationTime = taiText(queued.due);

    // Read before it is promised, so a grid that could never be applied is
    // refused now rather than dropped silently when its time comes.
    // applyAction(action, /*commit=*/false) runs every check the real
    // activation will -- output id, cell shape, channel bounds, input
    // existence -- and restores the matrix regardless of the answer, so
    // this can ask "would this be refused" without a controller having to
    // wait for the scheduled time to find out applyDueActivations()
    // silently threw the answer away.
    ApiResponse dryRun = applyAction(queued.action, /*commit=*/false);
    if (dryRun.status < 200 || dryRun.status > 299) return dryRun;

    const JsonValue envelope = activationEnvelope(queued);
    pending_.push_back(queued);
    return jsonResponse(202, JsonValue(JsonObject{{queued.id, envelope}}));
}

ApiResponse ChannelMappingApi::handle(const std::string& method, const std::string& path,
                                      const std::string& body) {
    // Whatever was promised and has come due happens here, before anything is
    // read or written: this API has no thread of its own, and nothing learns
    // what the grid is without asking.
    applyDueActivations();

    const std::string root = kChannelMappingApiRoot;
    if (path.rfind(root, 0) != 0) return errorResponse(404, "this device serves " + root);

    const std::vector<std::string> segments = segmentsOf(path.substr(root.size()));

    if (segments.empty()) {
        if (method != "GET") return errorResponse(405, "only GET here");
        return jsonResponse(200, JsonValue(JsonArray{JsonValue("inputs/"), JsonValue("io/"),
                                                     JsonValue("map/"), JsonValue("outputs/")}));
    }

    if (segments[0] == "io") {
        if (method != "GET") return errorResponse(405, "only GET here");
        return describeIo();
    }

    // The inputs and the outputs, one resource at a time. io says the same
    // thing in one answer, and a controller is entitled to either: the whole
    // picture for drawing a grid, or one field for a name it is about to show.
    if (segments[0] == "inputs" || segments[0] == "outputs") {
        const bool forInputs = segments[0] == "inputs";
        if (method != "GET") return errorResponse(405, "only GET here");

        if (segments.size() == 1) {
            std::vector<std::string> ids;
            if (forInputs) {
                for (const std::string& id : connections_.receiverIds()) ids.push_back(id + "/");
            } else {
                ids.emplace_back(std::string(kDeviceOutputId) + "/");
            }
            JsonArray listing;
            listing.reserve(ids.size());
            for (const std::string& id : ids) listing.emplace_back(id);
            return jsonResponse(200, JsonValue(listing));
        }

        const std::string& id = segments[1];
        if (forInputs ? !hasInput(id) : id != kDeviceOutputId) {
            return errorResponse(404, "no " + segments[0].substr(0, segments[0].size() - 1) +
                                          " called " + id);
        }

        if (segments.size() == 2) {
            return jsonResponse(
                200, JsonValue(JsonArray{JsonValue("caps/"), JsonValue("channels/"),
                                         JsonValue(forInputs ? "parent/" : "sourceid/"),
                                         JsonValue("properties/")}));
        }

        const std::string& leaf = segments[2];
        if (leaf == "properties") {
            return jsonResponse(200, forInputs ? inputProperties(id) : outputProperties());
        }
        if (leaf == "channels") {
            return jsonResponse(200, forInputs ? inputChannels(id) : outputChannels());
        }
        if (leaf == "caps") return jsonResponse(200, forInputs ? inputCaps() : outputCaps());
        if (forInputs && leaf == "parent") return jsonResponse(200, inputParent(id));
        if (!forInputs && leaf == "sourceid") return jsonResponse(200, outputSourceId());
        return errorResponse(404, "no such resource");
    }

    if (segments[0] != "map") return errorResponse(404, "no such resource");

    if (segments.size() == 1) {
        if (method != "GET") return errorResponse(405, "only GET here");
        return jsonResponse(200, JsonValue(JsonArray{JsonValue("activations/"),
                                                     JsonValue("active/")}));
    }

    if (segments[1] == "active") {
        if (method != "GET") return errorResponse(405, "only GET here");
        return activeMap();
    }

    if (segments[1] == "activations") {
        if (segments.size() == 2) {
            // The whole list, keyed by the id each was given. An object, not
            // an array: a controller reading an array here cannot index it.
            if (method == "POST") return postActivation(body);
            if (method != "GET") return errorResponse(405, "GET or POST here");

            JsonObject waiting;
            for (const PendingActivation& activation : pending_) {
                waiting[activation.id] = activationEnvelope(activation);
            }
            return jsonResponse(200, JsonValue(waiting));
        }

        const std::string& id = segments[2];
        const auto found = std::find_if(pending_.begin(), pending_.end(),
                                        [&id](const PendingActivation& activation) {
                                            return activation.id == id;
                                        });
        if (found == pending_.end()) return errorResponse(404, "no activation called " + id);

        if (method == "GET") return jsonResponse(200, activationEnvelope(*found));
        if (method == "DELETE") {
            // Cancelled: the grid stays as it is and the lock is lifted.
            pending_.erase(found);
            ApiResponse gone;
            gone.status = 204;
            gone.contentType.clear();
            return gone;
        }
        return errorResponse(405, "GET or DELETE here");
    }

    return errorResponse(404, "no such resource");
}

}  // namespace AES67::Ravenna
