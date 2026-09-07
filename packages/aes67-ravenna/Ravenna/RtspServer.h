//
// RtspServer.h
// AES67 RAVENNA session layer
// The socket half of DESCRIBE.
//
// POSIX, so it builds on the Mac it is written on and on the Pi it runs on.
// One thread, poll-driven: this answers a question a device asks once when it
// discovers a session, not a stream.
//
#pragma once

#include "Ravenna/SessionCatalogue.h"

#include <cstdint>
#include <string>
#include <vector>

namespace AES67::Ravenna {

class RtspServer {
public:
    explicit RtspServer(const SessionCatalogue& catalogue) : catalogue_(catalogue) {}
    ~RtspServer();

    RtspServer(const RtspServer&) = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    /// Listens on `port`, on every address. 554 is RTSP's, and it needs
    /// privilege; anything above 1024 does not, and RAVENNA finds it anyway
    /// because the SRV record carries the port.
    bool start(uint16_t port, std::string& error);
    void stop();

    /// Accepts what is waiting, answers it and closes. Returns how many
    /// requests were answered, which is almost always zero.
    size_t service();

    uint16_t port() const { return port_; }

private:
    void answer(int client, const std::string& request);

    const SessionCatalogue& catalogue_;
    int listener_ = -1;
    uint16_t port_ = 0;
};

}  // namespace AES67::Ravenna
