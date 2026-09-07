//
// NodeAdvertiser.h
// AES67 macOS Driver
//
// What makes a controller find this node without a registry: the
// _nmos-node._tcp service over mDNS (IS-04 peer-to-peer discovery). The
// records and the responder are aes67-ravenna's; this is the thread that
// runs them inside the driver, and the one record set the driver has to
// advertise.
//
#pragma once

#include "Ravenna/DnsSd.h"
#include "Ravenna/MdnsResponder.h"
#include "Ravenna/SessionCatalogue.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace AES67 {

/// The `_nmos-node._tcp` record set for this node. `addressV4` in host
/// byte order; `hostName` is the name the SRV points at and must end in
/// ".local".
Ravenna::SessionAdvertisement nodeAdvertisement(const std::string& label,
                                                const std::string& hostName,
                                                uint32_t addressV4, uint16_t apiPort);

class NodeAdvertiser {
public:
    NodeAdvertiser();
    ~NodeAdvertiser();

    NodeAdvertiser(const NodeAdvertiser&) = delete;
    NodeAdvertiser& operator=(const NodeAdvertiser&) = delete;

    /// Joins mDNS on `interfaceName`, announces three times a second apart
    /// (RFC 6762 sec 8.3), then answers queries until stop().
    bool start(const std::string& interfaceName,
               const Ravenna::SessionAdvertisement& advertisement, std::string& error);

    /// Withdraws the service with a zero TTL and joins the thread.
    void stop();

private:
    /// The responder wants a catalogue of RTSP sessions; the driver's
    /// sessions are announced over SAP, not RTSP, so this one stays empty
    /// and only the node is advertised.
    Ravenna::SessionCatalogue noSessions_;
    std::unique_ptr<Ravenna::MdnsResponder> responder_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace AES67
