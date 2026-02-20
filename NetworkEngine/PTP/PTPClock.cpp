//
// PTPClock.cpp
// AES67 macOS Driver - Build #9
// Multi-domain PTP clock implementation with graceful fallback
// Implements AES67-2018 Section 8.2 Media Clock Recovery
//

#include "PTPClock.h"
#include "PTPDInterface.h"
#include "PhaseLockedLoop.h"
#include "../NetworkInterfaceDetection.h"
#include "../../Driver/SDPParser.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <cmath>
#include <limits>

namespace AES67 {

// Constants for media clock recovery
namespace {
    // Maximum RTP timestamp difference before considering wraparound
    // RTP timestamps are 32-bit, so half range is 2^31
    constexpr uint32_t kRTPTimestampHalfRange = 0x80000000U;

    // Nanoseconds per second
    constexpr double kNsPerSecond = 1000000000.0;

    // Default sample rate for calculations
    constexpr uint32_t kDefaultSampleRate = 48000;

    // Minimum time between reference points for drift calculation (100ms)
    constexpr uint64_t kMinDriftCalcIntervalNs = 100000000ULL;

    // Maximum expected drift in PPM (parts per million)
    // AES67 allows up to +/- 4.6 ppm for sample rate accuracy
    constexpr double kMaxDriftPPM = 100.0; // Allow wider range for safety
}

//
// LocalClock Implementation
//

LocalClock::LocalClock() {
}

uint64_t LocalClock::getTime() const {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
    return nanos.count();
}

uint64_t LocalClock::getTimeMicroseconds() const {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(duration);
    return micros.count();
}

//
// PTPClock Implementation
//

PTPClock::PTPClock(int domain)
    : domain_(domain)
    , running_(false)
    , localClock_(std::make_unique<LocalClock>())
{
    // Auto-detect the best network interface for PTP
    networkInterface_ = NetworkInterfaceDetection::detectPTPInterface();

    std::cout << "[PTPClock] Domain " << domain << " - Auto-detected network interface: "
              << networkInterface_ << std::endl;

    // Verify the interface is valid
    if (networkInterface_.empty() || !NetworkInterfaceDetection::isInterfaceActive(networkInterface_)) {
        std::cerr << "[PTPClock] Warning: Detected interface '" << networkInterface_
                  << "' is not active, falling back to en0" << std::endl;
        networkInterface_ = "en0";
    }

    // Initialize the PTPD interface for this domain
    ptpdInterface_ = std::make_unique<PTPDInterface>();

    // Initialize the Phase-Locked Loop for audio clock recovery
    pll_ = std::make_unique<PhaseLockedLoop>(1.0, 0.707); // 1Hz bandwidth, critical damping

    // Initialize with the detected network interface
    if (!ptpdInterface_->init(networkInterface_)) {
        std::cerr << "[PTPClock] Failed to initialize PTPD interface on "
                  << networkInterface_ << std::endl;
        // Handle initialization failure
        ptpdInterface_.reset();
    } else {
        std::cout << "[PTPClock] Successfully initialized on interface "
                  << networkInterface_ << " (IP: "
                  << NetworkInterfaceDetection::getInterfaceIPAddress(networkInterface_)
                  << ")" << std::endl;
    }
}

PTPClock::~PTPClock() {
    stop();
}

bool PTPClock::start() {
    if (running_) {
        return false;
    }

    running_ = true;

    // Start the PTPD interface
    if (ptpdInterface_) {
        ptpdInterface_->start();
    }

    return true;
}

void PTPClock::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    // Stop the PTPD interface
    if (ptpdInterface_) {
        ptpdInterface_->stop();
    }
}

