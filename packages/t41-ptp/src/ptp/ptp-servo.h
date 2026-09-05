#pragma once

//
// The clock servo's decision, apart from the hardware that carries it out.
//
// PTPBase::updateController() did both: it worked out the drift, chose
// between the modes, and called EthernetIEEE1588 from inside the choosing.
// Nothing about the servo could therefore be run without a Teensy on the
// desk, and nothing could ask it what it would do with a given pair of
// timestamps -- every claim about how it behaves was a claim about what
// the source appears to say.
//
// So the arithmetic lives here, as free functions over plain numbers: no
// Arduino, no QNEthernet, no hardware. servoUpdate() says what should
// happen and updateController() does it. The modes, the thresholds, the
// accumulators and the lock counting are the code that was there, moved
// rather than rewritten, bounds and anti-windup included.
//

#include <cstdint>

namespace t41ptp
{

using NanoTime = int64_t;

// The tuning the servo works to. Every one of these belongs to the board
// and the deployment, not to IEEE 1588.
struct ServoTuning
{
    double kp = 1.0;
    double ki = 0.5;
    // Gain on the frequency-mode correction. One takes the whole measured
    // rate error in a single step.
    double kf = 1.0;

    // Above this much drift the measurement is refused outright: a master
    // implying more than a crystal could do has just been stepped, and is
    // not a frequency to chase.
    double maxDriftNsps = 100000.0;

    // The widest correction that will ever be asked of the timer, which is
    // also where the frequency term and the integral term are held.
    double maxFreqAdjustNsps = 100000.0;

    // Above this drift the servo corrects frequency alone.
    double freqModeThresholdNsps = 1000.0;

    // Above this offset it steps the clock instead of steering it.
    NanoTime coarseModeThresholdNs = 1000;

    // Inside this window a measurement counts towards being locked.
    NanoTime lockThresholdNs = 100;
};

// What the servo carries from one measurement to the next.
struct ServoState
{
    // The accumulated frequency correction, in nanoseconds per second.
    double driftNsps = 0;

    // The integral term's accumulator, in nanoseconds. A double, and held
    // inside the correction it can ever contribute: as an int it was a
    // 64-bit offset accumulated into 32 bits with no bound at all, and
    // signed overflow is undefined behaviour rather than a large number.
    double offsetAccumulator = 0;

    // Consecutive measurements inside the lock window.
    int lockCount = 0;
};

// Which of the four things the servo did with a measurement.
enum class ServoMode
{
    // Nothing: the two reference timestamps were equal, or the drift came
    // out as something that is not a number.
    NoMeasurement,
    // The drift is past what an oscillator can produce, so the master is
    // not to be followed this time round.
    DriftError,
    // Frequency alone.
    Frequency,
    // A step of the clock.
    Coarse,
    // The PI loop, with the frequency term feeding forward.
    Fine,
};

// What the caller has to do about it.
struct ServoOutcome
{
    ServoMode mode = ServoMode::NoMeasurement;

    // Hand freqAdjustNsps to the timer's rate adjustment. Unclamped: the
    // bound belongs where the hardware is touched, so that every
    // correction from anywhere passes through one clamp.
    bool adjustFrequency = false;
    double freqAdjustNsps = 0;

    // Step the timer by offsetCorrectionNs.
    bool stepClock = false;

    // What the offset was corrected by, and so what the local timestamp of
    // this measurement moves by. Zero unless the clock was steered or
    // stepped.
    NanoTime offsetCorrectionNs = 0;

    // The measurement itself, and the three terms of the correction, for
    // whoever wants to log them.
    double measuredDriftNsps = 0;
    double freqTermNsps = 0;
    double proportionalTermNsps = 0;
    double integralTermNsps = 0;
};

// One measurement: the step of the reference clock, the step of the local
// one, and the offset between them. state is carried forward.
ServoOutcome servoUpdate(ServoState &state, const ServoTuning &tuning, NanoTime refDiff,
                         NanoTime localDiff, NanoTime currentOffset);

}  // namespace t41ptp
