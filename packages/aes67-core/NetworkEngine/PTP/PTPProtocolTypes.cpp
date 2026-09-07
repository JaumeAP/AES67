//
// PTPProtocolTypes.cpp
// AES67 core
// The two members of PTPClockIdentity that are not one line.
//
// They were in the macOS driver's PTPSlave.cpp, which is where the type was
// first used. The type itself is declared here in the core, so every other
// implementation that includes the header -- the Linux grandmaster among them
// -- was linking against a definition that lived in a package it does not
// depend on. Moving them here is what makes the header usable by anybody who
// includes it.
//
#include "NetworkEngine/PTP/PTPProtocolTypes.h"

#include <iomanip>
#include <sstream>

namespace AES67 {

std::string PTPClockIdentity::toString() const {
    std::ostringstream oss;
    for (size_t i = 0; i < 8; ++i) {
        if (i > 0) oss << ':';
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(id[i]);
    }
    return oss.str();
}

PTPClockIdentity PTPClockIdentity::fromMAC(const uint8_t mac[6]) {
    PTPClockIdentity cid;
    // EUI-48 to EUI-64 conversion (insert FF:FE in the middle)
    cid.id[0] = mac[0];
    cid.id[1] = mac[1];
    cid.id[2] = mac[2];
    cid.id[3] = 0xFF;
    cid.id[4] = 0xFE;
    cid.id[5] = mac[3];
    cid.id[6] = mac[4];
    cid.id[7] = mac[5];
    return cid;
}

}  // namespace AES67
