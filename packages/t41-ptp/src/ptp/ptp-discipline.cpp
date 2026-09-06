// PTPBase, the clock discipline.
//
// What happens to this board's clock once an offset is known: the delay
// filter, the servo call and what it does with the answer, the coarse step
// against the fine rate change, the lock counting, and the external reference
// on the PPS pin. t41ptp::servoUpdate() is the decision; this is the part that
// carries it out and the bookkeeping around it.
//
// Split out of ptp-base.cpp, which held the whole class in two thousand lines.

#include <Arduino.h>
#include <QNEthernet.h>
#include <TimeLib.h>

#include "ptp-base.h"
#include "ptp-internal.h"

// Only what the loop itself carries. A gain changed at run time used to
// go through reset(), which by now also drops the chosen master, the
// receipt state and the delay window -- none of which a gain has anything
// to do with.
void PTPBase::resetServo()
{
    servo = t41ptp::ServoState();
}

// A delay below zero is not a path, it is a bad exchange: one of the four
// timestamps did not belong with the others. Storing it would hand the
// minimum filter a value nothing can beat for the length of the window.
bool PTPBase::recordDelaySample(NanoTime delay)
{
    if (delay < 0)
    {
        return false;
    }
    delaySamples[delaySampleIndex] = delay;
    delaySampleMillis[delaySampleIndex] = millis();
    delaySampleIndex = (uint8_t)((delaySampleIndex + 1) % delayFilterLength);
    if (delaySampleCount < delayFilterLength)
    {
        delaySampleCount++;
    }
    return true;
}

NanoTime PTPBase::filteredDelay() const
{
    if (delaySampleCount == 0)
    {
        return currentDelay;
    }

    // Old enough that the window would have turned over completely at the
    // rate the master asks for. Past that, a sample says nothing about the
    // path as it is now.
    const unsigned long maxAge = 2 * delayRequestIntervalMillis() * delayFilterLength;
    const unsigned long now = millis();

    NanoTime smallest = 0;
    bool haveSmallest = false;
    NanoTime newest = delaySamples[0];
    unsigned long newestAge = now - delaySampleMillis[0];

    for (uint8_t i = 0; i < delaySampleCount; i++)
    {
        const unsigned long age = now - delaySampleMillis[i];
        if (age < newestAge)
        {
            newest = delaySamples[i];
            newestAge = age;
        }
        if (age > maxAge)
        {
            continue;
        }
        if (!haveSmallest || delaySamples[i] < smallest)
        {
            smallest = delaySamples[i];
            haveSmallest = true;
        }
    }

    // Everything in the window is stale: the newest of them is still the
    // best guess there is, and better than reporting no path at all.
    return haveSmallest ? smallest : newest;
}

// Arms the hardware to timestamp the next frame out, after throwing away
// anything left in the register.
//
// A wait that timed out leaves its timestamp behind when the hardware
// posts it a moment later. The next read -- belonging to a different
// message -- would take that one and publish it as its own departure
// time, which is a Follow_Up saying a Sync left before it did.
void PTPBase::armTxTimestamp()
{
    timespec stale;
    qindesign::network::EthernetIEEE1588.readAndClearTxTimestamp(stale);
    qindesign::network::EthernetIEEE1588.timestampNextFrame();
}

// Every rate correction goes through here, so none of them can leave the
// range the hardware can act on.
void PTPBase::adjustFrequency(double nsps)
{
    if (nsps > MAX_FREQ_ADJUST_NSPS)
    {
        nsps = MAX_FREQ_ADJUST_NSPS;
    }
    else if (nsps < -MAX_FREQ_ADJUST_NSPS)
    {
        nsps = -MAX_FREQ_ADJUST_NSPS;
    }
    qindesign::network::EthernetIEEE1588.adjustFreq(nsps);
}

