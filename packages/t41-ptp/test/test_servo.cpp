//
// test_servo.cpp
// The servo's decision, on a host.
//
// What the servo does used to be reachable only through PTPBase, which
// means only with a Teensy on the desk: the arithmetic and the calls into
// EthernetIEEE1588 were the same function. Split out, the decision is a
// function over plain numbers, and these are the modes, the bounds and the
// lock counting checked directly rather than inferred from what the clock
// was seen to do.
//
#include "ptp/ptp-servo.h"
#include "test_harness.h"

#include <cmath>

namespace {

using t41ptp::NanoTime;
using t41ptp::ServoMode;
using t41ptp::ServoOutcome;
using t41ptp::ServoState;
using t41ptp::ServoTuning;

bool near(double a, double b, double tolerance = 1e-6)
{
    return std::fabs(a - b) <= tolerance;
}

constexpr NanoTime NS_PER_SECOND_VALUE = 1000000000;

// A second of reference time and a second of local time: no drift at all,
// so the mode is decided by the offset alone.
ServoOutcome noDrift(ServoState &state, const ServoTuning &tuning, NanoTime offset)
{
    return t41ptp::servoUpdate(state, tuning, NS_PER_SECOND_VALUE, NS_PER_SECOND_VALUE, offset);
}

} // namespace

// The reference clock has to have moved for there to be a measurement:
// two equal timestamps give 0/0, and every comparison with a NaN is
// false, so the drift guard let it through as a valid drift.
static void testAnUnmovedReferenceIsNotAMeasurement()
{
    ServoState state;
    ServoTuning tuning;
    state.lockCount = 4;
    state.driftNsps = 25;
    state.offsetAccumulator = 300;

    const ServoOutcome outcome = t41ptp::servoUpdate(state, tuning, 0, 1000, 0);
    CHECK(outcome.mode == ServoMode::NoMeasurement);
    CHECK(!outcome.adjustFrequency);
    CHECK(!outcome.stepClock);
    CHECK_EQ(outcome.offsetCorrectionNs, 0);

    // And nothing carried between measurements was touched, the lock
    // included: there was no measurement to lose it over.
    CHECK_EQ(state.lockCount, 4);
    CHECK(near(state.driftNsps, 25));
    CHECK(near(state.offsetAccumulator, 300));
}

// A master implying more drift than a crystal could produce has been
// stepped, and is not a rate to chase.
static void testDriftPastWhatACrystalCanDoIsRefused()
{
    ServoState state;
    ServoTuning tuning;
    state.lockCount = 7;

    // Local time running at half the reference: 500000000 ns/s of drift.
    const ServoOutcome outcome =
        t41ptp::servoUpdate(state, tuning, NS_PER_SECOND_VALUE, NS_PER_SECOND_VALUE / 2, 0);
    CHECK(outcome.mode == ServoMode::DriftError);
    CHECK(!outcome.adjustFrequency);
    CHECK(!outcome.stepClock);
    CHECK_EQ(state.lockCount, 0);
    CHECK(near(state.driftNsps, 0));
}

static void testFrequencyModeTakesTheMeasuredRate()
{
    ServoState state;
    ServoTuning tuning;

    // A hundredth of a percent fast: 10000 ns/s, past the 1000 that puts
    // the servo in frequency mode and inside what it will act on.
    const NanoTime local = NS_PER_SECOND_VALUE - 10000;
    const ServoOutcome outcome =
        t41ptp::servoUpdate(state, tuning, NS_PER_SECOND_VALUE, local, 0);

    CHECK(outcome.mode == ServoMode::Frequency);
    CHECK(outcome.adjustFrequency);
    CHECK(!outcome.stepClock);
    CHECK(near(outcome.measuredDriftNsps, 10000, 1.0));
    CHECK(near(outcome.freqAdjustNsps, state.driftNsps));
    // The offset is not corrected in this mode, so the local timestamp
    // does not move.
    CHECK_EQ(outcome.offsetCorrectionNs, 0);

    // The gain damps the step: half of the measured rate error, taken
    // twice, is what a whole one would have taken once.
    ServoState damped;
    ServoTuning half = tuning;
    half.kf = 0.5;
    t41ptp::servoUpdate(damped, half, NS_PER_SECOND_VALUE, local, 0);
    CHECK(near(damped.driftNsps, state.driftNsps / 2, 1.0));
}

