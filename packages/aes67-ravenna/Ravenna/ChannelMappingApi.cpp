#include "Ravenna/ChannelMappingApi.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <vector>

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
    error["error"] = JsonValue(status == 404   ? "Not Found"
                               : status == 501 ? "Not Implemented"
                               : status == 405 ? "Method Not Allowed"
                                               : "Bad Request");
    error["debug"] = JsonValue(detail);
    return jsonResponse(status, JsonValue(error));
}

std::string nowAsTaiString() {
    struct timespec now {};
    ::clock_gettime(CLOCK_REALTIME, &now);
    return std::to_string(static_cast<long long>(now.tv_sec)) + ":" +
           std::to_string(static_cast<long long>(now.tv_nsec));
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

/// Every channel of a mapping as an explicit device channel, so the sequential
/// case and the custom one are read the same way. -1 is a stream channel that
/// goes nowhere, which is what an unrouted cell in the grid is.
std::vector<int> explicitMapOf(const ChannelMapping& mapping) {
    if (!mapping.channelMap.empty()) return mapping.channelMap;

    std::vector<int> expanded(mapping.streamChannelCount, -1);
    for (uint16_t i = 0; i < mapping.deviceChannelCount && i < mapping.streamChannelCount; ++i) {
        expanded[i] = mapping.deviceChannelStart + i;
    }
    return expanded;
}

JsonValue channelLabels(uint16_t count, const char* prefix) {
    JsonArray channels;
    for (uint16_t i = 0; i < count; ++i) {
        JsonObject channel;
        channel["label"] = JsonValue(std::string(prefix) + " " + std::to_string(i + 1));
        channels.push_back(JsonValue(channel));
    }
    return JsonValue(channels);
}

}  // namespace

ApiResponse ChannelMappingApi::describeIo() const {
    JsonObject inputs;
    for (const std::string& receiverId : routing_.connectedReceivers()) {
        const auto mapping = routing_.mappingFor(receiverId);
        if (!mapping) continue;

        JsonObject input;
        input["name"] = JsonValue(mapping->streamName.empty() ? receiverId : mapping->streamName);
        input["description"] = JsonValue("the stream receiver " + receiverId + " took");
        input["channels"] = channelLabels(mapping->streamChannelCount, "Channel");
        // IS-08 sec 4: which resource this input belongs to. It is the
        // receiver, and its id is the connection API's.
        input["parent"] = JsonValue(JsonObject{{"type", JsonValue("receiver")},
                                               {"id", JsonValue(receiverId)}});
        // One channel at a time, and in any order: that is what the matrix
        // does, and claiming a block size it does not have would make a
        // controller draw a grid this device cannot honour.
        input["block_size"] = JsonValue(1);
        input["reordering"] = JsonValue(true);
        inputs[receiverId] = JsonValue(input);
    }

    JsonObject output;
    output["name"] = JsonValue("Device channels");
    output["description"] = JsonValue("the channels this device carries");
    output["channels"] = channelLabels(static_cast<uint16_t>(mapper_.getUsableChannelCount()),
                                       "Device");
    output["source_id"] = JsonValue();
    // Null rather than a list: any input can feed any of these, which is what
    // the matrix allows.
    output["routable_inputs"] = JsonValue();

    JsonObject outputs;
    outputs[kDeviceOutputId] = JsonValue(output);

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

    for (const std::string& receiverId : routing_.connectedReceivers()) {
        const auto mapping = routing_.mappingFor(receiverId);
        if (!mapping) continue;

        const std::vector<int> channels = explicitMapOf(*mapping);
        for (size_t streamChannel = 0; streamChannel < channels.size(); ++streamChannel) {
            const int deviceChannel = channels[streamChannel];
            if (deviceChannel < 0) continue;
            feeding[deviceChannel] = {receiverId, static_cast<int>(streamChannel)};
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

    JsonObject map;
    map["action"] = JsonValue(action);
    map["activation"] = JsonValue(activation);
    return jsonResponse(200, JsonValue(map));
}

ApiResponse ChannelMappingApi::activate(const std::string& body) {
    JsonValue request;
    std::string parseError;
    if (!parseJson(body, request, parseError)) {
        return errorResponse(400, "the body is not JSON: " + parseError);
    }
    if (!request.isObject()) return errorResponse(400, "the body has to be an object");

    const JsonValue& activation = request["activation"];
    if (activation.isObject()) {
        const JsonValue& mode = activation["mode"];
        if (!mode.isNull()) {
            if (!mode.isString()) return errorResponse(400, "activation.mode has to be a string");
            if (mode.asString() != "activate_immediate") {
                return errorResponse(501, "only activate_immediate is implemented, not " +
                                              mode.asString());
            }
        }
    }

    const JsonValue& action = request["action"];
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
    std::map<std::string, ChannelMapping> updated;
    for (const std::string& receiverId : routing_.connectedReceivers()) {
        const auto mapping = routing_.mappingFor(receiverId);
        if (!mapping) continue;
        ChannelMapping working = *mapping;
        working.channelMap = explicitMapOf(working);
        updated[receiverId] = working;
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
            for (int& channel : mapping.channelMap) {
                if (channel == deviceChannel) channel = -1;
            }
        }

        const JsonValue& input = cell["input"];
        if (input.isNull()) continue;  // the cell was emptied
        if (!input.isString()) return errorResponse(400, "a cell's input has to be a string");

        const auto target = updated.find(input.asString());
        if (target == updated.end()) {
            return errorResponse(404, "no input called " + input.asString() +
                                          "; io/ lists the ones there are");
        }

        const JsonValue& index = cell["channel_index"];
        if (!index.isNumber()) {
            return errorResponse(400, "a cell that names an input needs a channel_index");
        }
        const int streamChannel = static_cast<int>(index.asNumber());
        if (streamChannel < 0 ||
            streamChannel >= static_cast<int>(target->second.channelMap.size())) {
            return errorResponse(400, "input " + input.asString() + " has no channel " +
                                          std::to_string(streamChannel));
        }
        target->second.channelMap[static_cast<size_t>(streamChannel)] = deviceChannel;
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

    lastActivationTime_ = nowAsTaiString();
    return activeMap();
}

ApiResponse ChannelMappingApi::handle(const std::string& method, const std::string& path,
                                      const std::string& body) {
    const std::string root = kChannelMappingApiRoot;
    if (path.rfind(root, 0) != 0) return errorResponse(404, "this device serves " + root);

    const std::vector<std::string> segments = segmentsOf(path.substr(root.size()));

    if (segments.empty()) {
        if (method != "GET") return errorResponse(405, "only GET here");
        return jsonResponse(200, JsonValue(JsonArray{JsonValue("io/"), JsonValue("map/")}));
    }

    if (segments[0] == "io") {
        if (method != "GET") return errorResponse(405, "only GET here");
        return describeIo();
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

    if (segments[1] == "activate") {
        if (method != "POST") return errorResponse(405, "POST here");
        return activate(body);
    }

    if (segments[1] == "activations") {
        if (method != "GET") return errorResponse(405, "only GET here");
        // Scheduled activations are what this lists, and there are none
        // because there is no way to schedule one.
        return jsonResponse(200, JsonValue(JsonArray{}));
    }

    return errorResponse(404, "no such resource");
}

}  // namespace AES67::Ravenna
