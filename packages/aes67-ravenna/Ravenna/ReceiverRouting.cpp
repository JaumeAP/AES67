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

ConnectionApi::ReceiverActivation ReceiverRouting::callback() {
    return [this](const std::string& receiverId, const std::string& sdp, bool enable,
                  std::string& why) {
        RoutingOutcome outcome;
        return apply(receiverId, sdp, enable, outcome, why);
    };
}

}  // namespace AES67::Ravenna