// The term is bounded where it is kept. The old fmod against a thousand
// million never fired for any value an oscillator produces, so it grew
// without limit and the state itself stopped meaning anything.
static void testTheFrequencyTermIsHeldAtItsBound()
{
    ServoState state;
    ServoTuning tuning;

    const NanoTime local = NS_PER_SECOND_VALUE - 90000;  // 90000 ns/s, inside the drift guard
    for (int i = 0; i < 20; i++)
    {
        t41ptp::servoUpdate(state, tuning, NS_PER_SECOND_VALUE, local, 0);
    }
    CHECK(near(state.driftNsps, tuning.maxFreqAdjustNsps, 1.0));

    // And at the bottom too.
    ServoState below;
    const NanoTime fast = NS_PER_SECOND_VALUE + 90000;
    for (int i = 0; i < 20; i++)
    {
        t41ptp::servoUpdate(below, tuning, NS_PER_SECOND_VALUE, fast, 0);
    }
    CHECK(near(below.driftNsps, -tuning.maxFreqAdjustNsps, 1.0));
}

static void testACoarseOffsetStepsTheClock()
{
    ServoState state;
    ServoTuning tuning;
    state.offsetAccumulator = 5000;
    state.lockCount = 3;

    const ServoOutcome outcome = noDrift(state, tuning, 2000);
    CHECK(outcome.mode == ServoMode::Coarse);
    CHECK(outcome.stepClock);
    CHECK(!outcome.adjustFrequency);
    CHECK_EQ(outcome.offsetCorrectionNs, -2000);

    // The integral term goes with the step: what it accumulated was
    // measured against a clock that has just moved.
    CHECK(near(state.offsetAccumulator, 0));
    CHECK_EQ(state.lockCount, 0);
}

static void testFineModeIsTheSumOfTheThreeTerms()
{
    ServoState state;
    ServoTuning tuning;
    state.driftNsps = 40;

    const ServoOutcome outcome = noDrift(state, tuning, 200);
    CHECK(outcome.mode == ServoMode::Fine);
    CHECK(outcome.adjustFrequency);
    CHECK(!outcome.stepClock);
    CHECK_EQ(outcome.offsetCorrectionNs, -200);

    CHECK(near(outcome.freqTermNsps, 40));
    CHECK(near(outcome.proportionalTermNsps, -200 * tuning.kp));
    CHECK(near(outcome.integralTermNsps, -200 * tuning.ki));
    CHECK(near(outcome.freqAdjustNsps, outcome.freqTermNsps + outcome.proportionalTermNsps +
                                           outcome.integralTermNsps));
}

// Anti-windup: the accumulator is held where its own contribution reaches
// the largest correction that will ever be applied. Past that it only
// stores a debt that has to be paid back before the clock can move the
// other way.
static void testTheIntegralTermIsHeldAtItsBound()
{
    ServoState state;
    ServoTuning tuning;

    for (int i = 0; i < 2000; i++)
    {
        noDrift(state, tuning, 900);
    }
    const double limit = tuning.maxFreqAdjustNsps / tuning.ki;
    CHECK(near(state.offsetAccumulator, -limit, 1.0));

    ServoState above;
    for (int i = 0; i < 2000; i++)
    {
        noDrift(above, tuning, -900);
    }
    CHECK(near(above.offsetAccumulator, limit, 1.0));

    // A zero gain has no term to bound, and nothing to accumulate into.
    ServoState noIntegral;
    ServoTuning openLoop = tuning;
    openLoop.ki = 0.0;
    for (int i = 0; i < 10; i++)
    {
        noDrift(noIntegral, openLoop, 900);
    }
    CHECK(near(noIntegral.offsetAccumulator, 0));
}

static void testTheLockCountsWhileTheOffsetStaysInside()
{
    ServoState state;
    ServoTuning tuning;

    for (int i = 0; i < 5; i++)
    {
        noDrift(state, tuning, 50);
    }
    CHECK_EQ(state.lockCount, 5);

    // On the edge of the window is outside it: the test is strict.
    noDrift(state, tuning, tuning.lockThresholdNs);
    CHECK_EQ(state.lockCount, 0);

    for (int i = 0; i < 3; i++)
    {
        noDrift(state, tuning, -1);
    }
    CHECK_EQ(state.lockCount, 3);

    // Frequency mode is not a lock, whatever the offset says.
    t41ptp::servoUpdate(state, tuning, NS_PER_SECOND_VALUE, NS_PER_SECOND_VALUE - 10000, 0);
    CHECK_EQ(state.lockCount, 0);
}

void runServoTests()
{
    testAnUnmovedReferenceIsNotAMeasurement();
    testDriftPastWhatACrystalCanDoIsRefused();
    testFrequencyModeTakesTheMeasuredRate();
    testTheFrequencyTermIsHeldAtItsBound();
    testACoarseOffsetStepsTheClock();
    testFineModeIsTheSumOfTheThreeTerms();
    testTheIntegralTermIsHeldAtItsBound();
    testTheLockCountsWhileTheOffsetStaysInside();
}
