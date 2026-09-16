//
// AES67InteropSim.cpp
// AES67 macOS Driver - Tools
//
// Protocol-level interop dry-run against the original AES67 Linux daemon
// (RAVENNA aes67-linux-daemon, bondagit/aes67-linux-daemon): the packets the
// daemon puts on the wire go through THIS driver's own SAP listener, SDP
// parser and profile validator, and the packet this driver announces goes
// through the daemon's own receive rules, reporting per layer whether they
// connect. No network and no daemon process -- a reasoned simulation. It
// exists because it found a real bug: our SDP parser rejected the daemon's
// RFC 7273 bare-domain ts-refclk form (now fixed; regression-pinned in
// TestSDPParser).
//
// The daemon's half is Tests/support/DaemonSap.{h,cpp}, which mirrors SAP::send
// and SAP::receive from the daemon's own daemon/sap.cpp -- not restated here,
// so there is one oracle for what the daemon writes and accepts rather than
// two that can drift apart. What that oracle cannot settle is what a running
// daemon does with a packet it accepts, and that is
// Tools/AES67LiveDaemonInterop.cpp, driven against a real process in CI.
//
// This tool used to check two things it could not fail: an L24 payload encoded
// with our encoder and decoded with our decoder, and a SAP header it wrote
// into a local buffer and then read back out of it. Both passed whatever the
// daemon did. What replaced them is the daemon's real header, through the
// listener the driver actually runs, and back the other way.
//
#include <exception>

#include "Driver/SDPParser.h"
#include "NetworkEngine/Discovery/SAPAnnouncer.h"
#include "NetworkEngine/Discovery/SAPListener.h"
#include "NetworkEngine/ProfileAdapter.h"
#include "NetworkEngine/TxSession.h"
#include "Profiles/CompatibilityProfile.h"
#include "support/DaemonSap.h"

#include <arpa/inet.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace AES67;

