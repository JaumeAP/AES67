//
// ChannelMappingApi.h
// AES67 RAVENNA session layer
// NMOS IS-08: the grid, channel by channel.
//
// IS-05 says which stream a receiver takes. This says where each of that
// stream's channels lands, one cell at a time, which is the part of a routing
// controller that looks like a grid: rows are the channels arriving, columns
// are the device's, and a person clicks a cell.
//
// The grid itself is not new. aes67-core's StreamChannelMapper has carried
// per-channel routing all along, in ChannelMapping::channelMap; what was
// missing was the API that lets anything outside this machine read or change
// it. So nothing here decides where a channel may go -- the matrix does, and
// it refuses what does not fit.
//
// v1.0, and the parts of it a device with one output block has: the inputs
// and outputs with everything under them, io, the active map, and all three
// ways of activating a change to it.
//
#pragma once

#include "NetworkEngine/StreamChannelMapper.h"
#include "Ravenna/ConnectionApi.h"
#include "Ravenna/ReceiverRouting.h"
#include "Ravenna/TaiClock.h"

#include <string>
#include <vector>

namespace AES67::Ravenna {

inline constexpr char kChannelMappingApiRoot[] = "/x-nmos/channelmapping/v1.0";

/// The one output block: this device's channels. IS-08 allows several, for a
/// device whose outputs are physically separate; here they are one array of
/// device channels, so saying two would be describing hardware that is not
/// there.
inline constexpr char kDeviceOutputId[] = "device";

class ChannelMappingApi {
public:
    ChannelMappingApi(StreamChannelMapper& mapper, ReceiverRouting& routing,
                      const ConnectionApi& connections)
        : mapper_(mapper), routing_(routing), connections_(connections) {}

    /// When the grid was last changed, as IS-08 writes a time, and empty
    /// while it never has been.
    ///
    /// IS-08 sec 6: the device carrying this API changes version when its map
    /// does, so that a controller watching IS-04 learns the routing moved
    /// without polling the grid. The device resource does not carry the map,
    /// so this is what tells it something happened.
    const std::string& lastActivation() const { return lastActivationTime_; }

    /// Answers one request. "GET" for io and the map, "POST" to activate.
    ApiResponse handle(const std::string& method, const std::string& path,
                       const std::string& body);

private:
    /// An activation waiting for its moment. IS-08 answers a scheduled one
    /// with a 202 and an id, and a controller reads it back, or deletes it,
    /// under that id until the time comes.
    struct PendingActivation {
        std::string id;
        std::string mode;
        std::string requestedTime;
        std::string activationTime;  ///< when it is due, as IS-08 reports it
        TaiTime due;
        JsonValue action;
    };

    ApiResponse describeIo() const;
    ApiResponse activeMap() const;
    /// POST to map/activations: applies the change now, or promises it.
    ApiResponse postActivation(const std::string& body);
    /// Works the action into the matrix. The whole change is decided before
    /// any of it is applied, so a bad cell leaves the grid as it was.
    ///
    /// `commit=false` runs every validation and would-be-mapping check this
    /// does, then restores the matrix exactly as the failure path already
    /// does on a refusal -- a dry run, for postActivation()'s scheduled
    /// branch to ask "would this be refused" without a controller having to
    /// wait for the scheduled time to find out the answer was always no.
    ApiResponse applyAction(const JsonValue& action, bool commit = true);
    /// Fires whatever is due. Called at the top of every request, which is
    /// the only clock this API is driven by.
    void applyDueActivations();
    JsonValue activationEnvelope(const PendingActivation& activation) const;

    /// An input per receiver, whether or not it is carrying anything. IS-08's
    /// inputs are the ports a controller may route FROM, and a device that
    /// published only the ones already connected gave a controller nothing to
    /// draw a grid with until somebody had connected them by other means.
    bool hasInput(const std::string& id) const;
    JsonValue inputProperties(const std::string& id) const;
    JsonValue inputChannels(const std::string& id) const;
    JsonValue inputParent(const std::string& id) const;
    JsonValue inputCaps() const;

    JsonValue outputProperties() const;
    JsonValue outputChannels() const;
    JsonValue outputCaps() const;
    JsonValue outputSourceId() const;

    StreamChannelMapper& mapper_;
    ReceiverRouting& routing_;
    const ConnectionApi& connections_;
    std::string lastActivationTime_;
    std::vector<PendingActivation> pending_;
    uint64_t nextActivationId_ = 1;
};

}  // namespace AES67::Ravenna
