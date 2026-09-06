// PTPBase, the configuration.
//
// Every setter the sketch calls before begin(), the profile that sets a whole
// group of them at once, and the clamping that keeps a value the standard
// bounds inside those bounds. No protocol runs here: this is the state the
// rest of the port is built from.
//
// Split out of ptp-base.cpp, which held the whole class in two thousand lines.

#include <Arduino.h>
#include <QNEthernet.h>
#include <TimeLib.h>

#include "ptp-base.h"
#include "ptp-internal.h"

void PTPBase::setKi(double val)
{
    KI = val;
    resetServo();
}

void PTPBase::setClockClass(uint8_t val)
{
    clockClass = val;
    // Our own dataset is half of the comparison: a port that has just
    // been told it is better, or worse, decides again.
    updatePortState();
}

void PTPBase::setClockAccuracy(uint8_t val)
{
    clockAccuracy = val;
    // Our own dataset is half of the comparison: a port that has just
    // been told it is better, or worse, decides again.
    updatePortState();
}

void PTPBase::setOffsetScaledLogVariance(uint16_t val)
{
    offsetScaledLogVariance = val;
    // Our own dataset is half of the comparison: a port that has just
    // been told it is better, or worse, decides again.
    updatePortState();
}

void PTPBase::setPriority1(uint8_t val)
{
    priority1 = val;
    // Our own dataset is half of the comparison: a port that has just
    // been told it is better, or worse, decides again.
    updatePortState();
}

void PTPBase::setPriority2(uint8_t val)
{
    priority2 = val;
    // Our own dataset is half of the comparison: a port that has just
    // been told it is better, or worse, decides again.
    updatePortState();
}

void PTPBase::setTimeSource(uint8_t val)
{
    timeSource = val;
}

void PTPBase::setMasterIdentity(const uint8_t *identity)
{
    if (identity == nullptr)
    {
        clearMasterIdentity();
        return;
    }
    for (int i = 0; i < 8; i++)
    {
        pinnedMasterIdentity[i] = identity[i];
    }
    masterIdentityPinned = true;
    masterSelected = false;
    // The choice is gone, so the state that came out of it is too.
    // Dropping masterSelected on its own left the port where the last
    // comparison had put it: a master-only port that had stood aside
    // stayed Passive, and nothing brings a Passive port back without a
    // selected master to release -- so it never announced again, and the
    // pinned master it was waiting for was the only thing that could
    // ever have freed it.
    updatePortState();
}

void PTPBase::clearMasterIdentity()
{
    masterIdentityPinned = false;
    masterSelected = false;
    updatePortState();
}

void PTPBase::setBmcaEnabled(bool val)
{
    bmcaEnabled = val;
    updatePortState();
}

void PTPBase::setDomainNumber(uint8_t val)
{
    domainNumber = val;
}

void PTPBase::setCurrentUtcOffset(int16_t val)
{
    currentUtcOffset = val;
}

void PTPBase::setUtcOffsetValid(bool val)
{
    utcOffsetValid = val;
}

void PTPBase::setLeap59(bool val)
{
    leap59 = val;
}

void PTPBase::setLeap61(bool val)
{
    leap61 = val;
}

void PTPBase::setTimeTraceable(bool val)
{
    timeTraceable = val;
}

void PTPBase::setFrequencyTraceable(bool val)
{
    frequencyTraceable = val;
}

void PTPBase::setStepsRemoved(uint16_t val)
{
    stepsRemoved = val;
    // Our own dataset is half of the comparison: a port that has just
    // been told it is better, or worse, decides again.
    updatePortState();
}

void PTPBase::applyProfile(Profile profile)
{
    switch (profile)
    {
    case Profile::Default1588:
        setDomainNumber(0);
        setMajorSdoId(0);
        setLogSyncInterval(0);         // 1 s
        setLogAnnounceInterval(1);     // 2 s
        setLogMinDelayReqInterval(0);  // 1 s
        break;
    case Profile::AES67Media:
        setDomainNumber(0);
        setMajorSdoId(0);
        setLogSyncInterval(-3);        // eight per second
        setLogAnnounceInterval(0);     // 1 s
        setLogMinDelayReqInterval(-3); // eight per second
        break;
    case Profile::GPTP:
        setDomainNumber(0);
        setMajorSdoId(1);
        setLogSyncInterval(-3);        // eight per second
        setLogAnnounceInterval(0);     // 1 s
        setLogMinDelayReqInterval(0);  // Pdelay_Req once a second
        break;
    }
}

// The range 1588 gives a logMessageInterval. Every one of these three
// numbers is clamped to it, the ones that only reach the wire included:
// the delay-request interval was the one that ended up pacing an exchange
// and shifting a thousand by itself, and it was unclamped because at the
// time it only reached the wire too.
static int8_t clampLogInterval(int8_t val)
{
    if (val < MIN_LOG_INTERVAL)
    {
        return MIN_LOG_INTERVAL;
    }
    if (val > MAX_LOG_INTERVAL)
    {
        return MAX_LOG_INTERVAL;
    }
    return val;
}

void PTPBase::setLogSyncInterval(int8_t val)
{
    logSyncInterval = clampLogInterval(val);
}

void PTPBase::setLogAnnounceInterval(int8_t val)
{
    logAnnounceInterval = clampLogInterval(val);
}

// Clamped like the one the wire carries.
//
// It was taken as it came, and peer to peer it is what paces the
// exchange: delayRequestIntervalMillis() hands it to logIntervalToMillis(),
// which shifts a thousand by it. An int8_t reaches 127, unsigned long is
// 32 bits on the board, and a shift that wide is undefined behaviour
// rather than a large number -- the host, where the type is 64 bits wide,
// cannot even see it. Short of that it is merely absurd: 20 asks for one
// request every twelve days, and what survives the cast to uint32_t in
// scheduleNextDelayRequest() is a request on every pass through loop().
void PTPBase::setLogMinDelayReqInterval(int8_t val)
{
    logMinDelayReqIntervalAnnounced = clampLogInterval(val);
}

void PTPBase::setMajorSdoId(uint8_t val)
{
    majorSdoId = val & 0x0f;
}

void PTPBase::setPpsOffset(NanoTime val)
{
    ppsOffset = val;
}

void PTPBase::setKp(double val)
{
    KP = val;
    resetServo();
}

void PTPBase::setKf(double val)
{
    KF = val;
    resetServo();
}

// The thresholds only change what the next measurement is judged by, so
// none of them touches the loop's state: a threshold moved mid-run is not
// a reason to throw away the frequency the servo has learned.
void PTPBase::setMaxDriftNsps(double val)
{
    maxDriftNsps = val;
}

void PTPBase::setFreqModeThresholdNsps(double val)
{
    freqModeThresholdNsps = val;
}

void PTPBase::setCoarseModeThresholdNs(NanoTime val)
{
    coarseModeThresholdNs = val;
}

void PTPBase::setLockThresholdNs(NanoTime val)
{
    lockThresholdNs = val;
}

void PTPBase::setDelayFilterLength(uint8_t val)
{
    if (val < 1)
    {
        val = 1;
    }
    else if (val > MAX_DELAY_FILTER_SAMPLES)
    {
        val = MAX_DELAY_FILTER_SAMPLES;
    }
    delayFilterLength = val;
    delaySampleCount = 0;
    delaySampleIndex = 0;
}

void PTPBase::setTimestampOffset(NanoTime val)
{
    timestampOffset = val;
}

void PTPBase::setPeerOffsetCorrection(NanoTime val)
{
    peerOffsetCorrection = val;
}
