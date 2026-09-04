#ifndef RTCP_MONITOR_H
#define RTCP_MONITOR_H

//
// RTCPMonitor
// AES67 macOS Driver
//
// Listens on the RTCP ports of this driver's transmit streams and feeds every
// Sender/Receiver Report into an RTCPReceiverTable, so the distinct receivers
// of what this driver sends — a DAC3202 or DMA amplifier, if it emits RTCP —
// can be counted. The second detection vector beside PTPPeerObserver.
//
// The set of RTCP endpoints to watch is pulled from a provider callback and
// reconciled periodically, so streams starting and stopping open and close
// sockets without the caller managing them. Never sends; purely observational.
//

#include "NetworkEngine/Discovery/RTCPReceiverTable.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AES67 {

class RTCPMonitor {
public:
    struct Endpoint {
        std::string multicastIp; // the stream's multicast group
        uint16_t rtcpPort{0};    // usually the RTP destination port + 1
        bool operator==(const Endpoint& o) const {
            return multicastIp == o.multicastIp && rtcpPort == o.rtcpPort;
        }
    };

    // Returns the RTCP endpoints to watch right now. Called on the monitor's
    // own thread, off the real-time path.
    using EndpointProvider = std::function<std::vector<Endpoint>()>;

    RTCPMonitor();
    ~RTCPMonitor();

    // interfaceName selects the multicast RX interface (empty = default).
    bool start(EndpointProvider provider, const std::string& interfaceName = "");
    void stop();
    bool isRunning() const;

    // Snapshot of the receivers seen within RTCPReceiverTable::kReporterTimeout.
    std::vector<RTCPReporter> reporters() const;

    // Record one RTCP packet as if it had arrived from `sourceIp`.
    //
    // The receive path calls this with what it read off a socket; a test
    // calls it with bytes it built, which is the only way to exercise the
    // recording without a multicast group (2026-09-04 audit — this class was
    // at zero coverage). Parsing itself belongs to RTCPReceiverTable, which
    // is tested in the core; what this covers is the wiring between a packet
    // and the reporter table.
    void deliverPacket(const uint8_t* data, size_t length, const std::string& sourceIp);

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace AES67

#endif // RTCP_MONITOR_H
