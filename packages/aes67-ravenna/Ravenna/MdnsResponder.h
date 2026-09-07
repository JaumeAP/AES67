//
// MdnsResponder.h
// AES67 RAVENNA session layer
// The socket half of discovery.
//
// It answers queries for _rtsp._tcp and for RAVENNA's subtype, and announces
// unprompted when a session appears or changes, which is what RFC 6762 asks
// for and what makes a session show up in a browser that was already open.
//
// It is not a general mDNS responder. It does not defend names, does not
// probe before claiming one, and answers nothing but its own service -- a
// second RAVENNA device with the same session name is a fault to notice
// rather than a conflict to negotiate, and pretending otherwise would mean
// implementing a protocol this package does not need.
//
#pragma once

#include "Ravenna/SessionCatalogue.h"

#include <cstdint>
#include <string>
#include <vector>

namespace AES67::Ravenna {

class MdnsResponder {
public:
    explicit MdnsResponder(const SessionCatalogue& catalogue) : catalogue_(catalogue) {}
    ~MdnsResponder();

    MdnsResponder(const MdnsResponder&) = delete;
    MdnsResponder& operator=(const MdnsResponder&) = delete;

    /// Joins the mDNS group on `interfaceName` and prepares to answer for
    /// `hostName` at `addressV4`, with the RTSP server on `port`.
    bool start(const std::string& interfaceName, const std::string& hostName,
               uint32_t addressV4, uint16_t port, std::string& error);
    void stop();

    /// Something else to advertise under the same responder: the NMOS node,
    /// which is a service of its own rather than a session. Added before
    /// start(), and announced and withdrawn with the sessions.
    void alsoAdvertise(const SessionAdvertisement& service);

    /// Answers whatever queries are waiting. Returns how many it answered.
    size_t service();

    /// Announces every session unprompted. RFC 6762 sec 8.3 asks for a few of
    /// these when a service appears; call it at start-up and after a change.
    void announce();

    /// Withdraws every session with a zero TTL, so a browser drops them now
    /// rather than in 75 minutes. Called by the destructor too, because the
    /// common way a daemon stops is a signal.
    void goodbye();

private:
    bool sendPacket(const std::vector<uint8_t>& packet);
    std::vector<SessionAdvertisement> everything() const;

    const SessionCatalogue& catalogue_;
    std::vector<SessionAdvertisement> extras_;
    int socket_ = -1;
    std::string hostName_;
    uint32_t addressV4_ = 0;
    uint16_t port_ = 0;
};

}  // namespace AES67::Ravenna
