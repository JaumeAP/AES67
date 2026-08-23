//
// PTPDInterface.cpp
// AES67 macOS Driver
// PTP Interface — bridges PTPSlave (real IEEE 1588) with the rest of the driver.
// Falls back to stub mode when real PTP is unavailable or explicitly disabled.
//

#include "PTPDInterface.h"
#include "PTPSlave.h"
#include <iostream>

namespace AES67 {

PTPDInterface::PTPDInterface(bool useStub)
    : running_(false)
    , stubMode_(useStub)
{
    // Initialize state with defaults
    state_.masterOffsetNs.store(0);
    state_.frequencyDrift.store(0.0);
    state_.isLocked.store(false);
    state_.clockClass.store(248); // Default to slave-only clock
    state_.clockAccuracy.store(0xFE); // Unknown accuracy
    state_.offsetNs.store(0);

    // Initialize diagnostics with defaults
    diagnostics_.isConnected = false;
    diagnostics_.isLocked = false;
    diagnostics_.masterClockID = "";
    diagnostics_.currentDomain = 0;
    diagnostics_.currentOffset = 0.0;
}

PTPDInterface::~PTPDInterface() {
    stop();
}

void PTPDInterface::enableMasterCapability(PTPClockSourceKind clockSourceKind,
                                            AudioDeviceID lockToDeviceID) {
    masterCapable_ = true;
    masterClockSourceKind_ = clockSourceKind;
    masterLockToDeviceID_ = lockToDeviceID;
}

bool PTPDInterface::init(const std::string& interfaceName) {
    interfaceName_ = interfaceName;

    if (stubMode_) {
        std::cout << "[PTPDInterface] Stub initialization for interface: "
                  << interfaceName << std::endl;
        std::cout << "[PTPDInterface] NOTE: PTP synchronization not available - using local clock"
                  << std::endl;
        return true;
    }

    auto onMeasurement = [this](const PTPMeasurement& m) {
        if (!m.valid) return;
        onPTPMeasurement(
            m.offsetFromMasterNs,
            m.meanPathDelayNs,
            m.frequencyDriftPpb,
            m.clockClass,
            m.clockAccuracy,
            true, // We received a valid measurement
            m.grandmasterID.toString()
        );
    };

    if (masterCapable_) {
        PTPArbitratorConfig config;
        config.domain = domain_;
        config.interfaceName = interfaceName;
        config.clockSourceKind = masterClockSourceKind_;
        config.lockToDeviceID = masterLockToDeviceID_;

        ptpArbitrator_ = std::make_unique<PTPArbitrator>(config);
        ptpArbitrator_->setMeasurementCallback(onMeasurement);

        std::cout << "[PTPDInterface] Master-capable PTP initialization for interface: "
                  << interfaceName << " domain=" << domain_
                  << " clockSource=" << ptpArbitrator_->clockSource().name() << std::endl;
        return true;
    }

    // Slave-only mode — original behavior, unchanged.
    PTPSlaveConfig config;
    config.domain = domain_;
    config.interfaceName = interfaceName;
    config.delayReqIntervalMs = 1000;  // 1 second between Delay_Req messages
    config.twoStepOnly = true;          // AES67 uses two-step clocks

    ptpSlave_ = std::make_unique<PTPSlave>(config);
    ptpSlave_->setMeasurementCallback(onMeasurement);

    std::cout << "[PTPDInterface] Real PTP initialization for interface: "
              << interfaceName << " domain=" << domain_ << std::endl;

    return true;
}

void PTPDInterface::start() {
    if (running_) {
        return;
    }

    running_ = true;
    diagnostics_.isConnected = true;

    if (stubMode_) {
        // Stub mode: isLocked stays FALSE. The local clock fallback will be
        // used for media clock recovery instead.
        state_.isLocked.store(false);
        state_.clockClass.store(255); // Clock class 255 = slave-only, not traceable
        diagnostics_.isLocked = false;
        diagnostics_.masterClockID = "STUB-LOCAL-CLOCK (NOT SYNCHRONIZED)";

        std::cerr << "[PTPDInterface] WARNING: PTP STUB MODE - clock is NOT synchronized. "
                  << "Using local clock fallback for media clock recovery. "
                  << "Multi-device sync will not work." << std::endl;
        return;
    }

    // Start whichever real path we were init()'d with
    if (ptpArbitrator_) {
        if (!ptpArbitrator_->start()) {
            std::cerr << "[PTPDInterface] Failed to start PTPArbitrator on "
                      << interfaceName_ << " — falling back to stub mode" << std::endl;
            stubMode_ = true;
            state_.isLocked.store(false);
            state_.clockClass.store(255);
            diagnostics_.isLocked = false;
            diagnostics_.masterClockID = "STUB-LOCAL-CLOCK (PTP START FAILED)";
            return;
        }
        std::cout << "[PTPDInterface] PTPArbitrator started on "
                  << interfaceName_ << " domain=" << domain_ << std::endl;
    } else if (ptpSlave_) {
        if (!ptpSlave_->start()) {
            // Failed to start real PTP — fall back to stub mode
            std::cerr << "[PTPDInterface] Failed to start PTP slave on "
                      << interfaceName_ << " — falling back to stub mode" << std::endl;
            stubMode_ = true;
            state_.isLocked.store(false);
            state_.clockClass.store(255);
            diagnostics_.isLocked = false;
            diagnostics_.masterClockID = "STUB-LOCAL-CLOCK (PTP START FAILED)";
            return;
        }

        std::cout << "[PTPDInterface] PTP slave started on "
                  << interfaceName_ << " domain=" << domain_ << std::endl;
    }
}

void PTPDInterface::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    if (ptpArbitrator_) {
        ptpArbitrator_->stop();
    } else if (ptpSlave_) {
        ptpSlave_->stop();
    }

