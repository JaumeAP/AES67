#include <Arduino.h>
#include <QNEthernet.h>
#include <TimeLib.h>
#include "ptp-base.h"

constexpr int logging = T41PTP_LOGGING_LEVEL;

void printTime(const NanoTime t)
{
    NanoTime x = t;
    const int ns = static_cast<int>(x % 1000);
    x /= 1000;
    const int us = static_cast<int>(x % 1000);
    x /= 1000;
    const int ms = static_cast<int>(x % 1000);
    x /= 1000;

    tmElements_t tme;
    breakTime((time_t)x, tme);

    Serial.printf("%02d.%02d.%04d %02d:%02d:%02d::%03d:%03d:%03d\n", tme.Day, tme.Month, 1970 + tme.Year, tme.Hour, tme.Minute, tme.Second, ms, us, ns);
}

NanoTime timespecToNanoTime(const timespec &tm)
{
    const NanoTime s = tm.tv_sec;
    const NanoTime ns = tm.tv_nsec;
    return (s * NS_PER_S) + ns;
}

NanoTime bufferToNanoTime(const uint8_t *buf)
{
    NanoTime s = ((NanoTime)buf[34]) << 40;
    s += ((NanoTime)buf[35]) << 32;
    s += ((NanoTime)buf[36]) << 24;
    s += ((NanoTime)buf[37]) << 16;
    s += ((NanoTime)buf[38]) << 8;
    s += ((NanoTime)buf[39]);
    NanoTime ns = ((NanoTime)buf[40]) << 24;
    ns += ((NanoTime)buf[41]) << 16;
    ns += ((NanoTime)buf[42]) << 8;
    ns += ((NanoTime)buf[43]);

    // Clamped before the multiplication. No real timestamp comes near
    // the limit -- it is 292 years from the epoch -- so this only
    // touches corrupt or hostile packets, and the jump a clamped value
    // produces is rejected downstream by the drift guard.
    if (s > MAX_SAFE_SECONDS)
    {
        s = MAX_SAFE_SECONDS;
    }

    // 1588 defines the nanoseconds field as below a thousand million;
    // the wire carries 32 bits and nothing stops a sender writing more.
    // Left alone it walked straight past the clamp above: the seconds
    // held at MAX_SAFE_SECONDS plus a nanoseconds field of 0xffffffff is
    // four thousand million past what an int64_t holds, which is the
    // overflow the clamp exists to prevent.
    if (ns >= NS_PER_S)
    {
        ns = NS_PER_S - 1;
    }

    return (s * NS_PER_S) + ns;
}

NanoTime bufferToCorrection(const uint8_t *buf)
{
    // Assembled unsigned and then reinterpreted, rather than shifted
    // into a signed type. buf[8] is the top octet of a signed 64-bit
    // field, so `((NanoTime)buf[8]) << 56` overflows an int64_t for
    // anything from 0x80 up -- which is EVERY negative correctionField,
    // the ones a real path produces included, not only a hostile one.
    // Signed overflow is undefined behaviour, not a wrong number.
    uint64_t raw = 0;
    for (int i = 0; i < 8; i++)
    {
        raw = (raw << 8) | buf[8 + i];
    }
    // Nanoseconds scaled by 2^16, so the shift is the scaling undone. It
    // leaves at most 2^47 in magnitude, which is why negating the result
    // -- as the delay path does -- is always safe.
    return static_cast<NanoTime>(raw) >> 16;
}

// The correctionField of a request, carried into the answer.
//
// Transparent clocks on the path add their residence time to the request's
// correctionField as it goes by, and the requester subtracts it again from
// the answer. An answer that comes back with zero there throws that away,
// and every transparent clock in the path becomes an error in the measured
// path delay. 1588 says the Delay_Resp carries the Delay_Req's field
// (clause 11.3) and the Pdelay_Resp the Pdelay_Req's (11.4.3).
void copyCorrectionField(const uint8_t *request, uint8_t *answer)
{
    for (int i = 8; i < 16; i++)
    {
        answer[i] = request[i];
    }
}

void timespecToBuffer(const timespec &tm, uint8_t *buf)
{
    const NanoTime s = tm.tv_sec;
    const NanoTime ns = tm.tv_nsec;

    buf[34] = static_cast<uint8_t>((s >> 40)  & 0xff);
    buf[35] = static_cast<uint8_t>((s >> 32)  & 0xff);
    buf[36] = static_cast<uint8_t>((s >> 24)  & 0xff);
    buf[37] = static_cast<uint8_t>((s >> 16)  & 0xff);
    buf[38] = static_cast<uint8_t>((s >> 8)   & 0xff);
    buf[39] = static_cast<uint8_t>( s         & 0xff);
    buf[40] = static_cast<uint8_t>((ns >> 24) & 0xff);
    buf[41] = static_cast<uint8_t>((ns >> 16) & 0xff);
    buf[42] = static_cast<uint8_t>((ns >> 8)  & 0xff);
    buf[43] = static_cast<uint8_t>( ns        & 0xff);
}

void nanoTimeToTimespec(const NanoTime t, timespec &tm)
{
    NanoTime ns = t % NS_PER_S;
    NanoTime s = t / NS_PER_S;
    // Integer division truncates towards zero, so a negative NanoTime
    // gave a negative tv_nsec -- which timespecToBuffer() then wrote to
    // the wire as its two's complement, a nanoseconds field of four
    // thousand million. Reachable without a hostile packet: every
    // timestamp taken has timestampOffset added to it, the default is
    // negative, and the clock starts at zero on the first begin().
    if (ns < 0)
    {
        ns += NS_PER_S;
        s -= 1;
    }
    tm.tv_sec = clampToTvSec(s);
    tm.tv_nsec = static_cast<decltype(tm.tv_nsec)>(ns);
}

PTPBase::PTPBase(bool master_, bool slave_, bool p2p_):
master(master_),
slave(slave_),
p2p(p2p_)
{

}

void PTPBase::begin()
{
    // A second begin() re-initialises the port, sockets included.
    //
    // It used to reset the state and then return, leaving the sockets as
    // they were: half a re-initialisation, and one that threw away the
    // chosen master, the delay window and the servo without saying so
    // while the transport carried on as before. Either the port is being
    // brought up again -- which is what INITIALIZING means in 1588 -- or
    // it is not.
    if (initialised)
    {
        initialised = false;
        closeSockets();
        portState = PortState::Initializing;
    }

    reset();
    initSockets();

    // The clock itself starts at zero once, when the port is first
    // brought up. Coming back after an end(), or after the link dropped,
    // leaves it running: a slave steps back to its master in coarse mode
    // and a master keeps the time it is holding.
    if (!timerZeroed)
    {
        timespec tm;
        tm.tv_sec = 0;
        tm.tv_nsec = 0;
        qindesign::network::EthernetIEEE1588.writeTimer(tm);
        timerZeroed = true;
    }

    initialised = true;
    updatePortState();

    if (logging)
    {
        Serial.println("PTP Started");
    }
}

