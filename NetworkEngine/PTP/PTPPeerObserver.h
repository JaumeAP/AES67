#ifndef PTP_PEER_OBSERVER_H
#define PTP_PEER_OBSERVER_H

//
// PTPPeerObserver
// AES67 macOS Driver
//
// Passive discovery of PTP participants on the local segment. Binds the PTP
// event (319) and general (320) ports read-only, joins the PTP multicast
// group, and feeds every message's (clock identity, type, source IP, domain)
// into a PTPPeerTable. It never sends and never disciplines anything — it just
// watches — so it runs independently of, and alongside, whatever PTP role this
// driver is playing (master, slave, or none).
//
// It exists to answer "which Dolby elements are on the network, and how many"
// without the elements having to announce themselves at the SAP/SDP layer
// (a receiver like a DAC3202/DMA never does). See PTPPeerTable.h and
// Docs/dac3202_autodetection_study.md.
//

#include "NetworkEngine/PTP/PTPPeerTable.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace AES67 {

class PTPPeerObserver {
public:
    PTPPeerObserver();
    ~PTPPeerObserver();

    // interfaceName selects the egress/RX interface (e.g. "en0"); empty lets
    // the kernel choose the default. Returns false if the sockets can't be
    // set up — like SAP discovery, the caller treats that as non-fatal.
    bool start(const std::string& interfaceName = "");
    void stop();
    bool isRunning() const;

    // Snapshot of the peers seen within PTPPeerTable::kPeerTimeout, swept as
    // of now. Safe to call from any thread.
    std::vector<PTPPeerObservation> peers() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace AES67

#endif // PTP_PEER_OBSERVER_H
