//
// ptp-servo.cpp
// t41-ptp
//

#include "ptp-servo.h"

#include <cmath>

namespace t41ptp
{

namespace
{

constexpr double NS_PER_S = 1000.0 * 1000.0 * 1000.0;

double clampSymmetric(double value, double limit)
{
    if (value > limit)
    {
        return limit;
    }
    if (value < -limit)
    {
        return -limit;
    }
    return value;
}

}  // namespace

ServoOutcome servoUpdate(ServoState &state, const ServoTuning &tuning, NanoTime refDiff,
                         NanoTime localDiff, NanoTime currentOffset)
{
    ServoOutcome outcome;

    const double refStep = static_cast<double>(refDiff);
    const double localStep = static_cast<double>(localDiff);

    // Two equal reference timestamps give 0/0 = NaN, and every comparison
    // with NaN is false -- so the drift test below came out false and the
    // NaN went into the controller as a valid drift. The single-zero case
    // was already covered by luck: it gives infinity, and infinity does
    // exceed the threshold.
    if (refStep == 0.0)
    {
        return outcome;
    }

    const double drift = localStep / refStep;
    const double driftNsps = (1.0 - drift) * NS_PER_S;

    // And if something that is not a number still comes out, it is not let
    // through.
    if (!std::isfinite(driftNsps))
    {
        return outcome;
    }

    outcome.measuredDriftNsps = driftNsps;

    // A crystal drifts around 30 ppm. A master implying three times that
    // has been stepped, not run fast, and the measurement says nothing
    // about a rate to follow.
    if (driftNsps > tuning.maxDriftNsps || driftNsps < -tuning.maxDriftNsps)
    {
        outcome.mode = ServoMode::DriftError;
        state.lockCount = 0;
        return outcome;
    }

    const bool freqMode =
        driftNsps > tuning.freqModeThresholdNsps || driftNsps < -tuning.freqModeThresholdNsps;
    const bool coarseMode = currentOffset > tuning.coarseModeThresholdNs ||
                            currentOffset < -tuning.coarseModeThresholdNs;
    const NanoTime offsetCorrection = -currentOffset;

    if (freqMode)
    {
        // Bounded where it is kept, not only where it is used: the old fmod
        // against a thousand million never fired for any value a real
        // oscillator produces, so this term grew without limit and the
        // state itself became meaningless.
        state.driftNsps =
            clampSymmetric(state.driftNsps + tuning.kf * driftNsps, tuning.maxFreqAdjustNsps);
        state.offsetAccumulator = 0;

        outcome.mode = ServoMode::Frequency;
        outcome.adjustFrequency = true;
        outcome.freqTermNsps = state.driftNsps;
        outcome.freqAdjustNsps = state.driftNsps;
    }
    else if (coarseMode)
    {
        state.offsetAccumulator = 0;

        outcome.mode = ServoMode::Coarse;
        outcome.stepClock = true;
        outcome.offsetCorrectionNs = offsetCorrection;
    }
    else
    {
        state.offsetAccumulator += static_cast<double>(offsetCorrection);

        // Anti-windup: the accumulator is held where its own contribution
        // reaches the largest correction that will ever be applied. Past
        // that point it only stores a debt that has to be paid back before
        // the clock can move the other way.
        const double integralLimit =
            (tuning.ki > 0.0) ? (tuning.maxFreqAdjustNsps / tuning.ki) : 0.0;
        state.offsetAccumulator = clampSymmetric(state.offsetAccumulator, integralLimit);

        outcome.mode = ServoMode::Fine;
        outcome.adjustFrequency = true;
        outcome.freqTermNsps = state.driftNsps;
        outcome.proportionalTermNsps = static_cast<double>(offsetCorrection) * tuning.kp;
        outcome.integralTermNsps = state.offsetAccumulator * tuning.ki;
        outcome.freqAdjustNsps =
            outcome.freqTermNsps + outcome.proportionalTermNsps + outcome.integralTermNsps;
        outcome.offsetCorrectionNs = offsetCorrection;
    }

    if (!freqMode && !coarseMode && currentOffset < tuning.lockThresholdNs &&
        currentOffset > -tuning.lockThresholdNs)
    {
        state.lockCount++;
    }
    else
    {
        state.lockCount = 0;
    }

    return outcome;
}

}  // namespace t41ptp
