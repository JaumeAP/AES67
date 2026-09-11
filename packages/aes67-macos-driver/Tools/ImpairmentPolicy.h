//
// ImpairmentPolicy.h
// AES67 macOS Driver
//
// What AES67ImpairmentRelay does to a packet, separated from the sockets that
// carry it: the loss draw, the blackout window, and the due time that turns a
// delay, a jitter and a reordering into an order of departure.
//
// Separate because the relay's claim -- that it can reorder a stream -- was
// only ever checked by watching a receiver's counters, and those move for
// jitter too. Here the decision is a value: given a seed and a pattern of
// arrivals, the packets leave in an order a test can assert.
//
#pragma once

#include <cstdint>
#include <random>

namespace AES67 {
namespace Impairment {

/// The default head start, in milliseconds, and why it is not exactly one
/// packet time: a packet moved ahead by exactly the interval between packets
/// lands on its predecessor's due time, not in front of it, and a queue that
/// breaks ties by arrival order then sends the two in the order they came.
/// With 1 ms packets that made --reorder do nothing at all unless jitter
/// happened to break the tie. Half a packet time more is enough to overtake
/// one packet and not enough to overtake two.
inline constexpr double kDefaultReorderHeadStartMs = 1.5;

/// The degradation to apply, in the units the command line takes.
struct Settings {
    double lossPercent{0.0};      // uniform loss, 0-100
    int    burstMs{0};            // length of a total blackout, 0 = off
    int    burstEveryMs{10000};   // period between blackout starts
    double delayMs{0.0};          // fixed extra delay on every packet
    double jitterMs{0.0};         // uniform +/- jitter around that delay
    double reorderPercent{0.0};   // packets given a head start on the queue
    double reorderHeadStartMs{kDefaultReorderHeadStartMs};  // how big that head start is
};

/// What happens to one packet.
struct Decision {
    bool   droppedByLoss{false};
    bool   droppedByBurst{false};
    bool   reordered{false};
    double offsetMs{0.0};         // how long after arrival it should leave

    bool forwarded() const { return !droppedByLoss && !droppedByBurst; }
};


/// The policy itself. Deterministic for a given seed and sequence of calls,
/// which is what makes the relay's behaviour testable at all.
class Policy {
public:
    Policy(const Settings& settings, unsigned seed)
        : settings_(settings), rng_(seed), draw_(0.0, 100.0),
          jitter_(-settings.jitterMs, settings.jitterMs) {}

    /// `elapsedMs` is time since the relay started, which is what the
    /// blackout window is measured against.
    Decision decide(int64_t elapsedMs) {
        Decision decision;

        // A blackout wins over uniform loss: inside the window nothing gets
        // through at all, and the draw is not spent on it.
        if (settings_.burstMs > 0 && settings_.burstEveryMs > 0 &&
            (elapsedMs % settings_.burstEveryMs) < settings_.burstMs) {
            decision.droppedByBurst = true;
            return decision;
        }

        if (settings_.lossPercent > 0.0 && draw_(rng_) < settings_.lossPercent) {
            decision.droppedByLoss = true;
            return decision;
        }

        double offsetMs = settings_.delayMs;
        if (settings_.jitterMs > 0.0) offsetMs += jitter_(rng_);
        if (settings_.reorderPercent > 0.0 && draw_(rng_) < settings_.reorderPercent) {
            offsetMs -= settings_.reorderHeadStartMs;
            decision.reordered = true;
        }
        // The relay has no time machine: a packet cannot leave before it
        // arrived, however far back the jitter or the reordering reaches.
        decision.offsetMs = offsetMs < 0.0 ? 0.0 : offsetMs;
        return decision;
    }

private:
    Settings settings_;
    std::mt19937 rng_;
    std::uniform_real_distribution<double> draw_;
    std::uniform_real_distribution<double> jitter_;
};

} // namespace Impairment
} // namespace AES67
