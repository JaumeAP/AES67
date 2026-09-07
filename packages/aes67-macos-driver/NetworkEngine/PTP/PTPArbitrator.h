//
// PTPArbitrator.h
// AES67 macOS Driver
// Owns both PTPMaster and the existing PTPSlave, and switches which one is
// actually doing something based on PTPMaster's own BMCA verdict — this is
// the piece that makes the driver capable of "either role, whichever the
// network needs", instead of PTPSlave's original slave-only assumption.
//
// PTPMaster always runs (it has to, to hear foreign Announce and run BMCA);
// it only transmits while its role() is Master. PTPSlave is started/stopped
// to match: running whenever we're NOT the winning clock (Listening or
// Passive), stopped when we are (no point syncing to ourselves). A real PTP
// port state machine has more states (UNCALIBRATED, DISABLED, FAULTY...);
// this collapses to the two that matter for a single-segment AES67 LAN.
//
#pragma once

#include "AudioClockDeviceList.h"
#include "NetworkEngine/PTP/PTPClockSource.h"
#include "NetworkEngine/PTP/PTPDiagnostics.h"
#include "PTPMaster.h"
#include "PTPSlave.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace AES67 {

enum class PTPClockSourceKind {
    Internal,          // This Mac's own free-running clock
    LocalAudioDevice,  // Locked to another CoreAudio device — see AudioClockDeviceList
};

struct PTPArbitratorConfig {
    int domain = 0;
    std::string interfaceName = "en0";

    PTPClockSourceKind clockSourceKind = PTPClockSourceKind::Internal;
    /// Only read when clockSourceKind == LocalAudioDevice — the AudioDeviceID
    /// to lock to, from AudioClockDeviceList::listClockCapableAudioDevices().
    AudioDeviceID lockToDeviceID = kAudioObjectUnknown;

    PTPMasterConfig masterConfig; // domain/interfaceName here are overridden from the two fields above
    PTPSlaveConfig slaveConfig;   // same: domain/interfaceName come from above
};

enum class PTPRole {
    Master,  // We're the grandmaster, transmitting
    Slave,   // Syncing to a better clock on the wire (or still deciding — see PTPMaster::role())
};

class PTPArbitrator {
public:
    explicit PTPArbitrator(const PTPArbitratorConfig& config);
    ~PTPArbitrator();

    PTPArbitrator(const PTPArbitrator&) = delete;
    PTPArbitrator& operator=(const PTPArbitrator&) = delete;

    bool start();
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    PTPRole role() const;

    /// Forwarded to the underlying PTPSlave when it's the active side —
    /// no-op while we're Master (there's nothing to measure against
    /// ourselves).
    void setMeasurementCallback(const PTPMeasurementCallback& cb);

    void updateDiagnostics(PTPDiagnostics& diag) const;

    /// True while we're synced to an external master. Always false while
    /// role() == Master (there's nothing external to be locked to).
    bool isSlaveLocked() const { return slaveActive_.load(std::memory_order_acquire) && slave_->isLocked(); }

    /// The clock source actually driving PTPMaster's Announce/Sync — for UI
    /// display of "currently locked to: ...".
    const PTPClockSource& clockSource() const { return *clockSource_; }

private:
    void monitorThread();

    PTPArbitratorConfig config_;
    std::unique_ptr<PTPClockSource> clockSource_;
    std::unique_ptr<PTPMaster> master_;
    std::unique_ptr<PTPSlave> slave_;

    std::thread monitorThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> slaveActive_{false};
    mutable std::atomic<bool> everWasMaster_{false};

    mutable std::mutex callbackMutex_;
    PTPMeasurementCallback pendingCallback_; // set before slave_ exists yet, applied when it starts
};

} // namespace AES67