uint64_t PTPClock::getTime() const {
    // Get local time
    uint64_t localTime = localClock_->getTime();

    // Apply PTP offset if we have a valid PTPD interface and it's locked
    if (ptpdInterface_) {
        const auto& state = ptpdInterface_->getState();
        if (state.isLocked.load()) {
            // Use the PLL to get a more accurate time correlation
            // In a real implementation, we'd also pass sample count information
            // For now, we'll just use the offset from the PLL

            // Update the PLL with current timing information
            // In a real implementation, this would happen regularly, not on every call
            // uint64_t remoteTime = localTime + state.masterOffsetNs.load();
            // pll_->update(localTime, remoteTime, sampleCount, sampleRate);

            int64_t offset = state.masterOffsetNs.load();
            return localTime + offset;
        }
    }

    return localTime;
}

uint64_t PTPClock::getTimeMicroseconds() const {
    return getTime() / 1000;
}

std::string PTPClock::getMasterClockID() const {
    if (ptpdInterface_) {
        return ptpdInterface_->getDiagnostics().masterClockID;
    }
    return "";
}

uint8_t PTPClock::getClockClass() const {
    if (ptpdInterface_) {
        return static_cast<uint8_t>(ptpdInterface_->getState().clockClass.load());
    }
    return 248; // Default to slave-only clock
}

uint8_t PTPClock::getClockAccuracy() const {
    if (ptpdInterface_) {
        return static_cast<uint8_t>(ptpdInterface_->getState().clockAccuracy.load());
    }
    return 0xFE; // Unknown accuracy
}

bool PTPClock::isLocked() const {
    if (ptpdInterface_) {
        return ptpdInterface_->getState().isLocked.load();
    }
    return false;
}

int64_t PTPClock::getOffsetNs() const {
    if (ptpdInterface_) {
        return ptpdInterface_->getState().masterOffsetNs.load();
    }
    return 0;
}

//
// Media Clock Recovery Implementation (AES67-2018 Section 8.2)
//

void PTPClock::recordMediaClockReference(uint32_t rtpTimestamp, uint64_t ptpArrivalTimeNs, uint32_t sampleRate) {
    std::lock_guard<std::mutex> lock(mediaClockMutex_);

    // Get current local time for correlation
    uint64_t localTimeNs = localClock_->getTime();

    // Create new reference point
    MediaClockReference newRef(rtpTimestamp, ptpArrivalTimeNs, localTimeNs, sampleRate);

    // Move current to previous
    previousReference_ = currentReference_;

    // Update current reference
    currentReference_ = newRef;

    // Add to history for drift calculation
    referenceHistory_.push_back(newRef);
    while (referenceHistory_.size() > kMaxReferenceHistory) {
        referenceHistory_.pop_front();
    }

    // Calculate media clock offset if we have two valid reference points
    if (previousReference_.valid && currentReference_.valid) {
        // Calculate the RTP timestamp difference (handling wraparound)
        int64_t rtpDiff = rtpTimestampDifference(currentReference_.rtpTimestamp,
                                                  previousReference_.rtpTimestamp);

        // Calculate expected PTP time difference based on RTP timestamps
        // Formula: delta_ptp = delta_rtp * (1e9 / sample_rate)
        double expectedPtpDiffNs = static_cast<double>(rtpDiff) * nsPerSample(sampleRate);

        // Calculate actual PTP time difference
        int64_t actualPtpDiffNs = static_cast<int64_t>(currentReference_.ptpTimeNs) -
                                  static_cast<int64_t>(previousReference_.ptpTimeNs);

        // The media clock offset is the difference between expected and actual
        // This helps us track drift over time
        int64_t offsetError = static_cast<int64_t>(expectedPtpDiffNs) - actualPtpDiffNs;

        // Update PLL with timing measurement if we have enough data
        if (pll_ && std::abs(actualPtpDiffNs) > static_cast<int64_t>(kMinDriftCalcIntervalNs)) {
            // Calculate equivalent sample count for PLL update
            uint64_t sampleCount = static_cast<uint64_t>(rtpDiff);
            pll_->update(localTimeNs, ptpArrivalTimeNs, sampleCount, sampleRate);
        }

        // Store the computed offset
        mediaClockOffsetNs_.store(offsetError);
    }
}

