#ifndef PTPD_INTERFACE_H
#define PTPD_INTERFACE_H

#include <atomic>
#include <string>
#include "PTPDiagnostics.h"

namespace AES67 {

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
    PTPDInterface();
    ~PTPDInterface();

    bool init(const std::string& interfaceName);
    void start();
    void stop();

    PTPState& getState();
    
    // Get diagnostic information
    PTPDiagnostics& getDiagnostics();

private:
    PTPState state_;
    PTPDiagnostics diagnostics_;
    bool running_;
    std::string interfaceName_;

    // Internal ptpd structures (opaque to the outside)
    void* ptpdInstance_;
};

} // namespace AES67

#endif // PTPD_INTERFACE_H