    state_.isLocked.store(false);
    diagnostics_.isConnected = false;
    diagnostics_.isLocked = false;

    std::cout << "[PTPDInterface] Stopped" << std::endl;
}

PTPState& PTPDInterface::getState() {
    return state_;
}

PTPDiagnostics& PTPDInterface::getDiagnostics() {
    if (ptpArbitrator_ && !stubMode_) {
        ptpArbitrator_->updateDiagnostics(diagnostics_);
    } else if (ptpSlave_ && !stubMode_) {
        ptpSlave_->updateDiagnostics(diagnostics_);
    }
    return diagnostics_;
}

PTPRole PTPDInterface::getRole() const {
    return ptpArbitrator_ ? ptpArbitrator_->role() : PTPRole::Slave;
}

void PTPDInterface::onPTPMeasurement(int64_t offsetNs, int64_t pathDelayNs,
                                      double driftPpb, uint8_t clockClass,
                                      uint8_t clockAccuracy, bool locked,
                                      const std::string& grandmasterID) {
    // Update atomic state (consumed by PTPClock and audio threads)
    state_.masterOffsetNs.store(offsetNs);
    state_.frequencyDrift.store(driftPpb);
    state_.offsetNs.store(static_cast<uint64_t>(std::abs(offsetNs)));
    state_.clockClass.store(clockClass);
    state_.clockAccuracy.store(clockAccuracy);

    // Lock state is determined by whichever slave-side path is active — only
    // update if it says locked.
    if (ptpArbitrator_) {
        bool slaveLocked = ptpArbitrator_->isSlaveLocked();
        state_.isLocked.store(slaveLocked);
        diagnostics_.isLocked = slaveLocked;
    } else if (ptpSlave_) {
        bool slaveLocked = ptpSlave_->isLocked();
        state_.isLocked.store(slaveLocked);
        diagnostics_.isLocked = slaveLocked;
    }

    // Update diagnostics (non-atomic, for UI/monitoring)
    diagnostics_.currentOffset = static_cast<double>(offsetNs);
    diagnostics_.offsetNs = offsetNs;
    diagnostics_.frequencyOffset = driftPpb / 1000.0; // ppb to ppm
    diagnostics_.masterClockID = grandmasterID;
    diagnostics_.clockClass = clockClass;
    diagnostics_.clockAccuracy = clockAccuracy;
    diagnostics_.lastMessageTime = std::chrono::steady_clock::now();
}

} // namespace AES67
