//
// PTPClock.h
// AES67 macOS Driver - Build #9
// PTP (IEEE 1588-2008) clock implementation with multi-domain support
// Implements AES67-2018 Section 8.2 Media Clock Recovery
//

#pragma once

#include "../../Shared/Types.h"
#include "PTPDiagnostics.h"
#include <thread>
#include <atomic>
#include <memory>
#include <map>
#include <mutex>
#include <deque>

// Forward declarations (in AES67 namespace, defined later)

namespace AES67 {

// Forward declarations
struct SDPSession;
class PTPDInterface;
class PhaseLockedLoop;

//
// Media Clock Reference Point
// Stores the correlation between RTP timestamp and PTP time
// Per AES67-2018 Section 8.2
//
struct MediaClockReference {
    uint32_t rtpTimestamp;      // RTP timestamp at reference point
    uint64_t ptpTimeNs;         // PTP time in nanoseconds at reference point
    uint64_t localTimeNs;       // Local time when reference was captured
    uint32_t sampleRate;        // Sample rate for this reference
    bool valid;                 // Whether this reference is valid

    MediaClockReference()
        : rtpTimestamp(0), ptpTimeNs(0), localTimeNs(0), sampleRate(48000), valid(false) {}

    MediaClockReference(uint32_t rtp, uint64_t ptp, uint64_t local, uint32_t sr)
        : rtpTimestamp(rtp), ptpTimeNs(ptp), localTimeNs(local), sampleRate(sr), valid(true) {}
};

//
// Local Clock (fallback when PTP not available)
//
class LocalClock {
public:
    LocalClock();

    // Get current time in nanoseconds
    uint64_t getTime() const;

    // Get time in microseconds
    uint64_t getTimeMicroseconds() const;
};

//
// PTP Clock
//
// Single PTP domain clock instance
// Runs ptpd daemon and synchronizes to network master
// Implements media clock recovery per AES67-2018 Section 8.2
//
class PTPClock {
public:
    explicit PTPClock(int domain);
    ~PTPClock();

    // Prevent copy/move
    PTPClock(const PTPClock&) = delete;
    PTPClock& operator=(const PTPClock&) = delete;

    //
    // Control
    //

    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }

    //
    // Time Access
    //

    // Get current PTP time (nanoseconds)
    uint64_t getTime() const;

    // Get PTP time in microseconds
    uint64_t getTimeMicroseconds() const;

    //
    // Media Clock Recovery (AES67-2018 Section 8.2)
    //

    /**
     * Record a media clock reference point
     * Called when an RTP packet arrives with its timestamp and the PTP arrival time
     * @param rtpTimestamp The RTP timestamp from the packet
     * @param ptpArrivalTimeNs The PTP time when packet arrived (nanoseconds)
     * @param sampleRate The sample rate of the stream
     */
    void recordMediaClockReference(uint32_t rtpTimestamp, uint64_t ptpArrivalTimeNs, uint32_t sampleRate);

    /**
     * Convert RTP timestamp to PTP presentation time
     * Uses stored reference points to calculate when audio should be presented
     * @param rtpTimestamp The RTP timestamp to convert
     * @param sampleRate The sample rate of the stream
     * @return PTP time in nanoseconds when this sample should be presented
     */
    uint64_t rtpTimestampToPTPTime(uint32_t rtpTimestamp, uint32_t sampleRate) const;

    /**
     * Get the current clock drift ratio for adaptive resampling
     * Returns the ratio of remote clock rate to local clock rate
     * Value > 1.0 means remote is faster, < 1.0 means remote is slower
     * @return Clock drift ratio (nominally 1.0)
     */
    double getClockDriftRatio() const;

    /**
     * Update PLL with new timing measurement
     * Should be called periodically with timing information
     * @param localTimeNs Local time in nanoseconds
     * @param ptpTimeNs PTP time in nanoseconds
     * @param sampleCount Current audio sample count
     * @param sampleRate Audio sample rate
     */
    void updatePLL(uint64_t localTimeNs, uint64_t ptpTimeNs, uint64_t sampleCount, uint32_t sampleRate);

    /**
     * Check if media clock recovery is locked
     * @return true if we have valid reference points and PLL is locked
     */
    bool isMediaClockLocked() const;

    /**
     * Get the current media clock offset
     * This is the computed offset between local audio clock and PTP media clock
     * @return Offset in nanoseconds
     */
    int64_t getMediaClockOffsetNs() const;

    //
    // Status
    //

    bool isLocked() const;
    int64_t getOffsetNs() const;
    int getDomain() const { return domain_; }

    // Get master clock ID
    std::string getMasterClockID() const;

    /// Full diagnostic snapshot — role, competitor, message counts, not just
    /// the individual fields the getters above expose. What
    /// AES67Device's custom property (Shared/CustomProperties.h) actually
    /// serves to ManagerApp.
    PTPDiagnostics getDiagnostics() const;

