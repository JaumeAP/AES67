#include "Ravenna/ReceiverRouting.h"

#include "Driver/SDPParser.h"

namespace AES67::Ravenna {

void ReceiverRouting::release(const std::string& receiverId) {
    const auto known = streamIdOf_.find(receiverId);
    if (known == streamIdOf_.end()) return;
    mapper_.removeMapping(known->second);
    streamIdOf_.erase(known);
}

bool ReceiverRouting::apply(const std::string& receiverId, const std::string& sdp, bool enable,
                            RoutingOutcome& outcome, std::string& why) {
    outcome = RoutingOutcome{};

    if (!enable) {
        release(receiverId);
        return true;
    }

    const auto parsed = SDPParser::parseString(sdp);
    if (!parsed) {
        why = "the transport file is not an SDP this device can read";
        return false;
    }

    // Whatever this receiver held before: a controller pointing it at another
    // stream is one connection replacing another, not two. Released first, so
    // a receiver moving between two streams of the same width does not fail
    // for want of room it is itself holding.
    release(receiverId);

    auto mapping = mapper_.createDefaultMapping(*parsed);
    if (!mapping) {
        why = "no free device channels for " + std::to_string(parsed->numChannels) +
              " channels";
        return false;
    }
    mapping->streamID = StreamID::generate();

    // What a controller already asked for on this receiver, laid over the
    // block it was just given. Trimmed to the stream's width: a grid set for
    // eight channels and a stream that brings two is two channels routed.
    // The default block is kept if the remembered grid does not fit, and the
    // grid stays remembered for a stream that does.
    const auto asked = remembered_.find(receiverId);
    if (asked != remembered_.end()) {
        ChannelMapping wanted = *mapping;
        // Only the routes for channels this stream actually has: a grid set
        // for eight channels and a stream that brings two is two channels
        // routed, and the rest waits for a stream that has them.
        wanted.routes.clear();
        for (const ChannelRoute& route : asked->second) {
            if (route.streamChannel < wanted.streamChannelCount) wanted.routes.push_back(route);
        }
        if (wanted.routes.empty()) wanted.routes = mapping->routes;
        std::string ignored;
        if (mapper_.validateMapping(wanted, &ignored)) *mapping = wanted;
    }

    // Asked before adding, because addMapping() answers false to a mapping
    // that does not validate and to one that overlaps alike, and those are
    // not the same thing to tell a controller.
    if (!mapper_.validateMapping(*mapping, &why)) return false;
    if (!mapper_.addMapping(*mapping)) {
        why = "the mapping overlaps one already in place";
        return false;
    }

    streamIdOf_[receiverId] = mapping->streamID;

    outcome.connected = true;
    outcome.streamName = parsed->sessionName;
    outcome.channelCount = mapping->deviceChannelCount;
    outcome.deviceChannelStart = mapping->deviceChannelStart;
    return true;
}

std::optional<ChannelMapping> ReceiverRouting::mappingFor(const std::string& receiverId) const {
    const auto known = streamIdOf_.find(receiverId);
    if (known == streamIdOf_.end()) return std::nullopt;
    return mapper_.getMapping(known->second);
}

std::vector<std::string> ReceiverRouting::connectedReceivers() const {
    std::vector<std::string> receivers;
    receivers.reserve(streamIdOf_.size());
    for (const auto& [receiverId, streamId] : streamIdOf_) receivers.push_back(receiverId);
    return receivers;
}

void ReceiverRouting::rememberRoutes(const std::string& receiverId,
                                     std::vector<ChannelRoute> routes) {
    remembered_[receiverId] = std::move(routes);
}

std::vector<ChannelRoute> ReceiverRouting::rememberedRoutes(const std::string& receiverId) const {
    const auto found = remembered_.find(receiverId);
    if (found == remembered_.end()) return {};
    return found->second;
}

std::optional<StreamID> ReceiverRouting::streamIdFor(const std::string& receiverId) const {
    const auto known = streamIdOf_.find(receiverId);
    if (known == streamIdOf_.end()) return std::nullopt;
    return known->second;
}

ConnectionApi::ReceiverActivation ReceiverRouting::callback() {
    return [this](const std::string& receiverId, const std::string& sdp, bool enable,
                  std::string& why) {
        RoutingOutcome outcome;
        return apply(receiverId, sdp, enable, outcome, why);
    };
}

}  // namespace AES67::Ravenna
