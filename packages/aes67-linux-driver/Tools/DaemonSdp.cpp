//
// DaemonSdp.cpp
// aes67-linux-driver
//
// Line for line with session_manager.cpp:754-790, in the order it writes
// them. The duplicated-stream half of that function (ST 2022-7, a=group:DUP)
// is not here: this package configures one stream at a time and the second
// interface is not something a profile describes.
//

#include "Tools/DaemonSdp.h"

#include <iomanip>
#include <sstream>

namespace AES67::LinuxDriver {

std::string daemonPtime(uint32_t maxSamplesPerPacket, uint32_t sampleRate) {
    std::ostringstream out;
    out.precision(12);
    out << std::fixed
        << static_cast<double>(maxSamplesPerPacket) * 1000
               / static_cast<double>(sampleRate);
    std::string ptime = out.str();
    ptime.erase(ptime.find_last_not_of("0.") + 1, std::string::npos);
    return ptime;
}

namespace {

/// The daemon writes the TTL into "c=" only for a multicast destination
/// (IN_MULTICAST, session_manager.cpp:766).
bool isMulticast(const std::string& address) {
    const auto dot = address.find('.');
    if (dot == std::string::npos || dot == 0) return false;
    const std::string head = address.substr(0, dot);
    for (const char c : head) {
        if (c < '0' || c > '9') return false;
    }
    const int octet = std::stoi(head);
    return octet >= 224 && octet <= 239;
}

} // namespace

std::string daemonSdp(const DaemonSdpParams& params) {
    std::ostringstream ss;
    ss << "v=0\n"
       << "o=- " << params.sessionId << " " << params.sessionVersion << " IN IP4 "
       << params.sourceIp << "\n"
       << "s=" << params.nodeId << " " << params.sourceName << "\n"
       << "t=0 0\n"
       << "m=audio " << params.destinationPort << " RTP/AVP "
       << static_cast<unsigned>(params.payloadType) << "\n"
       << "c=IN IP4 " << params.destinationIp;
    if (isMulticast(params.destinationIp)) {
        ss << "/" << static_cast<unsigned>(params.ttl);
    }
    ss << "\na=source-filter: incl IN IP4 " << params.destinationIp << " "
       << params.sourceIp;
    ss << "\na=rtpmap:" << static_cast<unsigned>(params.payloadType) << " "
       << params.codec << "/" << params.sampleRate << "/"
       << static_cast<unsigned>(params.channels) << "\n"
       << "a=sync-time:0\n"
       << "a=framecount:" << params.maxSamplesPerPacket << "\n"
       << "a=ptime:" << daemonPtime(params.maxSamplesPerPacket, params.sampleRate) << "\n"
       << "a=mediaclk:direct=0\n"
       << "a=clock-domain:PTPv2 " << static_cast<unsigned>(params.ptpDomain)
       << "\na=ts-refclk:ptp=IEEE1588-2008:";
    if (params.refclkPtpTraceable) {
        ss << "traceable\n";
    } else {
        ss << params.gmid << ":" << static_cast<unsigned>(params.ptpDomain) << "\n";
    }
    ss << "a=recvonly\n";
    return ss.str();
}

} // namespace AES67::LinuxDriver