uint64_t PTPClock::rtpTimestampToPTPTime(uint32_t rtpTimestamp, uint32_t sampleRate) const {
    std::lock_guard<std::mutex> lock(mediaClockMutex_);

    // If we don't have a valid reference point, fall back to simple calculation
    if (!currentReference_.valid) {
        // Use current PTP time as base and calculate relative offset
        // This is a fallback - not accurate for proper synchronization
        uint64_t currentPtpTime = getTime();

        // Without a reference, we can only estimate based on current time
        // This assumes the RTP timestamp started at some arbitrary point
        return currentPtpTime;
    }

    // Calculate RTP timestamp difference from reference (handling wraparound)
    int64_t rtpDiff = rtpTimestampDifference(rtpTimestamp, currentReference_.rtpTimestamp);

    // Convert RTP timestamp difference to nanoseconds
    // Formula: ptp_time = reference_ptp_time + (rtp_diff * 1e9 / sample_rate)
    double timeDiffNs = static_cast<double>(rtpDiff) * nsPerSample(sampleRate);

    // Apply PLL frequency correction if available
    if (pll_ && pll_->isLocked()) {
        double freqCorrection = pll_->getFrequencyCorrection();
        timeDiffNs *= (1.0 + freqCorrection);
    }

    // Calculate presentation time
    // Note: This gives the media time corresponding to the RTP timestamp
    // The actual presentation time would add the stream's presentation offset
    int64_t presentationTimeNs = static_cast<int64_t>(currentReference_.ptpTimeNs) +
                                  static_cast<int64_t>(timeDiffNs);

    // Ensure we return a valid positive time
    if (presentationTimeNs < 0) {
        // This shouldn't happen in normal operation, but protect against it
        return currentReference_.ptpTimeNs;
    }

    return static_cast<uint64_t>(presentationTimeNs);
}

double PTPClock::getClockDriftRatio() const {
    // Get drift ratio from PLL if available and locked
    if (pll_) {
        if (pll_->isLocked()) {
            // The frequency correction from PLL represents the drift
            // A positive correction means remote clock is faster
            double freqCorrection = pll_->getFrequencyCorrection();

            // Convert to ratio: 1.0 + correction
            // e.g., +0.0001 (100 ppm) means ratio of 1.0001
            double ratio = 1.0 + freqCorrection;

            // Clamp to reasonable bounds
            double maxRatio = 1.0 + (kMaxDriftPPM / 1000000.0);
            double minRatio = 1.0 - (kMaxDriftPPM / 1000000.0);

            return std::max(minRatio, std::min(maxRatio, ratio));
        }
    }

    // If PLL not locked, try to calculate from reference history
    std::lock_guard<std::mutex> lock(mediaClockMutex_);

    if (referenceHistory_.size() >= 2) {
        const auto& oldest = referenceHistory_.front();
        const auto& newest = referenceHistory_.back();

        // Calculate time spans
        int64_t localTimeDiff = static_cast<int64_t>(newest.localTimeNs) -
                                static_cast<int64_t>(oldest.localTimeNs);
        int64_t ptpTimeDiff = static_cast<int64_t>(newest.ptpTimeNs) -
                              static_cast<int64_t>(oldest.ptpTimeNs);

        // Need sufficient time span for accurate calculation
        if (localTimeDiff > static_cast<int64_t>(kMinDriftCalcIntervalNs) && localTimeDiff > 0) {
            double ratio = static_cast<double>(ptpTimeDiff) / static_cast<double>(localTimeDiff);

            // Clamp to reasonable bounds
            double maxRatio = 1.0 + (kMaxDriftPPM / 1000000.0);
            double minRatio = 1.0 - (kMaxDriftPPM / 1000000.0);

            return std::max(minRatio, std::min(maxRatio, ratio));
        }
    }

    // Default: no drift detected
    return 1.0;
}

void PTPClock::updatePLL(uint64_t localTimeNs, uint64_t ptpTimeNs, uint64_t sampleCount, uint32_t sampleRate) {
    if (pll_) {
        pll_->update(localTimeNs, ptpTimeNs, sampleCount, sampleRate);
    }
}