void PTPBase::end()
{
    if (!initialised)
    {
        return;
    }
    initialised = false;
    closeSockets();
    reset();
    portState = PortState::Initializing;

    if (logging)
    {
        Serial.println("PTP Stopped");
    }
}

void PTPBase::update()
{
    if (initialised)
    {
        // Before anything else that could take the transmit timestamp out
        // of the hardware.
        serviceTxTimestamps();

        updateSockets();
        serviceTxTimestamps();
        // A finished delay exchange refreshes the path delay, and only
        // that. It used to be the other way round: the offset was worked
        // out inside the same test, so a Sync whose Delay_Req the pacing
        // held back corrected nothing at all.
        //
        // Before the next request goes out, not after it. An answer
        // arriving in the same update() that sends the following request
        // left T3 -- and, end to end, the T1 and T2 the request was
        // taken against -- already overwritten by the new exchange, so
        // the delay came out of one exchange's departure and another's
        // arrival. At eight Sync a second that is a Delay_Resp and a
        // Sync pair drained together, which is an ordinary pass through
        // update() rather than a rare one.
        bool delayExchangeComplete = t3updated && t4updated;
        if(p2p){
            delayExchangeComplete &= t5updated && t6updated;
        }
        if (delayExchangeComplete)
        {
            t3updated = false;
            t4updated = false;
            t5updated = false;
            t6updated = false;
            updateDelay();
        }
        // Peer delay measures the link and not the hierarchy, so the
        // request belongs to the port whatever the port is doing, and it
        // goes out on the port's own interval. It used to be armed by a
        // matched Sync pair, like the end-to-end request: a peer-delay
        // master -- which parses no Sync at all -- therefore never asked
        // for its link delay and never had one, and neither did a slave
        // until a master turned up. 1588 clause 11.4 has the exchange run
        // between neighbours independently of the synchronisation tree,
        // which is the whole point of measuring the link rather than the
        // path.
        //
        // The end-to-end request stays where it was: its measurement is
        // taken against the T1 and T2 of a Sync pair, so there is nothing
        // to ask for until one has arrived.
        const bool requestDue = p2p || syncPairMatched;
        syncPairMatched = false;
        if (requestDue)
        {
            // Not before the interval this port measures at says so.
            if ((long)(millis() - nextDelayRequestMillis) >= 0)
            {
                // Only a request that went out has spent its turn: one
                // held back by a pending peer answer must not push the
                // next measurement a whole interval away.
                if (delayRequestMessage())
                {
                    scheduleNextDelayRequest();
                }
            }
        }

        // Every Sync pair corrects the clock, against the delay last
        // measured. That is what a delay measured every so often is for.
        if (t1updated && t2updated)
        {
            t1updated = false;
            t2updated = false;
            // Not while a newer Sync is already in flight: T1 and T2 would
            // then belong to an exchange the rest of the timestamps do not.
            //
            // And not while an external reference is arriving: the pin
            // wins over the network. A port configured master and slave
            // stands aside for a better master and starts parsing that
            // master's Sync, which would otherwise steer a clock already
            // held by the reference it is wired to -- two servos pulling
            // the same oscillator. The network takes it back on its own
            // once EXTERNAL_REFERENCE_TIMEOUT_MS have passed with no edge.
            if (delayValid && t1lastvalid && t2lastvalid && !syncCycleActive &&
                !externalReferenceLive())
            {
                t1lastvalid = false;
                t2lastvalid = false;
                updateTimer();
            }
        }

        // The chosen master stops being the chosen master when it stops
        // saying who it is, so another one can take over -- including this
        // port itself, if it stood aside for it.
        if (masterSelected)
        {
            const unsigned long announceInterval =
                logIntervalToMillis(logAnnounceIntervalFromMaster);
            if ((millis() - lastAnnounceMillis) >
                announceInterval * ANNOUNCE_RECEIPT_TIMEOUT_INTERVALS)
            {
                // announceHeard stays set, so nothing is followed until an
                // Announce chooses a master again.
                masterSelected = false;
                updatePortState();
                if (logging)
                {
                    Serial.println("No Announce received, master released");
                }
            }
        }

        // Nothing from the master for three of its own intervals means the
        // lock says nothing about the clock any more.
        if (slave && syncReceiptValid &&
            (millis() - lastSyncMillis) > syncReceiptTimeoutMillis())
        {
            syncReceiptLost();
        }

        // Anything sent during this pass -- a peer answer, the request
        // above -- has its departure time taken as soon as the hardware
        // has it, rather than a whole pass through loop() later.
        serviceTxTimestamps();

        if (ppsupdated)
		{
            // The four timestamps are read with the interrupt held off:
            // an edge landing between two of them mixes one pair with
            // the next, and on a 32-bit core a single NanoTime is two
            // loads the interrupt can come between.
            noInterrupts();
            const NanoTime refNow = ppsT1;
            const NanoTime refLast = ppsT1last;
            const NanoTime localNow = ppsT2;
            const NanoTime localLast = ppsT2last;
			ppsupdated = false;
            interrupts();
		    updatePPS(refNow, refLast, localNow, localLast);
		}
    }
}

void PTPBase::serviceTxTimestamps()
{
    servicePeerFollowUp();
    serviceSyncFollowUp();
    serviceDelayRequestTimestamp();
}

