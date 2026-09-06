// PTPBase, the parsers.
//
// Everything that reads bytes off the wire: the dispatch that checks the
// version, the profile, the domain and each message's own minimum length
// before a field of it is touched, the parser for each message type, and the
// four timestamp setters they feed.
//
// Nothing here transmits. The length guards are the reason this file is worth
// reading on its own: every one of them was added to close a defect, and each
// carries the comment saying which.
//
// Split out of ptp-base.cpp, which held the whole class in two thousand lines.

#include <Arduino.h>
#include <QNEthernet.h>
#include <TimeLib.h>

#include "ptp-base.h"
#include "ptp-internal.h"

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

void PTPBase::setT3(NanoTime ts){
	t3 = ts;
    t3updated = true;
    if (logging)
    {
        Serial.print("T3 Delay send    timestamp=");
        printTime(t3);
    }
}