bool PTPClock::isMediaClockLocked() const {
    // Media clock recovery can operate in two modes:
    //
    // 1. Full PTP mode: PTP is locked to a network master, so reference
    //    points correlate RTP timestamps to real PTP time. This gives
    //    multi-device synchronization.
    //
    // 2. Local clock fallback: PTP is not available (stub mode), but we
    //    still have valid reference points correlating RTP timestamps to
    //    local time. This gives single-device timing — enough for audio
    //    to flow correctly through this driver, just not synchronized
    //    with other AES67 devices on the network.
    //
    // We require:
    //   - At least one valid reference point
    //   - The reference is recent (not stale)
    //   - If PTP IS locked, also require PLL lock for full accuracy
    //   - If PTP is NOT locked (stub), accept local-clock-based references

    std::lock_guard<std::mutex> lock(mediaClockMutex_);

    if (!currentReference_.valid) {
        return false;
    }

    // Check if reference is recent (within last 5 seconds)
    uint64_t currentLocalTime = localClock_->getTime();
    uint64_t referenceAge = currentLocalTime - currentReference_.localTimeNs;
    constexpr uint64_t kMaxReferenceAgeNs = 5000000000ULL; // 5 seconds

    if (referenceAge > kMaxReferenceAgeNs) {
        return false;
    }

    // If PTP is locked to a real master, require PLL lock for full accuracy
    if (isLocked()) {
        if (pll_) {
            return pll_->isLocked();
        }
        return true;
    }

    // PTP not locked (stub mode): accept local-clock-based recovery.
    // We have a valid, recent reference point — that's sufficient for
    // single-device operation using the local system clock.
    return true;
}

int64_t PTPClock::getMediaClockOffsetNs() const {
    return mediaClockOffsetNs_.load();
}

int64_t PTPClock::rtpTimestampDifference(uint32_t ts1, uint32_t ts2) const {
    // Handle RTP timestamp wraparound (32-bit counter)
    // Returns ts1 - ts2 with proper wraparound handling

    uint32_t diff = ts1 - ts2;

    // If the unsigned difference is greater than half the range,
    // it means ts1 is actually "before" ts2 (wrapped around)
    if (diff > kRTPTimestampHalfRange) {
        // ts1 wrapped around, so ts1 is actually less than ts2
        // Return negative difference
        return -static_cast<int64_t>(static_cast<uint32_t>(0) - diff);
    }

    return static_cast<int64_t>(diff);
}

void PTPClock::ptpThread() {
    // This method is no longer needed since PTPDInterface handles its own threading
}

bool PTPClock::setNetworkInterface(const std::string& interfaceName) {
    std::lock_guard<std::mutex> lock(interfaceMutex_);

    // Validate interface exists and is active
    if (!NetworkInterfaceDetection::isInterfaceActive(interfaceName)) {
        std::cerr << "[PTPClock] Cannot set interface to '" << interfaceName
                  << "' - interface not found or not active" << std::endl;
        return false;
    }

    // Check if interface supports multicast
    if (!NetworkInterfaceDetection::supportsMulticast(interfaceName)) {
        std::cerr << "[PTPClock] Warning: Interface '" << interfaceName
                  << "' may not support multicast, PTP may not work correctly" << std::endl;
    }

    // Stop current PTP operation
    bool wasRunning = running_;
    if (wasRunning) {
        stop();
    }

    // Update interface
    networkInterface_ = interfaceName;

    // Reinitialize PTPD interface with new network interface
    if (ptpdInterface_) {
        if (!ptpdInterface_->init(networkInterface_)) {
            std::cerr << "[PTPClock] Failed to reinitialize PTPD interface on "
                      << networkInterface_ << std::endl;
            return false;
        }
    }

    // Restart if it was running
    if (wasRunning) {
        start();
    }

    std::cout << "[PTPClock] Successfully switched to interface " << networkInterface_
              << " (IP: " << NetworkInterfaceDetection::getInterfaceIPAddress(networkInterface_)
              << ")" << std::endl;

    return true;
}

