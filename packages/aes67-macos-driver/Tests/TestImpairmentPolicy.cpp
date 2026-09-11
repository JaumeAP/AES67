//
// TestImpairmentPolicy.cpp
// AES67 macOS Driver
//
// AES67ImpairmentRelay's degradation, checked as arithmetic rather than by
// watching a receiver's counters.
//
// The relay is what stresses the receive path without root: loss, blackout
// bursts, delay, jitter and reordering, applied in user space to a live
// stream. Its reordering had never been proved -- in every run so far the
// counter that moved was the receiver's, and jitter moves that one too. What
// this suite asserts is the part that decides: for a seed and a pattern of
// arrivals, which packets are dropped and in what order the rest leave.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Tools/ImpairmentPolicy.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using AES67::Impairment::Decision;
using AES67::Impairment::Policy;
using AES67::Impairment::Settings;

namespace {

/// One packet through the relay: when it arrived, and when it is due out.
struct Departure {
    int64_t arrivalMs{0};
    double  dueMs{0.0};
    size_t  index{0};
    bool    reordered{false};
};

/// Run `count` packets one millisecond apart through a policy and return the
/// ones that survive, in the order the relay would send them: by due time,
/// arrival order breaking ties, which is exactly what the relay's queue does.
std::vector<Departure> departures(const Settings& settings, unsigned seed, size_t count) {
    Policy policy(settings, seed);
    std::vector<Departure> queued;

    for (size_t i = 0; i < count; ++i) {
        const int64_t arrival = static_cast<int64_t>(i);
        const Decision decision = policy.decide(arrival);
        if (!decision.forwarded()) continue;
        queued.push_back(Departure{arrival,
                                   static_cast<double>(arrival) + decision.offsetMs,
                                   i, decision.reordered});
    }

    std::stable_sort(queued.begin(), queued.end(),
                     [](const Departure& a, const Departure& b) { return a.dueMs < b.dueMs; });
    return queued;
}

/// How many packets leave before one that arrived earlier.
size_t inversions(const std::vector<Departure>& order) {
    size_t count = 0;
    for (size_t i = 1; i < order.size(); ++i) {
        if (order[i].index < order[i - 1].index) ++count;
    }
    return count;
}

} // namespace

TEST_CASE("An untouched stream keeps its order and every packet") {
    Settings settings;   // no loss, no delay, no jitter, no reordering
    const std::vector<Departure> order = departures(settings, 1, 100);

    CHECK(order.size() == 100);
    CHECK(inversions(order) == 0);
    for (const Departure& departure : order) {
        CHECK(departure.dueMs == static_cast<double>(departure.arrivalMs));
    }
}

TEST_CASE("A fixed delay moves every packet by the same amount and reorders nothing") {
    Settings settings;
    settings.delayMs = 5.0;
    const std::vector<Departure> order = departures(settings, 1, 100);

    CHECK(order.size() == 100);
    CHECK(inversions(order) == 0);
    for (const Departure& departure : order) {
        CHECK(departure.dueMs == static_cast<double>(departure.arrivalMs) + 5.0);
    }
}

TEST_CASE("Reordering puts a packet in front of the one before it") {
    Settings settings;
    settings.delayMs = 5.0;            // room for a packet to move earlier
    settings.reorderPercent = 20.0;
    const std::vector<Departure> order = departures(settings, 7, 500);

    // Nothing is lost: reordering is not loss.
    CHECK(order.size() == 500);

    const size_t marked = static_cast<size_t>(
        std::count_if(order.begin(), order.end(),
                      [](const Departure& d) { return d.reordered; }));

    // Roughly the share asked for, and the stream really does come out of
    // order. Not one inversion per marked packet: two marked packets in a row
    // move by the same amount and so keep their order between them, which is
    // why the exact count is bounded rather than equal.
    CHECK(marked > 60);
    CHECK(marked < 140);
    CHECK(inversions(order) > 0);
    CHECK(inversions(order) <= marked);

    // A marked packet overtakes the packet in front of it, and only that
    // one: a head start of one and a half packet times, never two.
    for (size_t i = 1; i < order.size(); ++i) {
        if (order[i].index >= order[i - 1].index) continue;
        // The pair reads (the overtaker, the overtaken): the marked packet
        // left first, and the one that arrived before it comes next.
        CHECK(order[i - 1].reordered);
        CHECK(order[i - 1].index == order[i].index + 1);
    }

    for (const Departure& departure : order) {
        const double expected = static_cast<double>(departure.arrivalMs) +
                                (departure.reordered ? 3.5 : 5.0);
        CHECK(departure.dueMs == expected);
    }
}