void PTPBase::reset()
{
    if (logging)
    {
        Serial.println("Reset PTP state");
    }
    // clockIdentity from the MAC address, as 1588 Annex says: the first
    // three octets, then FF FE, then the last three. It used to be FF FF
    // followed by the whole MAC, which is not the mapping the standard
    // defines and collides with nothing else only by luck.
    uint8_t mac[6];
    qindesign::network::Ethernet.macAddress(mac);
    clockID[0] = mac[0];
    clockID[1] = mac[1];
    clockID[2] = mac[2];
    clockID[3] = 0xFF;
    clockID[4] = 0xFE;
    clockID[5] = mac[3];
    clockID[6] = mac[4];
    clockID[7] = mac[5];

    t1 = -1;
    t1last = -1;
    t2 = -1;
    t2last = -1;
    t2new = -1;
    t3 = -1;
    t4 = -1;
    t5 = -1;
    t6 = -1;

    t1updated = false;
    t2updated = false;
    t3updated = false;
    t4updated = false;
    t5updated = false;
    t6updated = false;
    t1lastvalid = false;
    t2lastvalid = false;
    ppsupdated = false;
    ppsT1 = 0;
    ppsT2 = 0;
    ppsT1last = 0;
    ppsT2last = 0;
    externalReferenceSeen = false;

    // The exchange in progress and everything derived from it. These were
    // left standing before, so after a reset getLockCount() still reported
    // the lock the discarded state had reached and getOffset() still
    // returned a measurement taken against timestamps that no longer exist.
    syncSequenceID = 0;
    syncCycleActive = false;
    syncPairMatched = false;
    requestOutstanding = false;
    delayResponseSeen = false;
    logMinDelayReqInterval = 0;
    nextDelayRequestMillis = millis();
    masterSelected = false;
    announceHeard = false;
    selectedMaster = MasterDataset();
    requestT1 = -1;
    requestT2 = -1;
    delayValid = false;
    logAnnounceIntervalFromMaster = 0;
    lastAnnounceMillis = millis();
    logSyncIntervalFromMaster = 0;
    lastSyncMillis = millis();
    syncReceiptValid = false;
    delaySampleCount = 0;
    delaySampleIndex = 0;
    peerResponsePending = false;
    syncFollowUpPending = false;
    delayRequestPending = false;
    currentOffset = 0;
    currentDelay = 0;
    servo = t41ptp::ServoState();

    // With no master chosen any more, a port that was Slave or Passive is
    // neither.
    if (initialised)
    {
        updatePortState();
    }
}

// Only what the loop itself carries. A gain changed at run time used to
// go through reset(), which by now also drops the chosen master, the
// receipt state and the delay window -- none of which a gain has anything
// to do with.
void PTPBase::resetServo()
{
    servo = t41ptp::ServoState();
}

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

void PTPBase::parsePTPMessage(const uint8_t *buf, int size, const timespec &recv_ts)
{
    // Nothing is read before the common header is known to be there.
    if (size < PTP_HEADER_LEN)
    {
        return;
    }

    const uint8_t messageType = buf[0] & 0x0f;
    const uint8_t sdoId = (buf[0] >> 4) & 0x0f;
    const uint8_t versionPTP = buf[1] & 0x0f;
    // Another profile's traffic, as much as another domain's.
    if (sdoId != majorSdoId)
    {
        return;
    }
    const uint8_t domainNumer = buf[4];
    // Another domain's traffic is not ours to act on, whatever it says.
    if (domainNumer != domainNumber)
    {
        return;
    }
    if(versionPTP==2){
        if (logging >= 2)
        {
            Serial.printf("PTPMessage messageType:%d versionPTP:%d domainNumer:%d\n", messageType, versionPTP, domainNumer);
        }
        // Each type checks its own minimum before any field of it is
        // touched: the common header is not enough for the ones that
        // read as far as buf[51].
        if(messageType==11 && size >= PTP_ANNOUNCE_LEN){
            // Every role listens: a master decides whether to keep
            // sending from what it hears here.
            parseAnnounceMessage(buf);
        // The role flags say what this port was configured as; the state
        // says what it is doing. A port that is master and slave at once
        // must not discipline itself from another clock's Sync while it
        // is the one announcing.
        }else if(followingMaster() && messageType==0 && size >= PTP_SYNC_LEN && fromSelectedMaster(buf)){
            parseSyncMessage(buf,recv_ts);
        }else if(!p2p && portState == PortState::Master && messageType==1 && size >= PTP_SYNC_LEN){
            // Only the port that is actually the master answers. A port
            // that has stood aside for a better clock was still handing
            // out Delay_Resp, which pulls whoever asked towards a clock
            // that is no longer the reference.
            //
            // And only a port that measures the delay end to end. The two
            // mechanisms are exclusive in 1588 -- a port configured for
            // peer delay does not run the end-to-end exchange at all --
            // and this branch asked for neither, so a peer-delay master
            // answered Delay_Req with a Delay_Resp carrying its receipt
            // timestamp. Whoever asked measured a path against a clock
            // that had never agreed to be measured that way, and 802.1AS
            // requires the message to be dropped outright.
            parseDelayRequestMessage(buf,recv_ts);
        }
        else if(followingMaster() && messageType == 8 && size >= PTP_SYNC_LEN && fromSelectedMaster(buf)){
            parseFollowUpMessage(buf);
        }else if(p2p && messageType==3 && size >= PTP_DELAY_RESP_LEN){
            // Pdelay_Resp comes from the peer on the wire, which is not
            // the master and is not compared against it -- which is why
            // this branch is the one with no fromSelectedMaster() check,
            // and why it has to be shut on a port that measures the
            // delay end to end. It was open in both modes: a Pdelay_Resp
            // carrying the sequence ID of an outstanding Delay_Req and
            // this clock's requestingPortIdentity set T4 from anybody at
            // all, so relabelling message type 9 as 3 walked straight
            // past the check on the branch below.
            //
            // It is the answer to a request this port sent, so it is
            // accepted in whatever state the port is in -- as the
            // Pdelay_Req below is answered in whatever state it is in.
            // Asking for followingMaster() here left a peer-delay master
            // sending requests whose answers it then threw away, which
            // is the same mistake as pacing the request off a Sync: the
            // link this port sits on does not stop existing because
            // another clock is the better one.
            parseDelayResponseMessage(buf,recv_ts);
        }else if(!p2p && followingMaster() && messageType==9 && size >= PTP_DELAY_RESP_LEN && fromSelectedMaster(buf)){
            // The parentheses were missing, and && binds tighter than
            // ||: this read as (slave && type==3) || (type==9), so
            // message type 9 was parsed even when the device is not a
            // slave. It was the only one of the five branches that did
            // not check the role.
            //
            // `!p2p` is the other half of the guard on the branch above.
            // Without it a Delay_Resp was accepted on a port measuring
            // the delay peer to peer, and parseDelayResponseMessage()
            // treats every answer it accepts as the one the mechanism in
            // use expects: the Delay_Resp set T4 from its own
            // receiveTimestamp, T6 from its arrival, and delayResponseSeen
            // -- so a Pdelay_Resp_Follow_Up completed a peer-delay
            // measurement out of an end-to-end answer the peer never
            // sent, and the link delay came out of two unrelated
            // timestamps.
            parseDelayResponseMessage(buf,recv_ts);
        }else if(p2p && messageType==10 && size >= PTP_DELAY_RESP_LEN){
            // Peer delay only, for the same reason: T5 belongs to the
            // Pdelay_Resp_Follow_Up of a peer-delay exchange and to
            // nothing else. And in any state, like the Pdelay_Resp it
            // completes.
            parseDelayResponseFollowUpMessage(buf);
        }else if(p2p && messageType==2 && size >= PTP_DELAY_RESP_LEN){
            // Peer delay measures the link, not the hierarchy, so it is
            // answered whatever state this port is in -- which is what
            // this comment always claimed while the condition next to it
            // still asked for `master`. A slave-only port that never
            // answers leaves its peer unable to measure the link at all.
            // A peer-delay master had no answer to give: only the
            // end-to-end Delay_Req was handled, so a peer asking for the
            // path delay got nothing back and could never measure it.
            parsePeerDelayRequestMessage(buf,recv_ts);
        }
    }
}


