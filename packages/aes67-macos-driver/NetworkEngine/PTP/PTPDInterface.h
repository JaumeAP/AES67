#ifndef PTPD_INTERFACE_H
#define PTPD_INTERFACE_H

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "PTPArbitrator.h"
#include "NetworkEngine/PTP/PTPSettingsMapping.h"
#include "NetworkEngine/PTP/PTPDiagnostics.h"
#include "PTPService.h"

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

    /// The dataset the installation configured. Call before init(): it is
    /// read when the engines are built. Absent a call, every value stays
    /// where the code had it.
    void setSettings(const PTPMasterSettings& settings) { settings_ = settings; }

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

    // --- Shared PTP daemon -------------------------------------------------
    //
    // When aes67ptpd is running, its status socket is read instead of
    // starting a second PTP engine in this process: one engine per host,
    // surviving plugin reloads, shared with anything else that wants the
    // measurement. Absent socket, absent daemon: the in-process PTPSlave path
    // runs exactly as it did before.
    void setServiceSocketPath(const std::string& path) { servicePath_ = path; }
    const std::string& getServiceSocketPath() const { return servicePath_; }

    // Call before init() to refuse the daemon and keep the in-process path.
    void setPreferPrivilegedDaemon(bool prefer) { preferDaemon_ = prefer; }

    // True once init() has decided to read from the daemon.
    bool isUsingPrivilegedDaemon() const { return serviceClient_ != nullptr; }

    // Set PTP domain (default 0, per AES67)
    void setDomain(int domain) { domain_ = domain; }
    int getDomain() const { return domain_; }

    // --- Master capability (opt-in) ---------------------------------------
    //
    // By default this class is exactly what it always was: a slave-only
    // PTPSlave wrapper. Calling this before init() switches it to a
    // PTPArbitrator instead — BMCA-driven, capable of becoming grandmaster
    // when nothing better is on the wire, falling back to slave when there
    // is. See PTPArbitrator.h.

    /// Must be called before init(). clockSourceKind/lockToDeviceID pick
    /// what a Master role would advertise as its own clock (see
    /// PTPArbitratorConfig) — irrelevant if BMCA never elects us master.
    void enableMasterCapability(PTPClockSourceKind clockSourceKind,
                                 AudioDeviceID lockToDeviceID = kAudioObjectUnknown);

    bool isMasterCapable() const { return masterCapable_; }

    /// Valid once running: Master or Slave. Meaningless in stub mode or
    /// before start().
    PTPRole getRole() const;

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

    // Real PTP slave instance (null in stub mode, and null when master-capable)
    std::unique_ptr<PTPSlave> ptpSlave_;

    // Master-capable path (null unless enableMasterCapability() was called)
    bool masterCapable_{false};
    PTPMasterSettings settings_{};
    PTPClockSourceKind masterClockSourceKind_{PTPClockSourceKind::Internal};
    AudioDeviceID masterLockToDeviceID_{kAudioObjectUnknown};
    std::unique_ptr<PTPArbitrator> ptpArbitrator_;

    // Reader for the privileged daemon's status socket, and the thread that
    // turns its statuses into measurements for the rest of the driver.
    void serviceLoop();
    std::unique_ptr<PTPServiceClient> serviceClient_;
    std::thread serviceThread_;
    std::atomic<bool> serviceRunning_{false};
    std::string servicePath_{kPTPServiceSocketPath};
    bool preferDaemon_{true};
    uint32_t lastServiceSequence_{0};
};

} // namespace AES67

#endif // PTPD_INTERFACE_H
