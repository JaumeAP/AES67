// PTPBase, the senders.
//
// Everything that puts bytes on the wire: the common header every message
// starts from, Announce, Sync and its Follow_Up, the delay request and its
// answer in both mechanisms, and the transmit-timestamp servicing that turns a
// sent message into the timestamp the exchange needs.
//
// Nothing here parses. The transports supply sendPTPMessage(); this decides
// what to hand it and when.
//
// Split out of ptp-base.cpp, which held the whole class in two thousand lines.

#include <Arduino.h>
#include <QNEthernet.h>
#include <TimeLib.h>

#include "ptp-base.h"
#include "ptp-internal.h"

void PTPBase::serviceTxTimestamps()
{
    servicePeerFollowUp();
    serviceSyncFollowUp();
    serviceDelayRequestTimestamp();
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