    // Get clock quality
    uint8_t getClockClass() const;
    uint8_t getClockAccuracy() const;

    //
    // Network Interface Management
    //

    /**
     * Set network interface for PTP operation
     * Allows runtime override of auto-detected interface
     * @param interfaceName Name of the network interface (e.g., "en0", "en1")
     * @return true if interface was successfully changed, false otherwise
     */
    bool setNetworkInterface(const std::string& interfaceName);

    /**
     * Get current network interface being used for PTP
     * @return Interface name
     */
    std::string getNetworkInterface() const;

private:
    // PTP management thread - not needed anymore with PTPDInterface
    void ptpThread();

    // Calculate nanoseconds per sample for a given sample rate
    static constexpr double nsPerSample(uint32_t sampleRate) {
        return 1000000000.0 / static_cast<double>(sampleRate);
    }

    // Handle RTP timestamp wraparound (32-bit counter)
    int64_t rtpTimestampDifference(uint32_t ts1, uint32_t ts2) const;

    int domain_;
    std::atomic<bool> running_{false};

    // PTPD Interface for actual PTP synchronization
    std::unique_ptr<PTPDInterface> ptpdInterface_;

    // Phase-Locked Loop for audio clock recovery
    std::unique_ptr<PhaseLockedLoop> pll_;

    // Local clock for time calculation
    std::unique_ptr<LocalClock> localClock_;

    // Media clock reference points
    mutable std::mutex mediaClockMutex_;
    MediaClockReference currentReference_;
    MediaClockReference previousReference_;

    // Media clock offset (computed from reference points)
    std::atomic<int64_t> mediaClockOffsetNs_{0};

    // History of reference points for drift calculation
    static constexpr size_t kMaxReferenceHistory = 16;
    std::deque<MediaClockReference> referenceHistory_;

    // Network interface name
    std::string networkInterface_;
    mutable std::mutex interfaceMutex_;
};

//
// PTP Clock Manager
//
// Manages multiple PTP clocks (one per domain)
// Provides unified interface for time access and media clock recovery
//
class PTPClockManager {
public:
    static PTPClockManager& getInstance();

    //
    // Control
    //

    // Enable/disable PTP globally
    void setPTPEnabled(bool enabled);
    bool isPTPEnabled() const { return globalEnabled_.load(); }

    //
    // Clock Management
    //

    // Get or create PTP clock for domain
    std::shared_ptr<PTPClock> getClockForDomain(int domain);

    // Remove clock for domain
    void removeClock(int domain);

    // Get all active domains
    std::vector<int> getActiveDomains() const;

    //
    // Time Access
    //

    // Get time for specific stream (uses stream's PTP domain or fallback)
    uint64_t getTimeForStream(const SDPSession& sdp);

    // Get time for specific domain (or fallback if not available)
    uint64_t getTimeForDomain(int domain);

    // Get fallback local time
    uint64_t getLocalTime() const;

    //
    // Media Clock Recovery
    //

    /**
     * Record a media clock reference for a stream
     * @param domain PTP domain
     * @param rtpTimestamp RTP timestamp from packet
     * @param ptpArrivalTimeNs PTP arrival time
     * @param sampleRate Stream sample rate
     */
    void recordMediaClockReference(int domain, uint32_t rtpTimestamp,
                                   uint64_t ptpArrivalTimeNs, uint32_t sampleRate);

    /**
     * Convert RTP timestamp to presentation time for a domain
     * @param domain PTP domain
     * @param rtpTimestamp RTP timestamp
     * @param sampleRate Stream sample rate
     * @return PTP presentation time in nanoseconds
     */
    uint64_t rtpTimestampToPTPTime(int domain, uint32_t rtpTimestamp, uint32_t sampleRate);

    /**
     * Get clock drift ratio for adaptive resampling
     * @param domain PTP domain
     * @return Drift ratio (1.0 = no drift)
     */
    double getClockDriftRatio(int domain);

    /// Full diagnostic snapshot for a domain, or a default (disconnected,
    /// slave) PTPDiagnostics{} if that domain has no clock instance.
    PTPDiagnostics getDiagnostics(int domain);

private:
    PTPClockManager();
    ~PTPClockManager() = default;

    // Prevent copy/move
    PTPClockManager(const PTPClockManager&) = delete;
    PTPClockManager& operator=(const PTPClockManager&) = delete;

    // Clock instances
    std::map<int, std::shared_ptr<PTPClock>> clocks_;
    mutable std::mutex clocksMutex_;

    // Global enable/disable
    std::atomic<bool> globalEnabled_{true};

    // Fallback clock
    std::unique_ptr<LocalClock> fallbackClock_;
};

} // namespace AES67
