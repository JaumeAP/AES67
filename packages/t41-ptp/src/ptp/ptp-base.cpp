// PTPBase: the port, its state machine and its loop.
//
// The protocol logic that is not parsing, not sending, not configuration and
// not clock discipline -- those are ptp-parse.cpp, ptp-send.cpp,
// ptp-config.cpp and ptp-discipline.cpp, and all five are the same class. What
// is here is begin() and end(), update(), reset(), the port state machine and
// the BMCA decision it rests on, and the timeouts that drive both.
//
// The shared helpers the five files use are declared in ptp-internal.h and
// defined here.

#include <Arduino.h>
#include <QNEthernet.h>
#include <TimeLib.h>

#include "ptp-base.h"
#include "ptp-internal.h"

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
    // One pass, six phases, in the order they have to happen in. The order is
    // not obvious and every part of it was paid for: draining an answer after
    // the next request has gone out, or correcting the clock before the delay
    // it is corrected against has been refreshed, are both defects this file
    // has had. Each phase carries the comment saying which.
    if (!initialised)
    {
        return;
    }

    // Before anything else that could take the transmit timestamp out
    // of the hardware.
    serviceTxTimestamps();

    updateSockets();
    serviceTxTimestamps();

    serviceDelayExchange();
    serviceDelayRequestPacing();
    serviceSyncPair();
    serviceAnnounceTimeout();
    serviceSyncReceipt();
    serviceExternalReference();
}

// A finished delay exchange, drained before the next request goes out.
//
// A phase of update(), which calls these in the order it lists them.
void PTPBase::serviceDelayExchange()
{
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
}

// Whether a delay request is due, and sending it when it is.
//
// A phase of update(), which calls these in the order it lists them.
void PTPBase::serviceDelayRequestPacing()
{
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
}

// A matched Sync pair, which is what corrects the clock.
//
// A phase of update(), which calls these in the order it lists them.
void PTPBase::serviceSyncPair()
{

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
}

// The chosen master going quiet, and the port taking the choice back.
//
// A phase of update(), which calls these in the order it lists them.
void PTPBase::serviceAnnounceTimeout()
{

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
}

// The master's Sync going quiet, which ends the lock.
//
// A phase of update(), which calls these in the order it lists them.
void PTPBase::serviceSyncReceipt()
{

    // Nothing from the master for three of its own intervals means the
    // lock says nothing about the clock any more.
    if (slave && syncReceiptValid &&
        (millis() - lastSyncMillis) > syncReceiptTimeoutMillis())
    {
        syncReceiptLost();
    }
}

// The PPS pin, read with the interrupt held off.
//
// A phase of update(), which calls these in the order it lists them.
void PTPBase::serviceExternalReference()
{

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