// The dataset comparison of 1588, in the order the standard sets it out:
// priority1, then the clock quality, then priority2, then the identity of
// the grandmaster itself, and only then how far away it is and which port
// sent word of it. Lower wins at every step.
static bool isBetterMaster(const MasterDataset &candidate, const MasterDataset &current)
{
    if (candidate.priority1 != current.priority1)
    {
        return candidate.priority1 < current.priority1;
    }
    if (candidate.clockClass != current.clockClass)
    {
        return candidate.clockClass < current.clockClass;
    }
    if (candidate.clockAccuracy != current.clockAccuracy)
    {
        return candidate.clockAccuracy < current.clockAccuracy;
    }
    if (candidate.offsetScaledLogVariance != current.offsetScaledLogVariance)
    {
        return candidate.offsetScaledLogVariance < current.offsetScaledLogVariance;
    }
    if (candidate.priority2 != current.priority2)
    {
        return candidate.priority2 < current.priority2;
    }
    for (int i = 0; i < 8; i++)
    {
        if (candidate.grandmasterIdentity[i] != current.grandmasterIdentity[i])
        {
            return candidate.grandmasterIdentity[i] < current.grandmasterIdentity[i];
        }
    }
    // Same grandmaster, reached two ways: the shorter way through the
    // network wins, and a tie there is broken by the port that spoke.
    if (candidate.stepsRemoved != current.stepsRemoved)
    {
        return candidate.stepsRemoved < current.stepsRemoved;
    }
    for (int i = 0; i < 10; i++)
    {
        if (candidate.portIdentity[i] != current.portIdentity[i])
        {
            return candidate.portIdentity[i] < current.portIdentity[i];
        }
    }
    return false;
}

static bool sameSource(const MasterDataset &a, const MasterDataset &b)
{
    for (int i = 0; i < 10; i++)
    {
        if (a.portIdentity[i] != b.portIdentity[i])
        {
            return false;
        }
    }
    return true;
}

void PTPBase::parseAnnounceMessage(const uint8_t *buf)
{
    // Our own Announce, heard back: a port that is master and slave at
    // once, or a segment that loops multicast, would otherwise choose
    // itself as its own reference.
    bool ownAnnounce = true;
    for (int i = 0; i < 8; i++)
    {
        if (buf[20 + i] != clockID[i])
        {
            ownAnnounce = false;
            break;
        }
    }
    if (ownAnnounce)
    {
        return;
    }

    // A pinned identity is the only one worth listening to.
    if (masterIdentityPinned)
    {
        for (int i = 0; i < 8; i++)
        {
            if (buf[20 + i] != pinnedMasterIdentity[i])
            {
                return;
            }
        }
    }

    const uint16_t stepsRemoved = (uint16_t)((buf[61] << 8) | buf[62]);
    if (stepsRemoved >= MAX_STEPS_REMOVED)
    {
        // Further away than 1588 allows a port to follow, and what a
        // message that has gone round a loop ends up carrying.
        //
        // Refused before announceHeard is set, not after. A port whose
        // master sends Sync and no Announce follows it because no
        // Announce has ever been heard; setting the flag on the way to
        // refusing the message ended that for good, since
        // fromSelectedMaster() then wanted a chosen master and this
        // Announce had not chosen one. The next Sync of the master it was
        // following was refused, and nothing but a valid Announce could
        // ever bring the port back -- from one message that any device on
        // an unauthenticated multicast group can send, or that a segment
        // which loops multicast produces on its own.
        return;
    }

    announceHeard = true;

    MasterDataset candidate;
    candidate.priority1 = buf[47];
    candidate.clockClass = buf[48];
    candidate.clockAccuracy = buf[49];
    candidate.offsetScaledLogVariance = (uint16_t)((buf[50] << 8) | buf[51]);
    candidate.priority2 = buf[52];
    for (int i = 0; i < 8; i++)
    {
        candidate.grandmasterIdentity[i] = buf[53 + i];
    }
    candidate.stepsRemoved = stepsRemoved;
    for (int i = 0; i < 10; i++)
    {
        candidate.portIdentity[i] = buf[20 + i];
    }

    const bool fromCurrent = masterSelected && sameSource(candidate, selectedMaster);
    if (!masterSelected || fromCurrent || isBetterMaster(candidate, selectedMaster))
    {
        // A master may change its own dataset -- a grandmaster that loses
        // its reference downgrades its clockClass -- so the one being
        // followed is taken as it now says it is, even when that makes it
        // worse. The next Announce from anyone else settles the choice.
        selectedMaster = candidate;
        masterSelected = true;
    }

    updatePortState();

    if (fromCurrent || sameSource(candidate, selectedMaster))
    {
        const int8_t announced = (int8_t)buf[33];
        if (announced >= MIN_LOG_INTERVAL && announced <= MAX_LOG_INTERVAL)
        {
            logAnnounceIntervalFromMaster = announced;
        }
        lastAnnounceMillis = millis();
    }
}

// This port's own dataset, as its Announce would carry it: what the
// comparison weighs a foreign master against.
MasterDataset PTPBase::ownDataset() const
{
    MasterDataset own;
    own.priority1 = priority1;
    own.clockClass = clockClass;
    own.clockAccuracy = clockAccuracy;
    own.offsetScaledLogVariance = offsetScaledLogVariance;
    own.priority2 = priority2;
    for (int i = 0; i < 8; i++)
    {
        own.grandmasterIdentity[i] = clockID[i];
        own.portIdentity[i] = clockID[i];
    }
    own.stepsRemoved = stepsRemoved;
    own.portIdentity[8] = (PORT_NUMBER >> 8) & 0xff;
    own.portIdentity[9] = PORT_NUMBER & 0xff;
    return own;
}