std::string PTPClock::getNetworkInterface() const {
    std::lock_guard<std::mutex> lock(interfaceMutex_);
    return networkInterface_;
}

//
// PTPClockManager Implementation
//

PTPClockManager::PTPClockManager()
    : fallbackClock_(std::make_unique<LocalClock>())
{
}

PTPClockManager& PTPClockManager::getInstance() {
    static PTPClockManager instance;
    return instance;
}

void PTPClockManager::setPTPEnabled(bool enabled) {
    globalEnabled_ = enabled;
}

std::shared_ptr<PTPClock> PTPClockManager::getClockForDomain(int domain) {
    std::lock_guard<std::mutex> lock(clocksMutex_);

    auto it = clocks_.find(domain);
    if (it != clocks_.end()) {
        return it->second;
    }

    // Create new clock for this domain
    auto clock = std::make_shared<PTPClock>(domain);
    clock->start();
    clocks_[domain] = clock;

    return clock;
}

void PTPClockManager::removeClock(int domain) {
    std::lock_guard<std::mutex> lock(clocksMutex_);

    auto it = clocks_.find(domain);
    if (it != clocks_.end()) {
        it->second->stop();
        clocks_.erase(it);
    }
}

std::vector<int> PTPClockManager::getActiveDomains() const {
    std::lock_guard<std::mutex> lock(clocksMutex_);

    std::vector<int> domains;
    for (const auto& pair : clocks_) {
        domains.push_back(pair.first);
    }

    return domains;
}

uint64_t PTPClockManager::getTimeForStream(const SDPSession& sdp) {
    if (!globalEnabled_) {
        return fallbackClock_->getTime();
    }

    // Extract PTP domain from SDP session
    int domain = sdp.ptpDomain;

    return getTimeForDomain(domain);
}

uint64_t PTPClockManager::getTimeForDomain(int domain) {
    if (!globalEnabled_) {
        return fallbackClock_->getTime();
    }

    std::lock_guard<std::mutex> lock(clocksMutex_);

    auto it = clocks_.find(domain);
    if (it != clocks_.end()) {
        // Return PTP clock time if locked
        if (it->second->isLocked()) {
            return it->second->getTime();
        }
    }

    // Fallback to local clock if PTP not available or not locked
    return fallbackClock_->getTime();
}

uint64_t PTPClockManager::getLocalTime() const {
    return fallbackClock_->getTime();
}

//
// PTPClockManager Media Clock Recovery Methods
//

void PTPClockManager::recordMediaClockReference(int domain, uint32_t rtpTimestamp,
                                                 uint64_t ptpArrivalTimeNs, uint32_t sampleRate) {
    if (!globalEnabled_) {
        return;
    }

    auto clock = getClockForDomain(domain);
    if (clock) {
        clock->recordMediaClockReference(rtpTimestamp, ptpArrivalTimeNs, sampleRate);
    }
}

uint64_t PTPClockManager::rtpTimestampToPTPTime(int domain, uint32_t rtpTimestamp, uint32_t sampleRate) {
    if (!globalEnabled_) {
        // Fallback: use local time
        return fallbackClock_->getTime();
    }

    std::lock_guard<std::mutex> lock(clocksMutex_);

    auto it = clocks_.find(domain);
    if (it != clocks_.end()) {
        return it->second->rtpTimestampToPTPTime(rtpTimestamp, sampleRate);
    }

    // Domain not found, return local time as fallback
    return fallbackClock_->getTime();
}

double PTPClockManager::getClockDriftRatio(int domain) {
    if (!globalEnabled_) {
        return 1.0; // No drift compensation when PTP disabled
    }

    std::lock_guard<std::mutex> lock(clocksMutex_);

    auto it = clocks_.find(domain);
    if (it != clocks_.end()) {
        return it->second->getClockDriftRatio();
    }

    // Domain not found, return nominal ratio
    return 1.0;
}

} // namespace AES67
