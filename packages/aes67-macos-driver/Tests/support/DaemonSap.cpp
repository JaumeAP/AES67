//
// DaemonSap.cpp
// AES67 macOS Driver - Tests
//

#include "support/DaemonSap.h"

#include <cctype>
#include <cstring>

namespace AES67 {
namespace Tests {

std::vector<uint8_t> daemonSapPacket(const std::string& sdp,
                                     uint16_t msgIdHash,
                                     uint32_t originatingSourceNetworkOrder,
                                     bool deletion) {
    std::vector<uint8_t> packet(kDaemonSapHeaderLen + sdp.size(), 0);
    packet[0] = deletion ? 0x24 : 0x20;
    packet[1] = 0;
    std::memcpy(packet.data() + 2, &msgIdHash, sizeof(msgIdHash));
    std::memcpy(packet.data() + 4, &originatingSourceNetworkOrder,
                sizeof(originatingSourceNetworkOrder));
    std::memcpy(packet.data() + 8, "application/sdp", 16);
    std::memcpy(packet.data() + kDaemonSapHeaderLen, sdp.data(), sdp.size());
    return packet;
}

DaemonSapRead daemonSapRead(const uint8_t* data, size_t length) {
    DaemonSapRead read;

    if (length <= 4) {
        read.refusal = "shorter than five bytes";
        return read;
    }
    if (data[0] != 0x20 && data[0] != 0x24) {
        read.refusal = "first byte is not 0x20 or 0x24: version 1, IPv4, "
                       "no reserved, compression or encryption bit";
        return read;
    }
    read.isAnnouncement = data[0] == 0x20;
    std::memcpy(&read.msgIdHash, data + 2, sizeof(read.msgIdHash));
    std::memcpy(&read.originatingSource, data + 4, sizeof(read.originatingSource));

    // The daemon lower-cases the type in place before comparing, up to the
    // first NUL, then demands the sixteen bytes at offset 8 be exactly
    // "application/sdp" and its terminator.
    char type[16] = {0};
    const size_t available = length > 8 ? std::min<size_t>(length - 8, 16) : 0;
    std::memcpy(type, data + 8, available);
    for (size_t i = 0; i < available && type[i] != 0; ++i) {
        type[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(type[i])));
    }
    if (available < 16 || std::memcmp(type, "application/sdp", 16) != 0) {
        read.refusal = "the sixteen bytes at offset 8 are not \"application/sdp\\0\"";
        return read;
    }

    read.sdp.assign(reinterpret_cast<const char*>(data) + kDaemonSapHeaderLen,
                    reinterpret_cast<const char*>(data) + length);
    read.accepted = true;
    return read;
}

} // namespace Tests
} // namespace AES67