// The state machine of an ordinary clock, in the only shapes this library
// can be configured into.
void PTPBase::updatePortState()
{
    if (!initialised)
    {
        portState = PortState::Initializing;
        return;
    }

    // A foreign master worth stepping aside for.
    const bool foreignIsBetter =
        masterSelected && bmcaEnabled && isBetterMaster(selectedMaster, ownDataset());

    if (master && !slave)
    {
        portState = foreignIsBetter ? PortState::Passive : PortState::Master;
        return;
    }

    if (master && slave)
    {
        // With the comparison off there is nothing to decide from, so
        // the roles as configured stand: follow whatever master has been
        // chosen, and announce only while there is none. This used to
        // read foreignIsBetter alone, which the comparison being off
        // makes false whatever is on the segment -- so a port configured
        // master and slave was pinned to Master for ever and lost its
        // slave half entirely, while a slave-only port with the
        // comparison off went on following a master as it always had.
        const bool follow = bmcaEnabled ? foreignIsBetter : masterSelected;
        portState = follow ? PortState::Slave : PortState::Master;
        return;
    }

    if (slave)
    {
        // A master that sends no Announce is still a master, so a port
        // that has never heard one goes on following what arrives.
        if (masterSelected || !announceHeard)
        {
            portState = PortState::Slave;
        }
        else
        {
            portState = PortState::Listening;
        }
        return;
    }

    portState = PortState::Listening;
}

// Whether a message came from the port this clock is following. Anything
// is accepted until an Announce has been heard: a master that sends none
// is still a master, and this is where the library stood before.
bool PTPBase::fromSelectedMaster(const uint8_t *buf) const
{
    if (!masterSelected)
    {
        // Once an Announce has been heard on this port, a master is
        // chosen or nothing is followed. Before that, anything goes.
        return !announceHeard;
    }
    for (int i = 0; i < 10; i++)
    {
        if (buf[20 + i] != selectedMaster.portIdentity[i])
        {
            return false;
        }
    }
    return true;
}

void PTPBase::setT2(NanoTime ts){
    t2new = ts;
    
    if (logging)
    {
        Serial.print("T2 Sync  receive timestamp=");
        printTime(t2new);
    }
}

void PTPBase::parseSyncMessage(const uint8_t *buf, const timespec &recv_ts)
{
    const uint8_t twoStepFlag = buf[6] & 0x02;
    const uint16_t sequenceID = static_cast<uint16_t>((buf[30] << 8) | buf[31]);

    // Sequence ID zero is a sequence ID like any other: it is where a
    // master that has just started counts from, and it comes round again
    // every 65536 messages. Both were dropped before.
    setT2(timespecToNanoTime(recv_ts));
    syncSequenceID = sequenceID;

    // The master is talking, and this is the rate it says it talks at.
    const int8_t announced = (int8_t)buf[33];
    if (announced >= MIN_LOG_INTERVAL && announced <= MAX_LOG_INTERVAL)
    {
        logSyncIntervalFromMaster = announced;
    }
    lastSyncMillis = millis();
    syncReceiptValid = true;

    if (twoStepFlag > 0)
    {
        // T1 arrives in the Follow_Up.
        syncCycleActive = true;
    }
    else
    {
        // One-step: the Sync carries its own origin timestamp, and there
        // is no Follow_Up to wait for. A one-step master used to be
        // ignored outright -- no Sync of its was ever accepted.
        syncCycleActive = false;
        syncPairMatched = true;
        setT1(addSaturating(bufferToNanoTime(buf), bufferToCorrection(buf)));
    }
}

void PTPBase::setT1(NanoTime ts){
	t1last = t1;
    t1lastvalid = t1last > 0;
    t1 = ts;
    t1updated = true;
    
    t2last = t2; // Update T2 only if valid T1 data was received. Otherwise T2last and T1last might not be frrom the same sequenceID
    t2lastvalid = t2last > 0;
    t2 = t2new;
    t2updated = true;

    if (logging)
    {
        Serial.print("T1 Sync  send    timestamp=");
        printTime(t1);
    }
}

void PTPBase::parseFollowUpMessage(const uint8_t *buf)
{
    const uint16_t sequenceID = static_cast<uint16_t>((buf[30] << 8) | buf[31]);
    // syncCycleActive keeps a repeated Follow_Up from arming a second
    // Delay_Req for a Sync that has already been answered, and keeps a
    // Follow_Up to a one-step Sync out.
    if(syncCycleActive && sequenceID == syncSequenceID)
    {
        syncCycleActive = false;
        syncPairMatched = true;
        setT1(addSaturating(bufferToNanoTime(buf), bufferToCorrection(buf)));
        if (logging > 1)
        {
            Serial.printf("T1 corrected by %" PRId64 "\n", bufferToCorrection(buf));
        }
    }
}

void PTPBase::setT4(NanoTime ts){
	t4 = ts;
    t4updated = true;
    if (logging)
    {
        Serial.print("T4 Delay receive timestamp=");
        printTime(t4);
    }
}

// The full requestingPortIdentity, clock identity and port number both:
// the identity alone does not say which port of ours was asked.
bool PTPBase::requestingPortIdentityMatches(const uint8_t *buf) const
{
    for (int i = 0; i < 8; i++)
    {
        if (buf[44 + i] != clockID[i])
        {
            return false;
        }
    }
    return buf[52] == ((PORT_NUMBER >> 8) & 0xff) && buf[53] == (PORT_NUMBER & 0xff);
}

void PTPBase::parseDelayResponseMessage(const uint8_t *buf, const timespec &recv_ts)
{
    const uint16_t sequenceID = static_cast<uint16_t>((buf[30] << 8) | buf[31]);
    const bool answersOurRequest = requestOutstanding && sequenceID == outstandingRequestSequenceID;
    if (answersOurRequest && requestingPortIdentityMatches(buf))
    {
        // One answer per request: a duplicate no longer overwrites T4.
        requestOutstanding = false;
        // Only the peer-delay exchange has a Follow_Up to order, and it
        // has to come from the port that sent this answer.
        delayResponseSeen = p2p;
        for (int i = 0; i < 10; i++)
        {
            peerResponderIdentity[i] = buf[20 + i];
        }
        // The rate this master wants its requests at.
        const int8_t announced = (int8_t)buf[33];
        if (announced >= MIN_LOG_INTERVAL && announced <= MAX_LOG_INTERVAL)
        {
            logMinDelayReqInterval = announced;
        }
        setT4(addSaturating(bufferToNanoTime(buf), -bufferToCorrection(buf)));
        t6 = timespecToNanoTime(recv_ts);
        t6updated = true;
        if (logging > 1)
        {
            Serial.printf("T4 corrected by -%" PRId64 "\n", bufferToCorrection(buf));
        }
        if (logging)
        {
            if(p2p){
                Serial.print("T6 Resp  receive timestamp=");
                printTime(t6);
            }else{
                Serial.println("");
            }
        }
    }
}

