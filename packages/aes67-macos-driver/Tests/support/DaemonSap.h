//
// DaemonSap.h
// AES67 macOS Driver - Tests
// The SAP packet the AES67 Linux daemon writes, and the one it accepts.
//
// Mirrors SAP::send and SAP::receive in the vendored daemon
// (packages/aes67-linux-driver/external/aes67-linux-daemon/daemon/sap.cpp),
// which cannot be called from here: they are methods of a class holding a
// Boost.Asio socket and an io_service. What they put on the wire, and what
// they refuse to take off it, is a handful of bytes at fixed offsets, and
// that is what this is for -- so this driver's own SAPAnnouncer can be held
// against a real receiver's rules rather than against RFC 2974 alone.
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AES67 {
namespace Tests {

/// The daemon's own header length: eight bytes plus the sixteen of the
/// payload type (sap.hpp:36, sap_header_len = 24).
constexpr size_t kDaemonSapHeaderLen = 24;

/// The packet the daemon sends (sap.cpp:148-166). The message id hash goes in
/// as the daemon writes it -- memcpy of a uint16_t, so host byte order -- and
/// the originating address in network order.
std::vector<uint8_t> daemonSapPacket(const std::string& sdp,
                                     uint16_t msgIdHash,
                                     uint32_t originatingSourceNetworkOrder,
                                     bool deletion);

/// What the daemon makes of a packet (sap.cpp:105-119): true when it would
/// take the session description out of it, with the fields it read.
struct DaemonSapRead {
    bool accepted{false};
    bool isAnnouncement{false};
    uint16_t msgIdHash{0};
    uint32_t originatingSource{0};
    std::string sdp;
    std::string refusal;  ///< Which of the daemon's tests failed.
};

DaemonSapRead daemonSapRead(const uint8_t* data, size_t length);

} // namespace Tests
} // namespace AES67