NanoTime PTPBase::updateController(NanoTime refDiff, NanoTime localDiff)
{
    // What to do comes from ptp-servo.h, which is arithmetic over plain
    // numbers and runs on a host; this function is what touches the
    // hardware. The modes and the thresholds are the ones that were here.
    t41ptp::ServoTuning tuning;
    tuning.kp = KP;
    tuning.ki = KI;
    tuning.kf = KF;
    tuning.maxFreqAdjustNsps = MAX_FREQ_ADJUST_NSPS;
    tuning.maxDriftNsps = maxDriftNsps;
    tuning.freqModeThresholdNsps = freqModeThresholdNsps;
    tuning.coarseModeThresholdNs = coarseModeThresholdNs;
    tuning.lockThresholdNs = lockThresholdNs;

    const t41ptp::ServoOutcome outcome =
        t41ptp::servoUpdate(servo, tuning, refDiff, localDiff, currentOffset);

    if (outcome.stepClock)
    {
        qindesign::network::EthernetIEEE1588.offsetTimer(outcome.offsetCorrectionNs);
    }
    if (outcome.adjustFrequency)
    {
        adjustFrequency(outcome.freqAdjustNsps);
    }

    if (logging)
    {

        Serial.printf("T2diff:%f T1diff:%f\n", static_cast<double>(localDiff),
                      static_cast<double>(refDiff));
        Serial.printf("T2-T1:%d T4-T3:%d\n", (int)(t2 - t1), (int)(t4 - t3));
        Serial.printf("Delay:%dns ", (int)currentDelay);
        Serial.printf("Offset:%dns ", (int)currentOffset);
        Serial.printf("Drift:%dns \n", (int)outcome.measuredDriftNsps);

        switch (outcome.mode)
        {
        case t41ptp::ServoMode::NoMeasurement:
            Serial.printf("No measurement\n No controller update.\n");
            break;
        case t41ptp::ServoMode::DriftError:
            Serial.printf("Drift Error\n No controller update.\n");
            break;
        case t41ptp::ServoMode::Frequency:
            Serial.printf("Freq mode adjust f: %f ns/s", outcome.freqAdjustNsps);
            break;
        case t41ptp::ServoMode::Coarse:
            Serial.printf("Coarse mode adjust:%d ns", (int)outcome.offsetCorrectionNs);
            break;
        case t41ptp::ServoMode::Fine:
            Serial.printf("Fine filter mode ns/s:%f C:%f P(%f):%f I(%f):%f",
                          outcome.freqAdjustNsps, outcome.freqTermNsps, KP,
                          outcome.proportionalTermNsps, KI, outcome.integralTermNsps);
            break;
        }

        Serial.println();
        if (outcome.mode != t41ptp::ServoMode::DriftError &&
            outcome.mode != t41ptp::ServoMode::NoMeasurement)
        {
            Serial.printf("ENET_ATINC %08X\n", ENET_ATINC);
            Serial.printf("ENET_ATPER %d\n", ENET_ATPER);
            Serial.printf("ENET_ATCOR %d (%f)\n", ENET_ATCOR, 25000000 / outcome.freqAdjustNsps);
        }
        Serial.println();
        Serial.println();
        Serial.println();
    }

    return outcome.offsetCorrectionNs;
}

int PTPBase::getLockCount() const
{
	return servo.lockCount;
}

// The path delay, from the exchange that has just finished.
//
// The end-to-end formula uses T1 and T2 as they stood when the request
// went out, not as they stand now: with the request paced, the answer can
// arrive after a newer Sync has already moved them.
void PTPBase::updateDelay()
{
    const NanoTime sample = p2p ? (((t6 - t3) - (t5 - t4)) / 2)
                                : (((t4 - requestT1) - (t3 - requestT2)) / 2);
    // A rejected sample says nothing about the path, so neither the
    // measurement nor the flag moves. delayValid was set either way
    // before: with the first exchange of a port giving a negative delay
    // -- one bad timestamp does it -- the path counted as measured at
    // the zero currentDelay still held, and every Sync from then until
    // the next good exchange corrected the clock with the whole path
    // delay left in the offset.
    if (!recordDelaySample(sample))
    {
        return;
    }
    currentDelay = filteredDelay();
    delayValid = true;
}