void PTPBase::parseDelayResponseFollowUpMessage(const uint8_t *buf)
{
    const uint16_t sequenceID = static_cast<uint16_t>((buf[30] << 8) | buf[31]);
    // Only after the Pdelay_Resp it belongs to, and for the same request.
    const bool followsOurResponse = delayResponseSeen && sequenceID == outstandingRequestSequenceID;
    // From the peer that sent the Pdelay_Resp, and not merely from
    // somebody who saw it go by. See peerResponderIdentity.
    bool fromTheSameResponder = true;
    for (int i = 0; i < 10; i++)
    {
        if (buf[20 + i] != peerResponderIdentity[i])
        {
            fromTheSameResponder = false;
            break;
        }
    }
    if (followsOurResponse && fromTheSameResponder && requestingPortIdentityMatches(buf))
    {
        delayResponseSeen = false;
        // Added, not subtracted, and for the same reason T1 adds the
        // Sync's: this timestamp is on the far side of the subtraction in
        // updateDelay(), so adding here takes the correction OUT of the
        // measured delay, which is what 1588 asks for.
        t5 = addSaturating(bufferToNanoTime(buf), bufferToCorrection(buf));
        t5updated = true;
        if (logging)
        {
            Serial.print("T5 Resp  send    timestamp=");
            printTime(t5);
            Serial.println("");
        }
    }    
}

void PTPBase::parseDelayRequestMessage(const uint8_t *buf, const timespec &recv_ts)
{
    const uint16_t sequenceID = static_cast<uint16_t>((buf[30] << 8) | buf[31]);
    const NanoTime receipt = timespecToNanoTime(recv_ts) + timestampOffset;
    timespec ts;
    nanoTimeToTimespec(receipt,ts);
    if (logging)
    {
        Serial.print("T4s Delay receiv timestamp=");
        printTime(receipt);
    }
    delayResponseMessage(buf,sequenceID,ts);
}

void PTPBase::delayResponseMessage(const uint8_t *request_buf, uint16_t sequenceID, const timespec &request_recv_ts)
{
    if(!initialised){
        return;
    }
    constexpr uint16_t size = 54;
    uint8_t type=9;
    uint8_t control=3;
   
    uint8_t buf[size] = {0};

    initPTPMessage(buf, size, type, sequenceID, control);
    buf[33]=(uint8_t)logMinDelayReqIntervalAnnounced;
    copyCorrectionField(request_buf, buf);
    timespecToBuffer(request_recv_ts,buf);
    for (int i = 0; i < 10; i++)
    {
        buf[44 + i] = request_buf[20 + i];  // requestingPortIdentity
    }
    sendPTPMessage(buf,size,true,false);
}

void PTPBase::parsePeerDelayRequestMessage(const uint8_t *buf, const timespec &recv_ts)
{
    // One answer in flight at a time. A peer that floods requests gets
    // one response per exchange this port can actually finish, instead of
    // two frames and a millisecond of the loop for every request it sends.
    //
    // And not while a Sync is waiting for its own departure time either:
    // answering arms the register again, which would leave that Sync's
    // Follow_Up with the wrong timestamp or none at all.
    if (txTimestampOwed())
    {
        return;
    }

    const uint16_t sequenceID = static_cast<uint16_t>((buf[30] << 8) | buf[31]);
    const NanoTime receipt = timespecToNanoTime(recv_ts) + timestampOffset;
    timespec ts;
    nanoTimeToTimespec(receipt,ts);
    if (logging)
    {
        Serial.print("T4s Pdelay receiv timestamp=");
        printTime(receipt);
    }
    peerDelayResponseMessage(buf,sequenceID,ts);
}

// The two-step peer-delay answer: Pdelay_Resp carries when the request
// arrived, Pdelay_Resp_Follow_Up when the answer left.
void PTPBase::peerDelayResponseMessage(const uint8_t *request_buf, uint16_t sequenceID,
                                       const timespec &request_recv_ts)
{
    if(!initialised){
        return;
    }
    constexpr uint16_t size = 54;

    uint8_t buf[size] = {0};
    initPTPMessage(buf, size, 3, sequenceID, 5);
    buf[6] = 2;  // twoStepFlag: the origin timestamp is in the Follow_Up
    copyCorrectionField(request_buf, buf);
    timespecToBuffer(request_recv_ts,buf);
    for (int i = 0; i < 10; i++)
    {
        buf[44 + i] = request_buf[20 + i];  // requestingPortIdentity
    }
    armTxTimestamp();
    sendPTPMessage(buf,size,false,true);

    // The departure time is collected from update(), not waited for here:
    // this runs inside the parser, with the receive queue held.
    peerResponsePending = true;
    peerResponseSequenceID = sequenceID;
    for (int i = 0; i < 10; i++)
    {
        peerRequesterIdentity[i] = request_buf[20 + i];
    }
    peerResponseDeadlineMillis = millis() + PEER_FOLLOW_UP_TIMEOUT_MS;
}

// Sends the Pdelay_Resp_Follow_Up once the hardware has the departure
// time, or gives the exchange up when it does not come.
//
// Without the departure time the Follow_Up would have to invent one. The
// peer sees a Pdelay_Resp with no Follow_Up and drops the exchange, which
// is the outcome we want.
void PTPBase::servicePeerFollowUp()
{
    if (!peerResponsePending)
    {
        return;
    }

    struct timespec send_ts;
    if (!qindesign::network::EthernetIEEE1588.readAndClearTxTimestamp(send_ts))
    {
        if ((long)(millis() - peerResponseDeadlineMillis) >= 0)
        {
            peerResponsePending = false;
        }
        return;
    }
    peerResponsePending = false;

    const NanoTime departure = timespecToNanoTime(send_ts) + timestampOffset;
    nanoTimeToTimespec(departure,send_ts);

    constexpr uint16_t size = 54;
    uint8_t followUp[size] = {0};
    initPTPMessage(followUp, size, 10, peerResponseSequenceID, 5);
    timespecToBuffer(send_ts,followUp);
    for (int i = 0; i < 10; i++)
    {
        followUp[44 + i] = peerRequesterIdentity[i];
    }
    sendPTPMessage(followUp,size,true,true);
}

