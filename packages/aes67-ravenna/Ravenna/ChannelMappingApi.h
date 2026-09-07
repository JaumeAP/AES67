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
// v1.0, and the parts of it a device with one output block has: io, the
// active map, and immediate activation. Scheduled activation answers 501
// rather than being accepted and forgotten.
//
#pragma once

#include "NetworkEngine/StreamChannelMapper.h"
#include "Ravenna/ConnectionApi.h"
#include "Ravenna/ReceiverRouting.h"

#include <string>

namespace AES67::Ravenna {

inline constexpr char kChannelMappingApiRoot[] = "/x-nmos/channelmapping/v1.0";

/// The one output block: this device's channels. IS-08 allows several, for a
/// device whose outputs are physically separate; here they are one array of
/// device channels, so saying two would be describing hardware that is not
/// there.
inline constexpr char kDeviceOutputId[] = "device";

class ChannelMappingApi {
public:
    ChannelMappingApi(StreamChannelMapper& mapper, ReceiverRouting& routing)
        : mapper_(mapper), routing_(routing) {}

    /// Answers one request. "GET" for io and the map, "POST" to activate.
    ApiResponse handle(const std::string& method, const std::string& path,
                       const std::string& body);

private:
    ApiResponse describeIo() const;
    ApiResponse activeMap() const;
    ApiResponse activate(const std::string& body);

    StreamChannelMapper& mapper_;
    ReceiverRouting& routing_;
    std::string lastActivationTime_;
};

}  // namespace AES67::Ravenna
