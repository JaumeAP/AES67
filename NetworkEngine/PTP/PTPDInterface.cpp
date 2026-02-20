//
// PTPDInterface.cpp
// AES67 macOS Driver
// Stub implementation - PTP synchronization placeholder
// TODO: Integrate proper PTP synchronization when ptpd vendoring is complete
//

#include "PTPDInterface.h"
#include <iostream>

namespace AES67 {

PTPDInterface::PTPDInterface()
    : running_(false), ptpdInstance_(nullptr) {
    // Initialize state with defaults
    state_.masterOffsetNs.store(0);
    state_.frequencyDrift.store(0.0);
    state_.isLocked.store(false);
    state_.clockClass.store(248); // Default to slave-only clock
    state_.clockAccuracy.store(0xFE); // Unknown accuracy
    state_.offsetNs.store(0);

    // Initialize diagnostics with defaults (matching PTPDiagnostics struct)
    diagnostics_.isConnected = false;
    diagnostics_.isLocked = false;
    diagnostics_.masterClockID = "";
    diagnostics_.currentDomain = 0;
    diagnostics_.currentOffset = 0.0;
}

PTPDInterface::~PTPDInterface() {
    stop();
}

bool PTPDInterface::init(const std::string& interfaceName) {
    interfaceName_ = interfaceName;

    std::cout << "[PTPDInterface] Stub initialization for interface: " << interfaceName << std::endl;
    std::cout << "[PTPDInterface] NOTE: PTP synchronization not available - using local clock" << std::endl;

    return true;
}

void PTPDInterface::start() {
    if (running_) {
        return;
    }

    running_ = true;
    diagnostics_.isConnected = true;

    // Stub mode: isLocked stays FALSE. The local clock fallback will be
    // used for media clock recovery instead. Do NOT fake a lock — that
    // causes downstream code (resampling, presentation timing) to trust a
    // clock source that doesn't exist.
    state_.isLocked.store(false);
    state_.clockClass.store(255); // Clock class 255 = slave-only, not traceable
    diagnostics_.isLocked = false;
    diagnostics_.masterClockID = "STUB-LOCAL-CLOCK (NOT SYNCHRONIZED)";

    std::cerr << "[PTPDInterface] WARNING: PTP STUB MODE - clock is NOT synchronized. "
              << "Using local clock fallback for media clock recovery. "
              << "Multi-device sync will not work. "
              << "Integrate real ptpd for production use." << std::endl;
}

void PTPDInterface::stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    state_.isLocked.store(false);
    diagnostics_.isConnected = false;
    diagnostics_.isLocked = false;

    std::cout << "[PTPDInterface] Stopped" << std::endl;
}

PTPState& PTPDInterface::getState() {
    return state_;
}

PTPDiagnostics& PTPDInterface::getDiagnostics() {
    return diagnostics_;
}

} // namespace AES67