void PTPBase::announceMessage()
{
    if(!initialised || portState != PortState::Master){
        return;
    }
    constexpr uint16_t size = 64;
    uint8_t type=11;
    uint8_t control=5;
   
    uint8_t buf[size] = {0};

    initPTPMessage(buf, size, type, announceServerSequenceID++, control);
    // flagField octet 1, bit by bit: leap61, leap59, currentUtcOffsetValid,
    // PTPTimescale, timeTraceable, frequencyTraceable.
    buf[7] = 0x08
           | (leap61 ? 0x01 : 0x00)
           | (leap59 ? 0x02 : 0x00)
           | (utcOffsetValid ? 0x04 : 0x00)
           | (timeTraceable ? 0x10 : 0x00)
           | (frequencyTraceable ? 0x20 : 0x00);
    buf[33]=(uint8_t)logAnnounceInterval;
    buf[44]=static_cast<uint8_t>((currentUtcOffset >> 8) & 0xff);//UTCOffset
    buf[45]=static_cast<uint8_t>(currentUtcOffset & 0xff);
    buf[47]=priority1;
    buf[48]=clockClass;
    buf[49]=clockAccuracy;
    buf[50]=static_cast<uint8_t>((offsetScaledLogVariance >> 8) & 0xff);
    buf[51]=static_cast<uint8_t>(offsetScaledLogVariance & 0xff);
    buf[52]=priority2;
    for (int i = 0; i < 8; i++)
    {
        buf[53 + i] = clockID[i];  // grandmasterIdentity
    }
    buf[61] = static_cast<uint8_t>((stepsRemoved >> 8) & 0xff);
    buf[62] = static_cast<uint8_t>(stepsRemoved & 0xff);
    buf[63] = timeSource;
    sendPTPMessage(buf,size,true,false);
}

void PTPBase::syncMessage()
{
    if(!initialised || portState != PortState::Master){
        return;
    }
    // Something is waiting for the one timestamp the hardware holds. Take
    // it if it has arrived, and give that exchange up if it has not.
    //
    // A pending peer answer used to make this return, and that let a
    // neighbour stop the master: every update() answers the next
    // Pdelay_Req in the queue and arms the register again, so with one
    // request arriving per pass the pending answer was never not pending,
    // and no Sync went out at all -- one in a hundred, measured, against a
    // hundred in a hundred idle. Sending Sync is what a master is for; a
    // peer-delay measurement is not worth a Sync. The peer sees a
    // Pdelay_Resp whose Follow_Up never comes and drops that one exchange,
    // which is what it does whenever the timestamp is late anyway. The
    // Sync before this one is given up on the same terms.
    if (peerResponsePending)
    {
        servicePeerFollowUp();
        peerResponsePending = false;
    }
    if (syncFollowUpPending)
    {
        serviceSyncFollowUp();
        syncFollowUpPending = false;
    }
    if (delayRequestPending)
    {
        serviceDelayRequestTimestamp();
        if (delayRequestPending)
        {
            abandonDelayRequest();
        }
    }
    constexpr uint16_t size = 44;
    uint8_t type=0;
    uint8_t control=0;
   
    uint8_t buf[size] = {0};

    // Taken before anything can go wrong with the send: a Sync that went
    // out has used its sequence ID, and the next one has to move on even
    // if this exchange is abandoned below. It used to be incremented only
    // on the way out of a successful exchange, so a missing transmit
    // timestamp made the next Sync repeat the sequence ID of this one.
    const uint16_t sequenceID = syncServerSequenceID++;

    initPTPMessage(buf, size, type, sequenceID, control);
    buf[6] =2;
    buf[33]=(uint8_t)logSyncInterval;
    armTxTimestamp();
    sendPTPMessage(buf,size,false,false);

    // The departure time is collected from update(), not waited for here.
    // See SYNC_FOLLOW_UP_TIMEOUT_MS.
    syncFollowUpPending = true;
    syncFollowUpSequenceID = sequenceID;
    syncFollowUpDeadlineMillis = millis() + SYNC_FOLLOW_UP_TIMEOUT_MS;
}

// Sends the Follow_Up once the hardware has the Sync's departure time, or
// gives it up when that never comes.
//
// A Sync with no Follow_Up is a Sync a two-step slave discards, which is
// the outcome wanted: no timestamp beats a made-up one.
void PTPBase::serviceSyncFollowUp()
{
    if (!syncFollowUpPending)
    {
        return;
    }

    struct timespec send_ts;
    if (!qindesign::network::EthernetIEEE1588.readAndClearTxTimestamp(send_ts))
    {
        if ((long)(millis() - syncFollowUpDeadlineMillis) >= 0)
        {
            syncFollowUpPending = false;
        }
        return;
    }
    syncFollowUpPending = false;

    const NanoTime departure = timespecToNanoTime(send_ts) + timestampOffset;
    nanoTimeToTimespec(departure,send_ts);
    if (logging)
    {
        Serial.print("T1s Delay send   timestamp=");
        printTime(departure);
    }
    followUpMessage(send_ts, syncFollowUpSequenceID);
}

void PTPBase::followUpMessage(const timespec &send_ts, uint16_t sequenceID)
{
    if(!initialised || portState != PortState::Master){
        return;
    }
    constexpr uint16_t size = 44;
    uint8_t type=8;
    uint8_t control=2;
   
    uint8_t buf[size] = {0};

    initPTPMessage(buf, size, type, sequenceID, control);
    buf[33]=(uint8_t)logSyncInterval;
    timespecToBuffer(send_ts,buf);
    sendPTPMessage(buf,size,true,false);
}

// A logMessageInterval as milliseconds: two to the power of it, seconds.
// The exponent is clamped where it is taken in, so the shift is always
// narrower than the type.
unsigned long PTPBase::logIntervalToMillis(int8_t logInterval)
{
    if (logInterval >= 0)
    {
        return 1000UL << logInterval;
    }
    return 1000UL >> (-logInterval);
}

// How long the slave waits for a Sync before it stops believing its own
// lock: three of the intervals the master announces.
unsigned long PTPBase::syncReceiptTimeoutMillis() const
{
    return logIntervalToMillis(logSyncIntervalFromMaster) * SYNC_RECEIPT_TIMEOUT_INTERVALS;
}

