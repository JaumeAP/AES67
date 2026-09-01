#include "PTPArbitrator.h"
#include "CoreAudioClockSource.h"

#include <iostream>

namespace AES67 {

PTPArbitrator::PTPArbitrator(const PTPArbitratorConfig& config) : config_(config) {
    if (config_.clockSourceKind == PTPClockSourceKind::LocalAudioDevice &&
        config_.lockToDeviceID != kAudioObjectUnknown) {
        // Name is cosmetic here (diagnostics/logging); CoreAudioClockSource
        // re-derives its actual locked/unlocked state live from the device,
        // it doesn't cache anything from construction time.
        clockSource_ = std::make_unique<CoreAudioClockSource>(config_.lockToDeviceID, "Locked device");
    } else {
        clockSource_ = std::make_unique<InternalClockSource>();
    }

    PTPMasterConfig masterConfig = config_.masterConfig;
    masterConfig.domain = config_.domain;
    masterConfig.interfaceName = config_.interfaceName;
    master_ = std::make_unique<PTPMaster>(masterConfig, *clockSource_);

    PTPSlaveConfig slaveConfig = config_.slaveConfig;
    slaveConfig.domain = config_.domain;
    slaveConfig.interfaceName = config_.interfaceName;
    slave_ = std::make_unique<PTPSlave>(slaveConfig);
}

PTPArbitrator::~PTPArbitrator() { stop(); }

bool PTPArbitrator::start() {
    if (running_.load(std::memory_order_acquire)) return false;

    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (pendingCallback_) slave_->setMeasurementCallback(pendingCallback_);
    }

    if (!master_->start()) {
        std::cerr << "[PTPArbitrator] Failed to start PTPMaster" << std::endl;
        return false;
    }

    running_.store(true, std::memory_order_release);
    monitorThread_ = std::thread(&PTPArbitrator::monitorThread, this);
    return true;
}

void PTPArbitrator::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    if (monitorThread_.joinable()) monitorThread_.join();

    slave_->stop();
    slaveActive_.store(false, std::memory_order_release);
    master_->stop();
}

PTPRole PTPArbitrator::role() const {
    return master_->isActive() ? PTPRole::Master : PTPRole::Slave;
}

void PTPArbitrator::setMeasurementCallback(PTPMeasurementCallback cb) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    pendingCallback_ = cb;
    if (slave_) slave_->setMeasurementCallback(cb);
}

void PTPArbitrator::updateDiagnostics(PTPDiagnostics& diag) const {
    const bool weAreMaster = master_->isActive();
    if (weAreMaster) everWasMaster_.store(true, std::memory_order_relaxed);

    if (weAreMaster) {
        // We're the grandmaster: no offset to report (we ARE the reference),
        // but the rest of the diagnostic surface should reflect that state
        // honestly rather than showing stale slave numbers.
        diag.isConnected = true;
        diag.isLocked = true;
        diag.masterClockID = "SELF (acting as grandmaster)";
        diag.offsetNs = 0;
        diag.currentOffset = 0.0;
        diag.role = PTPDiagnostics::Role::Master;
        diag.hasCompetitor = false;
    } else {
        slave_->updateDiagnostics(diag);
        diag.role = PTPDiagnostics::Role::Slave;
    }

    diag.everWasMaster = everWasMaster_.load(std::memory_order_relaxed);

    // Who we lost BMCA to (or are still listening for) — meaningful in
    // either PTPMasterRole::Listening or ::Passive, i.e. whenever we're not
    // the active master ourselves.
    if (!weAreMaster) {
        if (auto competitor = master_->currentCompetitor()) {
            diag.hasCompetitor = true;
            diag.competitorPriority1 = competitor->grandmasterPriority1;
            diag.competitorPriority2 = competitor->grandmasterPriority2;
        } else {
            diag.hasCompetitor = false;
        }
    }
}

void PTPArbitrator::monitorThread() {
    while (running_.load(std::memory_order_acquire)) {
        const bool weAreMaster = master_->isActive();
        const bool slaveShouldRun = !weAreMaster;

        if (slaveShouldRun && !slaveActive_.load(std::memory_order_acquire)) {
            if (slave_->start()) {
                slaveActive_.store(true, std::memory_order_release);
            }
        } else if (!slaveShouldRun && slaveActive_.load(std::memory_order_acquire)) {
            slave_->stop();
            slaveActive_.store(false, std::memory_order_release);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

} // namespace AES67
