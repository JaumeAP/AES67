//
// ReceiverRouting.h
// AES67 RAVENNA session layer
// What happens to the channels when a receiver is activated.
//
// IS-05 says which stream a receiver takes; this is what that means for the
// device. It sits between ConnectionApi and aes67-core's StreamChannelMapper
// and owns the one thing neither of them can: the fact that a receiver, named
// by the connection API, holds a mapping, keyed in the matrix by a StreamID.
//
// It was written inside the announcer tool, where it could not be tested and
// where the demonstration and the behaviour were the same code.
//
#pragma once

#include "NetworkEngine/StreamChannelMapper.h"
#include "Ravenna/ConnectionApi.h"

#include <map>
#include <optional>
#include <string>

namespace AES67::Ravenna {

/// What an activation did, for whoever wants to report it.
struct RoutingOutcome {
    bool connected = false;        ///< false when the receiver was disabled
    std::string streamName;
    uint16_t channelCount = 0;
    uint16_t deviceChannelStart = 0;
};

class ReceiverRouting {
public:
    explicit ReceiverRouting(StreamChannelMapper& mapper) : mapper_(mapper) {}

    /// Applies what a controller activated.
    ///
    /// Enabling parses the transport file, asks the matrix for room and takes
    /// it. Disabling gives the block back -- without that, the next receiver
    /// finds the channels taken by a connection nobody has any more.
    ///
    /// Pointing a receiver at another stream replaces its mapping rather than
    /// adding one: one receiver is one connection.
    bool apply(const std::string& receiverId, const std::string& sdp, bool enable,
               RoutingOutcome& outcome, std::string& why);

    /// What this receiver holds now, if anything.
    std::optional<ChannelMapping> mappingFor(const std::string& receiverId) const;

    /// The same thing shaped for ConnectionApi::onReceiverActivation, so the
    /// wiring is one line and not a lambda in every caller.
    ConnectionApi::ReceiverActivation callback();

private:
    void release(const std::string& receiverId);

    StreamChannelMapper& mapper_;
    /// A receiver's name is not a StreamID: the matrix keys on a UUID, and one
    /// has to be kept per receiver so that disabling frees what that same
    /// receiver took.
    std::map<std::string, StreamID> streamIdOf_;
};

}  // namespace AES67::Ravenna
