#include "ExternalReference.h"

#include "PhcClock.h"
#include "ReferenceMath.h"

#include <linux/ptp_clock.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/timex.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace AES67::LinuxPtpd {
namespace {

/// clock_adjtime carries frequency as parts per million scaled by 2^16, and
/// the servo works in nanoseconds per second, which is parts per billion.
long frequencyFieldFor(double nanosecondsPerSecond) {
    const double scaled = nanosecondsPerSecond * 65536.0 / 1000.0;
    return static_cast<long>(std::llround(scaled));
}

/// An edge further from the second than this is not a reference edge: it is a
/// glitch on the line, or the clock has been stepped underneath us. Half a
/// millisecond is far wider than any oscillator drifts between two edges and
/// far narrower than the fold of offsetFromSecondBoundary.
constexpr int64_t kCredibleOffsetNs = 500000;

}  // namespace

ExternalReference::~ExternalReference() {
    if (!enabled_ || clock_ == nullptr) return;

    // Leave the channel as it was found: another process asking for the same
    // one after this exits should not inherit our request.
    struct ptp_extts_request request {};
    request.index = channel_;
    request.flags = 0;
    ::ioctl(clock_->descriptor(), PTP_EXTTS_REQUEST2, &request);
}

bool ExternalReference::enable(PhcClock& clock, unsigned int channel, std::string& error) {
    if (!clock.isHardware()) {
        error = "no PTP hardware clock to stamp the reference against";
        return false;
    }

    struct ptp_clock_caps caps {};
    if (::ioctl(clock.descriptor(), PTP_CLOCK_GETCAPS, &caps) != 0) {
        error = std::string("PTP_CLOCK_GETCAPS: ") + std::strerror(errno);
        return false;
    }
    if (caps.n_ext_ts == 0) {
        error = clock.devicePath() +
                " has no external timestamp channel: this NIC stamps packets but "
                "cannot stamp an input, so the reference cannot be measured "
                "against the clock that stamps";
        return false;
    }
    if (channel >= static_cast<unsigned int>(caps.n_ext_ts)) {
        error = clock.devicePath() + " has " + std::to_string(caps.n_ext_ts) +
                " external timestamp channels; asked for index " +
                std::to_string(channel);
        return false;
    }

    struct ptp_extts_request request {};
    request.index = channel;
    request.flags = PTP_ENABLE_FEATURE | PTP_RISING_EDGE;
    if (::ioctl(clock.descriptor(), PTP_EXTTS_REQUEST2, &request) != 0) {
        error = std::string("PTP_EXTTS_REQUEST2: ") + std::strerror(errno);
        return false;
    }

    clock_ = &clock;
    channel_ = channel;
    enabled_ = true;
    status_.available = true;

    // The gains and the bounds are the box's, which are upstream's, chosen
    // rather than inherited: see the Teensy firmware's src/main.cpp and
    // COMPARATIVA-SERVO.md. A word clock through a divider is the same kind
    // of reference here as it is there.
    tuning_.kp = 1.0;
    tuning_.ki = 0.5;
    tuning_.maxDriftNsps = 100000.0;   // 100 ppm: a bigger jump is not drift
    tuning_.lockThresholdNs = 100;
    return true;
}

void ExternalReference::service() {
    if (!enabled_ || clock_ == nullptr) return;

    struct pollfd waiting {};
    waiting.fd = clock_->descriptor();
    waiting.events = POLLIN;

    while (::poll(&waiting, 1, 0) > 0) {
        struct ptp_extts_event event {};
        const ssize_t read = ::read(clock_->descriptor(), &event, sizeof(event));
        if (read != static_cast<ssize_t>(sizeof(event))) break;
        if (event.index != channel_) continue;

        const uint64_t edgeNs = static_cast<uint64_t>(event.t.sec) *
                                    static_cast<uint64_t>(kNanosecondsPerSecond) +
                                static_cast<uint64_t>(event.t.nsec);
        ++status_.edges;

        const int64_t offsetNs = offsetFromSecondBoundary(edgeNs);
        status_.offsetNs = offsetNs;

        if (offsetNs > kCredibleOffsetNs || offsetNs < -kCredibleOffsetNs) {
            // Not a reference edge. Counting it would let one glitch throw the
            // integrator further than a minute of good edges can pull it back.
            ++status_.rejected;
            consecutiveInWindow_ = 0;
            haveLastEdge_ = false;
            continue;
        }

        if (!haveLastEdge_) {
            lastEdgeNs_ = edgeNs;
            haveLastEdge_ = true;
            continue;
        }

        // The reference advances exactly one second between edges by
        // definition; the local clock advances whatever it advanced.
        const t41ptp::NanoTime localDiff =
            static_cast<t41ptp::NanoTime>(edgeNs - lastEdgeNs_);
        lastEdgeNs_ = edgeNs;

        const t41ptp::ServoOutcome outcome =
            t41ptp::servoUpdate(servo_, tuning_, kNanosecondsPerSecond, localDiff, offsetNs);
        apply(outcome);

        status_.driftNsps = servo_.driftNsps;

        if (offsetNs <= tuning_.lockThresholdNs && offsetNs >= -tuning_.lockThresholdNs) {
            ++consecutiveInWindow_;
        } else {
            consecutiveInWindow_ = 0;
        }
        // Ten edges inside the window before saying locked, and one outside
        // to stop saying it. Announcing a lock is a claim other devices act
        // on; taking it back has to be the fast direction.
        status_.locked = consecutiveInWindow_ >= 10;
    }
}

void ExternalReference::apply(const t41ptp::ServoOutcome& outcome) {
    if (clock_ == nullptr) return;

    if (outcome.stepClock) {
        struct timex adjustment {};
        adjustment.modes = ADJ_SETOFFSET | ADJ_NANO;
        // A step moves the clock towards the reference, so it is the negative
        // of the offset the reference sits at.
        const int64_t correction = -outcome.offsetCorrectionNs;
        adjustment.time.tv_sec = correction / kNanosecondsPerSecond;
        adjustment.time.tv_usec = static_cast<long>(correction % kNanosecondsPerSecond);
        if (adjustment.time.tv_usec < 0) {
            adjustment.time.tv_sec -= 1;
            adjustment.time.tv_usec += kNanosecondsPerSecond;
        }
        if (::clock_adjtime(clock_->clockId(), &adjustment) < 0) {
            std::fprintf(stderr, "[ptpd] stepping the PHC: %s\n", std::strerror(errno));
        }
        return;
    }

    if (outcome.adjustFrequency) {
        struct timex adjustment {};
        adjustment.modes = ADJ_FREQUENCY;
        adjustment.freq = frequencyFieldFor(outcome.freqAdjustNsps);
        if (::clock_adjtime(clock_->clockId(), &adjustment) < 0) {
            std::fprintf(stderr, "[ptpd] steering the PHC: %s\n", std::strerror(errno));
        }
    }
}

}  // namespace AES67::LinuxPtpd
