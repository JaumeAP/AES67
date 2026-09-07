//
// PtpSockets.h
// AES67 Linux PTP daemon
// The two UDP sockets a PTP port lives on, and the hardware timestamps the
// NIC takes on them.
//
// Everything Linux-specific about sending and receiving is here: the
// multicast joins, SO_TIMESTAMPING, the SIOCSHWTSTAMP that turns the NIC's
// stamping on, and the error queue a transmit timestamp comes back through.
// The messages themselves are PtpWire.h, which knows none of it.
//
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace AES67::LinuxPtpd {

class PtpSockets {
public:
    PtpSockets() = default;
    ~PtpSockets();

    PtpSockets(const PtpSockets&) = delete;
    PtpSockets& operator=(const PtpSockets&) = delete;

    /// Binds 319 and 320, joins the PTP group on `interfaceName` and asks the
    /// NIC to timestamp PTP v2 events. `error` carries why on false.
    ///
    /// Turning the NIC's stamping on needs CAP_NET_ADMIN; without it this
    /// fails rather than falling back, because a master whose timestamps come
    /// from the kernel instead of the wire is a master that lies about when it
    /// sent. `allowSoftwareTimestamps` is the deliberate opt-out, for a board
    /// whose driver offers nothing better.
    bool open(const std::string& interfaceName, bool allowSoftwareTimestamps,
              std::string& error);
    void close();

    /// The interface's MAC, which is what the clock identity is built from.
    const std::array<uint8_t, 6>& mac() const { return mac_; }

    int eventFd() const { return eventFd_; }
    int generalFd() const { return generalFd_; }

    /// True when the NIC is doing the stamping. False means the timestamps
    /// come from the kernel, and everything that reads them should say so.
    bool hardwareTimestamps() const { return hardwareTimestamps_; }

    /// Sends to the PTP group on port 319 and waits briefly for the transmit
    /// timestamp to come back on the error queue. `txTimeNs` is left at zero
    /// when it does not arrive in time, which the caller must treat as a
    /// Sync it cannot follow up.
    bool sendEvent(const uint8_t* data, size_t length, uint64_t& txTimeNs,
                   std::string& error);

    /// Sends to the group on port 320. General messages carry their time
    /// inside them, so there is nothing to stamp.
    bool sendGeneral(const uint8_t* data, size_t length, std::string& error);

    /// One datagram, if one is waiting. `receiveTimeNs` is the hardware
    /// receive timestamp, or the kernel's if that is what is in use. Returns
    /// false when nothing was read, which is the ordinary case.
    bool receive(int fd, uint8_t* buffer, size_t capacity, size_t& length,
                 uint64_t& receiveTimeNs);

private:
    bool openOne(uint16_t port, int& fd, std::string& error);
    bool enableHardwareTimestamping(const std::string& interfaceName,
                                    std::string& error);

    int eventFd_ = -1;
    int generalFd_ = -1;
    unsigned int interfaceIndex_ = 0;
    uint32_t interfaceAddress_ = 0;  ///< network order
    std::array<uint8_t, 6> mac_{};
    bool hardwareTimestamps_ = false;
};

}  // namespace AES67::LinuxPtpd