// The master has gone quiet. The lock goes with it, along with the
// exchange in progress and the timestamps a later one would otherwise be
// measured against.
//
// The frequency correction stays where it is: the clock carries on at the
// rate it last learned, which is the best it can do without a reference.
// So do the last offset and delay, as the last thing that was true.
void PTPBase::syncReceiptLost()
{
    syncReceiptValid = false;
    servo.lockCount = 0;

    syncCycleActive = false;
    syncPairMatched = false;
    requestOutstanding = false;
    delayResponseSeen = false;
    delayRequestPending = false;

    t1updated = false;
    t2updated = false;
    t3updated = false;
    t4updated = false;
    t5updated = false;
    t6updated = false;

    // The timestamps themselves, not only the flags: setT1 works out
    // whether the previous pair is usable from t1last and t2last, so
    // leaving them behind would have the first exchange after the gap
    // measured against timestamps taken before it.
    t1 = -1;
    t1last = -1;
    t2 = -1;
    t2last = -1;
    t2new = -1;
    t1lastvalid = false;
    t2lastvalid = false;

    if (logging)
    {
        Serial.println("No Sync received, lock lost");
    }
}

// The interval the requests go out at: the one the master asks for end to
// end, and the port's own configured one peer to peer.
//
// There is no master to ask in the peer-delay exchange -- it runs between
// neighbours, and the Pdelay_Resp carries 0x7f in the field the
// end-to-end answer uses to name a rate -- so the number
// setLogMinDelayReqInterval() holds, which a peer-delay port could
// otherwise only announce and never act on, is what paces it.
unsigned long PTPBase::delayRequestIntervalMillis() const
{
    return logIntervalToMillis(p2p ? logMinDelayReqIntervalAnnounced : logMinDelayReqInterval);
}

void PTPBase::scheduleNextDelayRequest()
{
    // Uniform over twice the interval, so the mean is the interval
    // itself and a segment full of slaves does not answer every Sync at
    // the same instant.
    const unsigned long interval = delayRequestIntervalMillis();
    // The board declares random(uint32_t), so the bound is built unsigned:
    // logMinDelayReqInterval is clamped to [-7, 7], which caps the interval
    // at 128 seconds and leaves the doubling nowhere near an overflow.
    nextDelayRequestMillis =
        millis() + random(static_cast<uint32_t>(2 * interval + 1));
}

void PTPBase::setT3(NanoTime ts){
	t3 = ts;
    t3updated = true;
    if (logging)
    {
        Serial.print("T3 Delay send    timestamp=");
        printTime(t3);
    }
}

bool PTPBase::delayRequestMessage()
{
    // The request takes the transmit timestamp for its own T3, so it
    // waits until whatever is already owed that register has had it.
    if (txTimestampOwed())
    {
        return false;
    }
    uint16_t size;
    uint8_t type;
    uint8_t control;
    if(!p2p){
        type=1;
        size=44;
        control=1;
    }else{
        type=2;
        size=54;
        control=5;
    }
    // A fixed buffer of the largest this can need, not a
    // variable-length one: `size` is 44 or 54 depending on the branch
    // above. A VLA also cannot be initialised in standard C++ -- clang
    // rejects `uint8_t buf[size] = {0}` outright.
    uint8_t buf[54] = {0};

    initPTPMessage(buf, size, type, delayRequestSequenceID, control);
    // Through armTxTimestamp(), which throws away whatever the register
    // still holds before arming it. Calling timestampNextFrame() on its
    // own left the door the Sync path had already closed: a wait that
    // gave up gets its timestamp posted a moment later, and the next read
    // -- this request's -- took it. T3 then said the Delay_Req left
    // before it did, and the whole path delay came out of that.
    armTxTimestamp();
    // A Pdelay_Req belongs on the peer-delay group; a Delay_Req does not.
    sendPTPMessage(buf,size,false,p2p);
    outstandingRequestSequenceID = delayRequestSequenceID;
    requestOutstanding = true;
    delayResponseSeen = false;
    requestT1 = t1;
    requestT2 = t2;
    delayRequestSequenceID++;

    // T3 is collected from update(), not waited for here. See
    // DELAY_REQUEST_TIMEOUT_MS.
    delayRequestPending = true;
    delayRequestDeadlineMillis = millis() + DELAY_REQUEST_TIMEOUT_MS;
    return true;
}

// The answer to the request goes with it: a T4 with no T3 of its own
// would wait to be paired with whatever T3 the next exchange produces,
// and the delay would come out of two different round trips.
void PTPBase::abandonDelayRequest()
{
    delayRequestPending = false;
    requestOutstanding = false;
    delayResponseSeen = false;
    t4updated = false;
    t6updated = false;
}

// Takes the departure time of the request just sent, or gives the
// exchange up when the hardware never posts it.
void PTPBase::serviceDelayRequestTimestamp()
{
    if (!delayRequestPending)
    {
        return;
    }

    struct timespec send_ts;
    if (!qindesign::network::EthernetIEEE1588.readAndClearTxTimestamp(send_ts))
    {
        if ((long)(millis() - delayRequestDeadlineMillis) >= 0)
        {
            abandonDelayRequest();
        }
        return;
    }
    delayRequestPending = false;
    setT3(timespecToNanoTime(send_ts));
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

void PTPBase::initPTPMessage(uint8_t *buf, const uint16_t messageLength, const uint8_t messageType, const uint16_t sequenceID, const uint8_t controlField)
{
    buf[0] = (uint8_t)((majorSdoId << 4) | (messageType & 0x0f));
    buf[1] = 2;
    buf[2] = static_cast<uint8_t>((messageLength >> 8) & 0x00ff);
    buf[3] = static_cast<uint8_t>(messageLength & 0x00ff);
    buf[4] = domainNumber;
    buf[5] = 0;
    buf[6] = 0;
    buf[7] = 0;
    buf[8] = 0;
    buf[9] = 0;
    buf[10] = 0;
    buf[11] = 0;
    buf[12] = 0;
    buf[13] = 0;
    buf[14] = 0;
    buf[15] = 0;
    buf[16] = 0;
    buf[17] = 0;
    buf[18] = 0;
    buf[19] = 0;
    for (int i = 0; i < 8; i++)
    {
        buf[20 + i] = clockID[i];  // sourcePortIdentity, clock identity
    }
    buf[28] = static_cast<uint8_t>((PORT_NUMBER >> 8) & 0x00ff);
    buf[29] = static_cast<uint8_t>(PORT_NUMBER & 0x00ff);
    buf[30] = static_cast<uint8_t>((sequenceID >> 8) & 0x00ff);
    buf[31] = static_cast<uint8_t>(sequenceID & 0x00ff);
    buf[32] = controlField;
    buf[33] = 0x7f;
}