TEST_CASE("Without delay there is nothing to move a packet in front of") {
    // A reordered packet cannot leave before it arrived, so with no delay to
    // borrow from it goes out immediately -- still in order. This is the case
    // that made the relay's counter and the receiver's disagree.
    Settings settings;
    settings.reorderPercent = 50.0;
    const std::vector<Departure> order = departures(settings, 3, 200);

    CHECK(order.size() == 200);
    CHECK(inversions(order) == 0);
    for (const Departure& departure : order) {
        CHECK(departure.dueMs == static_cast<double>(departure.arrivalMs));
    }
}

TEST_CASE("Uniform loss drops about what it is asked to") {
    Settings settings;
    settings.lossPercent = 10.0;
    const std::vector<Departure> order = departures(settings, 11, 10000);

    CHECK(order.size() > 8800);
    CHECK(order.size() < 9200);
    CHECK(inversions(order) == 0);     // loss is not reordering
}

TEST_CASE("A burst drops everything inside its window and nothing outside") {
    Settings settings;
    settings.burstMs = 20;
    settings.burstEveryMs = 100;
    const std::vector<Departure> order = departures(settings, 5, 1000);

    // Ten windows of twenty milliseconds in a thousand packets one
    // millisecond apart.
    CHECK(order.size() == 800);
    for (const Departure& departure : order) {
        CHECK((departure.arrivalMs % 100) >= 20);
    }
}

TEST_CASE("A burst does not spend the loss the rest of the stream is owed") {
    // The blackout returns before the loss draw is made, so the packets
    // outside the windows still meet the loss rate they were configured with
    // rather than a rate diluted by the ones the burst already took.
    Settings settings;
    settings.lossPercent = 25.0;
    settings.burstMs = 10;
    settings.burstEveryMs = 100;

    Policy policy(settings, 42);
    size_t outsideWindows = 0, lostOutsideWindows = 0, insideWindows = 0;

    for (int64_t ms = 0; ms < 20000; ++ms) {
        const Decision decision = policy.decide(ms);
        if ((ms % 100) < 10) {
            CHECK(decision.droppedByBurst);
            ++insideWindows;
            continue;
        }
        CHECK_FALSE(decision.droppedByBurst);
        ++outsideWindows;
        if (decision.droppedByLoss) ++lostOutsideWindows;
    }

    CHECK(insideWindows == 2000);
    CHECK(outsideWindows == 18000);
    const double lossRate = 100.0 * static_cast<double>(lostOutsideWindows) /
                            static_cast<double>(outsideWindows);
    CHECK(lossRate > 23.0);
    CHECK(lossRate < 27.0);
}

TEST_CASE("Jitter stays inside the range it was given") {
    Settings settings;
    settings.delayMs = 10.0;
    settings.jitterMs = 4.0;
    const std::vector<Departure> order = departures(settings, 9, 2000);

    CHECK(order.size() == 2000);
    bool sawEarly = false, sawLate = false;
    for (const Departure& departure : order) {
        const double offset = departure.dueMs - static_cast<double>(departure.arrivalMs);
        CHECK(offset >= 6.0);
        CHECK(offset <= 14.0);
        if (offset < 10.0) sawEarly = true;
        if (offset > 10.0) sawLate = true;
    }
    CHECK(sawEarly);
    CHECK(sawLate);

    // Jitter alone reorders: that is the whole point of it, and it is why a
    // receiver's out-of-order count cannot stand as proof that --reorder
    // works.
    CHECK(inversions(order) > 0);
}
