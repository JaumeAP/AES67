//
// PhcClock.h
// AES67 Linux PTP daemon
// The NIC's own clock, as the source a grandmaster serves.
//
// On a Raspberry Pi 5 the ethernet controller keeps a hardware clock the
// kernel exposes as /dev/ptpN, and it is the same clock that stamps the
// packets. Reading the system clock instead would mean announcing a time that
// is not the one the timestamps are taken against, which is the whole error
// this daemon exists to avoid.
//
// The interface it implements is packages/aes67-core's PTPClockSource: what
// the macOS side already defines for exactly this, so a clock is a clock on
// either platform.
//
#pragma once

#include "NetworkEngine/PTP/PTPClockSource.h"

#include <ctime>
#include <string>

namespace AES67::LinuxPtpd {

class PhcClock : public PTPClockSource {
public:
    PhcClock() = default;
    ~PhcClock() override;

    PhcClock(const PhcClock&) = delete;
    PhcClock& operator=(const PhcClock&) = delete;

    /// Opens /dev/ptpN. `device` empty means the one the interface reports
    /// through ethtool, which is the normal case: the caller passes the
    /// interface name and this finds the clock behind it.
    bool open(const std::string& interfaceName, const std::string& device,
              std::string& error);

    /// True once open() succeeded. A closed clock reports the system clock
    /// and says so; it is never silently substituted.
    bool isHardware() const { return fd_ >= 0; }

    /// The device actually opened, for the log line that says which.
    const std::string& devicePath() const { return devicePath_; }

    /// The open descriptor, for the ioctls that ask the clock for things
    /// clock_gettime does not cover -- its capabilities, and its external
    /// timestamp channels.
    int descriptor() const { return fd_; }

    /// The same clock as a clockid_t, which is what clock_adjtime steers.
    clockid_t clockId() const;

    // PTPClockSource.
    uint64_t currentTimeNs() const override;
    uint8_t clockClass() const override { return clockClass_; }
    PTPClockAccuracy clockAccuracy() const override { return accuracy_; }
    std::string name() const override;

    /// What to announce about this clock. A free-running NIC oscillator is
    /// clockClass 248 and accuracy unknown, which is what IEEE 1588 sec 7.6.2.4
    /// says a clock not synchronised to a primary reference is; overriding it
    /// is for the case where something better disciplines the PHC.
    void setAnnouncedQuality(uint8_t clockClass, PTPClockAccuracy accuracy);

private:
    int fd_ = -1;
    std::string devicePath_;
    uint8_t clockClass_ = 248;
    PTPClockAccuracy accuracy_ = PTPClockAccuracy::Unknown;
};

}  // namespace AES67::LinuxPtpd
