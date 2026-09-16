//
// TxSession.h
// AES67 macOS Driver - NetworkEngine
//
// The session a transmit stream is announced with: the one place its fields
// are decided, called by StreamManager::createTxStream() and by the interop
// simulations that hold what this driver announces against a peer's rules.
//
// A header rather than a member of StreamManager, which is where the fields
// used to be set inline: a tool that only reasons about bytes would otherwise
// have to link StreamManager, and with it the RTP transmitter, the receiver
// and the real-time thread priority helper -- an audio stack, to build an SDP.
//
// Tools/DanteInteropSim and Tools/AES67InteropSim carried a copy of these
// fields built by hand, one of them claiming to mirror createTxStream() "field
// for field". A change to what the driver actually announces reached the
// driver and not the simulations that exist to check it.
//
#pragma once

#include "Driver/SDPParser.h"

#include <cstdint>
#include <ctime>
#include <string>

namespace AES67 {

/// The origin address is deliberately not a parameter: it is the interface the
/// announcer sends on, which SAPAnnouncer fills in, and nothing that builds a
/// session has any business guessing it.
inline SDPSession announcedTxSession(const std::string& name,
                                     const std::string& multicastIP,
                                     uint16_t port,
                                     uint16_t numChannels,
                                     uint32_t sampleRate,
                                     int dscp) {
    SDPSession sdp;
    sdp.sessionName = name;
    sdp.connectionAddress = multicastIP;
    sdp.port = port;
    sdp.numChannels = numChannels;
    sdp.sampleRate = sampleRate;
    sdp.encoding = "L24"; // Use L24 for best quality
    sdp.payloadType = 97; // Dynamic payload type
    sdp.sessionID = static_cast<uint64_t>(std::time(nullptr));
    sdp.sessionVersion = 1;
    sdp.dscp = dscp; // -1 = inherit the active profile's DSCP (createTransmitter)
    // SDPSession defaults this to "recvonly", and building the session field
    // by field left every transmit stream carrying that default. Two
    // consumers read it: saveAllStreamsInternal() persists it, and
    // loadSavedStreams() decides isTransmit from it -- so after one restart
    // the driver built an RTPReceiver on its own transmit group instead of the
    // transmitter, which is silent, total TX loss with the channels counted as
    // RX. The other is SDPParser's writer, which put `a=recvonly` into the SAP
    // announcement, the RTSP DESCRIBE body and the IS-05 sender transport file.
    sdp.direction = "sendonly";
    return sdp;
}

} // namespace AES67