namespace {

int fails = 0;
int unverified = 0;

void ok(const char* layer, bool cond, const std::string& detail) {
    std::printf("  [%s] %s -- %s\n", cond ? "OK" : "XX", layer, detail.c_str());
    if (!cond) fails++;
}

/// A claim this simulation cannot settle: printed, counted, never failed.
void unsettled(const char* layer, const std::string& detail) {
    std::printf("  [??] %s -- %s\n", layer, detail.c_str());
    unverified++;
}

bool has(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

std::string line(const std::string& sdp, const char* prefix) {
    const size_t at = sdp.find(prefix);
    if (at == std::string::npos) return "";
    const size_t end = sdp.find_first_of("\r\n", at);
    return sdp.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

/// The session description the daemon announces for one of its own sources:
/// a session name of "<name> : <channels>", the RAVENNA attributes it always
/// writes (sync-time, framecount, mediaclk, clock-domain) and the bare-domain
/// ts-refclk form -- the line this driver's parser used to reject.
std::string daemonSdp(unsigned channels) {
    return
        "v=0\r\n"
        "o=- 1443716955 1443716955 IN IP4 192.168.1.50\r\n"
        "s=AES67 daemon : " + std::to_string(channels) + "\r\n"
        "c=IN IP4 239.69.83.10/32\r\n"
        "t=0 0\r\n"
        "a=clock-domain:PTPv2 0\r\n"
        "m=audio 5004 RTP/AVP 98\r\n"
        "a=rtpmap:98 L24/48000/" + std::to_string(channels) + "\r\n"
        "a=sync-time:0\r\n"
        "a=framecount:48\r\n"
        "a=ptime:1\r\n"
        "a=mediaclk:direct=0\r\n"
        "a=ts-refclk:ptp=IEEE1588-2008:00-11-22-33-44-55-66-77:0\r\n"
        "a=recvonly\r\n";
}

/// The transmit session this driver announces: announcedTxSession(), the same
/// call createTxStream() makes, plus the origin address AES67Device's announcer
/// fills in from the interface it announces on and the grandmaster the PTP
/// layer supplies once it has one.
SDPSession ourTxSession() {
    SDPSession sdp = announcedTxSession("macOS AES67", "239.69.83.20", 5004, 8, 48000,
                                        /*dscp=*/-1);
    sdp.originAddress = "192.168.1.60";
    sdp.ptpDomain = 0;
    sdp.ptpMasterMAC = "00-60-2b-11-22-33";
    return sdp;
}

} // namespace

int run() {
    std::printf("\n=== INTEROP SIM: macOS driver <-> aes67-linux-daemon (RAVENNA) ===\n");

    // ------------------------------------------------------------------
    std::printf("\n[1] DAEMON -> US: its SAP announcement, through our listener\n");
    const std::string announced = daemonSdp(8);
    const uint16_t daemonHash = 0x1234;
    const std::vector<uint8_t> pkt = Tests::daemonSapPacket(
        announced, daemonHash, ::inet_addr("192.168.1.50"), /*deletion=*/false);
    const SAPAnnouncement heard = SAPListener::parseAnnouncement(
        reinterpret_cast<const char*>(pkt.data()), pkt.size(), "192.168.1.50");

    ok("SAP parse", !heard.sessionDescription.empty(),
       "our SAPListener reads the daemon's packet");
    ok("SAP payload type",
       heard.sessionDescription.rfind("v=0", 0) == 0,
       heard.sessionDescription.rfind("v=0", 0) == 0
           ? "the daemon's 24-byte header, \"application/sdp\" included, is stripped and the "
             "SDP starts at v=0"
           : "the SDP we keep starts with \"" + heard.sessionDescription.substr(0, 15) +
                 "\": the daemon's payload type was not stripped");
    ok("SAP body intact", heard.sessionDescription == announced,
       "every byte the daemon sent, and no more");
    {
        // The identity, read off the wire rather than off the value handed to
        // daemonSapPacket(): the daemon memcpy's the hash, so what it writes is
        // its host byte order, while this driver reads the two bytes
        // big-endian as RFC 2974 says. The two therefore disagree on the
        // NUMBER, which costs nothing -- both use it only as an opaque key --
        // and TestDaemonSAPInterop pins it. What matters here is that the
        // listener reads the bytes that are actually in the packet.
        const uint16_t onWire = static_cast<uint16_t>((pkt[2] << 8) | pkt[3]);
        char hex[16];
        std::snprintf(hex, sizeof(hex), "0x%04x", heard.msgIdHash);
        ok("SAP identity", heard.msgIdHash == onWire && heard.originatingSource != 0,
           std::string("hash ") + hex + " read big-endian off the header" +
               (onWire == daemonHash ? "" : ", which is the daemon's own memcpy byte-swapped"));
    }
    ok("SAP announcement, not deletion", !heard.isDeletion, "type bit clear");
    {
        const std::vector<uint8_t> bye = Tests::daemonSapPacket(
            announced, daemonHash, ::inet_addr("192.168.1.50"), /*deletion=*/true);
        const SAPAnnouncement gone = SAPListener::parseAnnouncement(
            reinterpret_cast<const char*>(bye.data()), bye.size(), "192.168.1.50");
        ok("SAP deletion", gone.isDeletion,
           "the daemon's 0x24 first byte withdraws the session instead of timing it out");
    }

    // ------------------------------------------------------------------
    std::printf("\n[2] DAEMON -> US: the SDP itself, through our parser\n");
    const auto parsed = SDPParser::parseString(
        heard.sessionDescription.empty() ? announced : heard.sessionDescription);
    ok("SDP parse", parsed.has_value(), "our SDPParser accepts the daemon's SDP");
    if (parsed) {
        const auto& s = *parsed;
        ok("session name", s.sessionName == "AES67 daemon : 8", "\"" + s.sessionName + "\"");
        ok("sample rate", s.sampleRate == 48000, std::to_string(s.sampleRate) + " Hz");
        ok("encoding", s.encoding == "L24", s.encoding);
        ok("channels", s.numChannels == 8, std::to_string(s.numChannels) + " ch");
        ok("ptime", s.ptimeUs == 1000, std::to_string(s.ptimeUs) + " us");
        ok("framecount", s.framecount == 48, std::to_string(s.framecount) + " frames");
        ok("multicast", s.connectionAddress == "239.69.83.10", s.connectionAddress);
        ok("port", s.port == 5004, std::to_string(s.port));
        ok("payload type", s.payloadType == 98, std::to_string(s.payloadType));
        ok("direction", s.direction == "recvonly", s.direction);
        ok("grandmaster", s.ptpMasterMAC == "00-11-22-33-44-55-66-77", s.ptpMasterMAC);
        ok("PTP domain", s.ptpDomain == 0,
           "domain " + std::to_string(s.ptpDomain) +
               " from the bare \":0\" form the daemon writes -- the line this parser "
               "used to reject outright");
    }

    // ------------------------------------------------------------------
    std::printf("\n[3] DAEMON -> US: the same flow under every profile (receive direction)\n");
    std::printf("    The daemon's flow: L24, 48 kHz, 1 ms, 239.69.83.10, PTPv2 domain 0\n");
    for (unsigned channels : {2u, 8u}) {
        const auto flow = SDPParser::parseString(daemonSdp(channels));
        if (!flow) {
            ok("SDP parse", false, std::to_string(channels) + " channels");
            continue;
        }
        std::printf("    -- %u channels\n", channels);
        for (const auto& profile : CompatibilityProfile::all()) {
            std::string err;
            const bool accepted = profile.validate(describeStream(*flow), /*isTransmit=*/false, &err);
            // The daemon is a RAVENNA implementation of AES67's baseline, so
            // those two profiles have to take it. The ones that do not are
            // listed with their reason rather than counted as failures: Level B
            // wants 125 us packets, Dolby wants Dolby's addresses, and DAC3202
            // only ever transmits.
            std::printf("  [%s] %-34s -- %s\n", accepted ? "ok" : "--",
                        profile.displayName.c_str(), accepted ? "accepts" : err.c_str());
            if (profile.kind == CompatibilityProfileKind::AES67 ||
                profile.kind == CompatibilityProfileKind::RAVENNA) {
                ok("must accept", accepted, profile.displayName);
            }
        }
    }

    // ------------------------------------------------------------------
    std::printf("\n[4] US -> DAEMON: the SAP packet we send, against the daemon's receive path\n");
    const SDPSession ours = ourTxSession();
    const std::string ourSdp = SDPParser::generate(ours);
    const std::vector<uint8_t> ourPkt = SAPAnnouncer::buildPacket(
        ourSdp, SAPAnnouncer::messageIdHash(ourSdp), ::inet_addr("192.168.1.60"), false);
    {
        const Tests::DaemonSapRead read = Tests::daemonSapRead(ourPkt.data(), ourPkt.size());
        ok("daemon accepts our packet", read.accepted,
           read.accepted ? "header, payload type and body all pass SAP::receive" : read.refusal);
        ok("read as an announcement", read.isAnnouncement, "not a deletion");
        ok("body arrives whole", read.sdp == ourSdp,
           read.sdp == ourSdp ? "the SDP it takes off the wire is the one we wrote"
                              : "the daemon reads " + std::to_string(read.sdp.size()) +
                                    " bytes of the " + std::to_string(ourSdp.size()) + " we sent");
    }
    {
        const std::vector<uint8_t> bye = SAPAnnouncer::buildPacket(
            ourSdp, SAPAnnouncer::messageIdHash(ourSdp), ::inet_addr("192.168.1.60"), true);
        const Tests::DaemonSapRead read = Tests::daemonSapRead(bye.data(), bye.size());
        ok("our deletion is a deletion to it", read.accepted && !read.isAnnouncement,
           read.accepted ? "type bit set, everything else unchanged" : read.refusal);
    }

    // ------------------------------------------------------------------
    std::printf("\n[5] US -> DAEMON: our SDP, against the shape the daemon announces\n");
    std::printf("%s\n", ourSdp.c_str());
    ok("v= line", has(ourSdp, "v=0"), "the version line every SDP reader demands first");
    {
        const std::string origin = line(ourSdp, "o=");
        unsigned fields = 0;
        for (size_t at = 2; at < origin.size();) {
            const size_t sp = origin.find(' ', at);
            if (sp == std::string::npos) { if (at < origin.size()) ++fields; break; }
            if (sp > at) ++fields;
            at = sp + 1;
        }
        ok("o= six fields", fields == 6,
           "\"" + origin + "\"" + (fields == 6 ? "" : " has " + std::to_string(fields) +
                                                          " fields, not six"));
        ok("o= field order", has(origin, " IN IP4 "), "nettype IN, then addrtype IP4");
    }
    ok("s= line", has(ourSdp, "s=macOS AES67"), line(ourSdp, "s="));
    ok("c= line", has(ourSdp, "c=IN IP4 239.69.83.20"), line(ourSdp, "c="));
    ok("t= line", has(ourSdp, "t=0 0"), "an unbounded session, as the daemon announces its own");
    ok("m= line", has(ourSdp, "m=audio 5004 RTP/AVP 97"), line(ourSdp, "m=audio"));
    ok("a=rtpmap", has(ourSdp, "a=rtpmap:97 L24/48000/8"), line(ourSdp, "a=rtpmap"));
    ok("a=ptime", has(ourSdp, "a=ptime:1\r") || has(ourSdp, "a=ptime:1\n"),
       "\"" + line(ourSdp, "a=ptime") + "\", the 1 ms the daemon runs at by default");
    {
        const std::string refclk = line(ourSdp, "a=ts-refclk");
        ok("a=ts-refclk present", !refclk.empty(), refclk);
        ok("a=ts-refclk domain form", !has(refclk, "domain-nmbr="),
           "\"" + refclk + "\"" +
               (has(refclk, "domain-nmbr=")
                    ? " -- RFC 7273's form; the daemon writes and reads the bare \":0\", which is "
                      "the mismatch this tool found in the other direction"
                    : " -- the bare domain the daemon itself writes"));
    }
    unsettled("a=sync-time",
              "the daemon writes it on its own announcements and this driver writes none (it does "
              "write the a=clock-domain beside it); whether the daemon's receive path needs one is "
              "not in the source DaemonSap mirrors, so only Tools/AES67LiveDaemonInterop against a "
              "running daemon settles it");
    unsettled("RTSP DESCRIBE",
              "the daemon fetches a session it has heard about over RTSP; that surface is "
              "Tests/support/DaemonRtsp and the live tool, not this one");

    std::printf("\n=== RESULT: %s (%d checks failed, %d left to a live daemon) ===\n",
                fails == 0 ? "THEY CONNECT" : "MISMATCH", fails, unverified);
    return fails == 0 ? 0 : 1;
}

// main only guards run(): a tool that dies on an uncaught exception prints
// "libc++abi: terminating" and nothing about what it was doing.
int main() {
    try {
        return run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