void PTPBase::updateTimer()
{
    if (logging >= 2)
    {
        Serial.println("NEW DATA");
    }
    currentOffset = (t2 - t1) - currentDelay + (p2p ? peerOffsetCorrection : timestampOffset);

    // T2 moves with the clock: it is a reading of the timer this
    // measurement has just stepped, and the next Sync measures its step
    // against it.
    t2 += updateController(t1 - t1last, t2 - t2last);
}

void PTPBase::updatePPS(NanoTime refNow, NanoTime refLast, NanoTime localNow,
                        NanoTime localLast)
{
    // The reference arrives on a pin: there is no path to subtract, so
    // nothing is subtracted. currentDelay used to be zeroed here instead,
    // and the zero outlived the reference -- it belongs to the network
    // path, which goes on measuring while the pin holds the clock. When
    // the pin fell silent and the Sync took the clock back, every offset
    // until the next completed delay exchange was short by the whole path
    // delay, and getDelay() reported no path at all in the meantime.
    currentOffset = (localNow - refNow) + ppsOffset;
    const NanoTime correction = updateController(refNow - refLast, localNow - localLast);
    if (correction == 0)
    {
        return;
    }

    // The reference's own local timestamp moves with the clock too, and
    // this is the half that was missing: the Sync path had its T2
    // corrected and the pin's pair did not, so the edge after a step read
    // the step itself as drift. Five microseconds stepped out is five
    // thousand nanoseconds per second of rate error a second later --
    // inside the frequency mode's window, so it went into the frequency
    // term and stayed there.
    //
    // Which of the two holds the timestamp this measurement was taken
    // against depends on whether an edge has arrived since: the interrupt
    // shifts ppsT2 into ppsT2last and writes a new ppsT2, and a timestamp
    // taken after the step must not be corrected for it.
    noInterrupts();
    if (ppsT2 == localNow)
    {
        ppsT2 = ppsT2 + correction;
    }
    else if (ppsT2last == localNow)
    {
        ppsT2last = ppsT2last + correction;
    }
    interrupts();
}

bool PTPBase::externalReferenceLive() const
{
    return externalReferenceSeen &&
           (millis() - lastExternalReferenceMillis) <= EXTERNAL_REFERENCE_TIMEOUT_MS;
}

NanoTime PTPBase::getOffset() const
{
    return currentOffset;
}

NanoTime PTPBase::getDelay() const
{
    return currentDelay;
}

void PTPBase::ppsInterruptTriggered(NanoTime pps_ts, NanoTime local_ts){
	// The role this port was configured into, not the state the BMCA has
	// put it in. What the pin is fed does not change when a better master
	// appears on the segment, and this used to return unless the port was
	// currently Master: a port configured master and slave threw its own
	// reference away the moment it stood aside, and disciplined itself
	// from that master's Sync instead -- the network correcting the clock
	// that is wired to the house reference, which is backwards. A port not
	// configured master still ignores the call, as before.
	if(!initialised || !master){
		return;
	}
	// Live, not merely seen once. externalReferenceSeen never expires, so
	// after the pin had been silent long enough for the network to take
	// the clock back, the edge that brought the reference in again was
	// measured against the pair from before the silence -- a step the Sync
	// path made in the meantime read as drift over the whole gap. A
	// reference that has been away starts again from one edge, exactly as
	// it did at boot.
	const bool hadReference = externalReferenceLive();

	ppsT1last = ppsT1;
	ppsT2last = ppsT2;
	ppsT1 = pps_ts;
	ppsT2 = local_ts;
	lastExternalReferenceMillis = millis();
	externalReferenceSeen = true;

	// The first edge only arms the path: the feedforward term needs a
	// previous pair to measure the step against, which is what the Sync
	// path's t1lastvalid/t2lastvalid used to provide here by accident of
	// sharing them. The reference counts as present from this edge on
	// either way, so the Sync is held off from the first one.
	ppsupdated = hadReference;
}
