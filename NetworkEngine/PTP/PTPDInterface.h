#ifndef PTPD_INTERFACE_H
#define PTPD_INTERFACE_H

#include <atomic>
#include <memory>
#include <string>
#include "PTPDiagnostics.h"

namespace AES67 {

// Forward declaration
class PTPSlave;

struct PTPState {
    std::atomic<int64_t> masterOffsetNs;   // The difference: PTP Time - Local Time
    std::atomic<double> frequencyDrift;    // Parts per billion drift
    std::atomic<bool> isLocked;            // Whether the PTP clock is synchronized
    std::atomic<int> clockClass;           // PTP clock class
    std::atomic<int> clockAccuracy;        // PTP clock accuracy
    std::atomic<uint64_t> offsetNs;        // Current offset in nanoseconds
};

class PTPDInterface {
public:
    /**
     * Construct the PTP interface.
     * @param useStub If true, operate in stub mode (no network PTP).
     *                If false (default), attempt real IEEE 1588 PTP synchronization.
     */
    explicit PTPDInterface(bool useStub = false);
    ~PTPDInterface();

    bool init(const std::string& interfaceName);
    void start();
    void stop();

    PTPState& getState();

    // Get diagnostic information
    PTPDiagnostics& getDiagnostics();

    // Returns true if running in stub mode (no real PTP synchronization).
    // When true, isLocked/clockClass values are simulated and audio will
    // NOT be synchronized to network PTP time.
    bool isStubMode() const { return stubMode_; }

    // Set PTP domain (default 0, per AES67)
    void setDomain(int domain) { domain_ = domain; }
    int getDomain() const { return domain_; }

private:
    // Called by PTPSlave when new measurements arrive
    void onPTPMeasurement(int64_t offsetNs, int64_t pathDelayNs,
                          double driftPpb, uint8_t clockClass,
                          uint8_t clockAccuracy, bool locked,
                          const std::string& grandmasterID);

    PTPState state_;
    PTPDiagnostics diagnostics_;
    bool running_;
    bool stubMode_;
    int domain_{0};
    std::string interfaceName_;

    // Real PTP slave instance (null in stub mode)
    std::unique_ptr<PTPSlave> ptpSlave_;
};

} // namespace AES67

#endif // PTPD_INTERFACE_H
