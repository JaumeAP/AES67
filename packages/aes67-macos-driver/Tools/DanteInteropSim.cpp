//
// DanteInteropSim.cpp
// AES67 macOS Driver - Tools
//
// Protocol-level interop dry-run against Dante, in both directions and under
// every compatibility profile. No network and no Dante hardware: a Dante
// device's AES67-mode announcement -- the SAP packet as Dante Controller's
// own SapMessages builds it, MIME payload type included, carrying the SDP a
// Dante device publishes when AES67 mode is on -- goes through THIS driver's
// SAP parser, SDP parser and profile validator; and the SAP packet this driver
// sends for a transmit stream is held against what Dante Controller 4.18.1.1
// needs to list it as an AES67 flow.
//
// What Dante needs was read out of the Controller bundle, not guessed:
//  - com/audinate/sdp/SdpDocument: "SDP document must contain version line
//    (v=)", "... origin line (o=)", "... timing information", "Failed to
//    parse media field - invalid port / invalid format".
//  - com/audinate/sap/SapMessages: MIME_SDP = "application/sdp", builds and
//    parses the RFC 2974 header, "SAP message too short (minimum 8 bytes)",
//    "Unsupported SAP version".
//  - com/audinate/aes67/DefaultRtpFlowAdvertisement: the fields a flow is
//    made of -- flowName (s=), originName (o= username), srcIpAddress and
//    sessionId (o=, the flow's identity), mediaTitle (i=), numChannels and
//    encoding and srate (a=rtpmap), ptpV2ClockDomain (a=ts-refclk, read with
//    Integer.parseInt in DiscoveredRtpFlows), clockOffset (a=mediaclk),
//    sourceIsDante (a=keywds:Dante), prim/sec destination (c=).
//  - libDanteController.dylib: "a=rtpmap:%u L%u/%u/%u", "a=ptime:%u.%u",
//    "a=mediaclk:direct=%u", "ts-refclk:ptp=", "recvonly", "keywds".
//  - com/audinate/ptp/Aes67ClockConfig: PTPv2 domain number, DSCP, TTL,
//    priority1/2 -- AES67 mode is PTPv2 on a domain, PTPv1 stays on for
//    Dante's own clocking.
//  - RouterLogic$SetAES67MulticastPrefixHandler: the 239.69.0.0/16 prefix
//    AES67 flows are addressed in by default.
//
// Same reason the RAVENNA one exists: it is the cheapest way to find the
// mismatch before the hardware does.
//
#include "Shared/CheckReport.h"
#include <exception>
#include "Driver/SDPParser.h"
#include "NetworkEngine/Discovery/SAPAnnouncer.h"
#include "NetworkEngine/Discovery/SAPListener.h"
#include "NetworkEngine/ProfileAdapter.h"
#include "NetworkEngine/RTP/PacketBudget.h"
#include "NetworkEngine/TxSession.h"
#include "Profiles/CompatibilityProfile.h"
#include "Profiles/PtpProfiles.h"

#include <arpa/inet.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace AES67;
using namespace AES67::CheckReport;

namespace {

bool has(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

/// The SAP packet Dante Controller's SapMessages.createAnnouncement() builds:
/// RFC 2974 version 1, IPv4 origin, no auth, then the "application/sdp" MIME
/// type as a NUL-terminated string, then the SDP. The MIME type is optional
/// in RFC 2974 and Dante sends it.
std::vector<uint8_t> danteSapPacket(const std::string& sdp, uint16_t hash, const char* origin) {
    std::vector<uint8_t> pkt;
    pkt.push_back(0x20); // V=1, A=0 (IPv4), R=0, T=0 (announce), E=0, C=0
    pkt.push_back(0);    // auth length
    pkt.push_back(static_cast<uint8_t>(hash >> 8));
    pkt.push_back(static_cast<uint8_t>(hash & 0xFF));
    in_addr src{};
    ::inet_pton(AF_INET, origin, &src);
    const uint8_t* s = reinterpret_cast<const uint8_t*>(&src.s_addr);
    pkt.insert(pkt.end(), s, s + 4);
    const char* mime = "application/sdp";
    pkt.insert(pkt.end(), mime, mime + std::strlen(mime) + 1); // with the NUL
    pkt.insert(pkt.end(), sdp.begin(), sdp.end());
    return pkt;
}

/// What a Dante device publishes for one AES67-mode transmit flow: the shape
/// every capture of a Dante AVIO, Ultimo or Brooklyn in AES67 mode shows.
/// Two channels here; Dante Controller splits anything wider into flows of at
/// most eight, "channels per bundle" in its own words.
std::string danteDeviceSdp(unsigned channels) {
    std::string channelList;
    for (unsigned c = 1; c <= channels; ++c) {
        if (c > 1) channelList += ", ";
        channelList += (c < 10 ? "0" : "") + std::to_string(c);
    }
    return
        "v=0\r\n"
        "o=- 1423986 1423994 IN IP4 169.254.98.63\r\n"
        "s=AVIOAI2-51f9a2 : " + std::to_string(channels) + "\r\n"
        "c=IN IP4 239.69.44.174/32\r\n"
        "t=0 0\r\n"
        "a=keywds:Dante\r\n"
        "m=audio 5004 RTP/AVP 97\r\n"
        "i=" + std::to_string(channels) + " channels: " + channelList + "\r\n"
        "a=recvonly\r\n"
        "a=rtpmap:97 L24/48000/" + std::to_string(channels) + "\r\n"
        "a=ptime:1\r\n"
        "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-50-1F-9A:0\r\n"
        "a=mediaclk:direct=0\r\n";
}

/// The transmit session this driver announces: announcedTxSession(), the same
/// call createTxStream() makes, plus the origin address AES67Device's
/// announcer fills in from the interface it announces on and a fixed session
/// id so two runs print the same thing.
///
/// Built by hand here until 2026-09-15, which made this simulation blind to
/// the only thing it is for: a change to what the driver actually announces
/// reached the driver and not the copy.
SDPSession ourTxSession(const std::string& name, const std::string& multicast, uint16_t channels) {
    SDPSession sdp = announcedTxSession(name, multicast, 5004, channels, 48000, /*dscp=*/-1);
    sdp.originAddress = "192.168.0.11";
    sdp.sessionID = 1757200000;
    return sdp;
}

std::string line(const std::string& sdp, const char* prefix) {
    const size_t at = sdp.find(prefix);
    if (at == std::string::npos) return "";
    const size_t end = sdp.find_first_of("\r\n", at);
    return sdp.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

} // namespace

int run() {
    std::printf("\n=== INTEROP SIM: macOS driver <-> Dante (Controller 4.18.1.1, AES67 mode) ===\n");

    // ------------------------------------------------------------------
    std::printf("\n[1] DANTE -> US: a Dante device's SAP announcement, through our listener\n");
    const std::string deviceSdp = danteDeviceSdp(2);
    const std::vector<uint8_t> pkt = danteSapPacket(deviceSdp, 0x1234, "169.254.98.63");
    const SAPAnnouncement heard = SAPListener::parseAnnouncement(
        reinterpret_cast<const char*>(pkt.data()), pkt.size(), "169.254.98.63");
    check("SAP parse", !heard.sessionDescription.empty(), "our SAPListener reads Dante's packet");
    check("SAP MIME type",
       heard.sessionDescription.rfind("v=0", 0) == 0,
       heard.sessionDescription.rfind("v=0", 0) == 0
           ? "the \"application/sdp\" payload type is stripped, the SDP starts at v=0"
           : "the SDP we keep starts with \"" + heard.sessionDescription.substr(0, 15) +
                 "\": Dante's \"application/sdp\" payload type was not stripped");
    check("SAP identity", heard.msgIdHash == 0x1234 && heard.originatingSource != 0,
       "message id hash and originating source read from the header");

    std::printf("\n[2] DANTE -> US: the SDP itself, through our parser\n");
    auto parsed = SDPParser::parseString(heard.sessionDescription.empty() ? deviceSdp
                                                                          : heard.sessionDescription);
    check("SDP parse", parsed.has_value(), "our SDPParser accepts a Dante AES67-mode SDP");
    if (parsed) {
        const auto& s = *parsed;
        check("session name", s.sessionName == "AVIOAI2-51f9a2 : 2", "\"" + s.sessionName + "\"");
        check("channels", s.numChannels == 2, std::to_string(s.numChannels) + " ch");
        check("encoding", s.encoding == "L24", s.encoding);
        check("sample rate", s.sampleRate == 48000, std::to_string(s.sampleRate) + " Hz");
        check("ptime", s.ptimeUs == 1000, std::to_string(s.ptimeUs) + " us");
        check("multicast", s.connectionAddress == "239.69.44.174", s.connectionAddress);
        check("PTP domain", s.ptpDomain == 0,
           "domain " + std::to_string(s.ptpDomain) + " from the bare \":0\" form Dante writes");
        check("grandmaster", s.ptpMasterMAC == "00-1D-C1-FF-FE-50-1F-9A", s.ptpMasterMAC);
        check("direction", s.direction == "recvonly", s.direction);
        const auto keywds = s.customAttributes.find("keywds");
        check("keywds:Dante", keywds != s.customAttributes.end() && keywds->second == "Dante",
           "kept as a custom attribute, so a UI can say the source is Dante");
    }

    std::printf("\n[3] DANTE -> US: the same flow under every profile (receive direction)\n");
    std::printf("    Dante's flow: L24, 48 kHz, 1 ms, 239.69.0.0/16, PTPv2 domain 0, up to 8 channels\n");
    if (parsed) {
        for (unsigned channels : {2u, 8u}) {
            auto flow = SDPParser::parseString(danteDeviceSdp(channels));
            if (!flow) { check("SDP parse", false, std::to_string(channels) + " channels"); continue; }
            std::printf("    -- %u channels\n", channels);
            for (const auto& profile : CompatibilityProfile::all()) {
                std::string err;
                const bool accepted = profile.validate(describeStream(*flow), /*isTransmit=*/false, &err);
                // A Dante flow is AES67's baseline; every profile that talks
                // AES67 at 48 kHz and 1 ms has to take it. The ones that do
                // not are listed with their reason, not counted as failures:
                // Level B wants 125 us packets, Dolby wants Dolby's addresses,
                // and DAC3202 only ever transmits.
                std::printf("  [%s] %-34s -- %s\n", accepted ? "ok" : "--",
                            profile.displayName.c_str(), accepted ? "accepts" : err.c_str());
                if (profile.kind == CompatibilityProfileKind::Dante ||
                    profile.kind == CompatibilityProfileKind::AES67 ||
                    profile.kind == CompatibilityProfileKind::RAVENNA ||
                    profile.kind == CompatibilityProfileKind::ST2110_30) {
                    check("must accept", accepted, profile.displayName);
                }
            }
        }
    }

    // ------------------------------------------------------------------
    std::printf("\n[4] US -> DANTE: the SAP packet we send, against Dante Controller's SapMessages\n");
    const SDPSession ours = ourTxSession("macOS AES67 TX", "239.69.1.10", 8);
    const std::string ourSdp = SDPParser::generate(ours);
    const std::vector<uint8_t> ourPkt =
        SAPAnnouncer::buildPacket(ourSdp, SAPAnnouncer::messageIdHash(ourSdp), 0x0B00A8C0, false);
    check("SAP length", ourPkt.size() >= 8, "at least the 8-byte header SapMessages requires");
    check("SAP version", ((ourPkt[0] >> 5) & 7) == 1, "version 1, the one SapMessages accepts");
    check("SAP address type", ((ourPkt[0] >> 4) & 1) == 0, "IPv4 origin");
    check("SAP not compressed/encrypted", (ourPkt[0] & 3) == 0, "plain SDP payload");
    {
        // This used to be an unsettled claim that we omit the payload type and
        // that only a live Controller could say whether Dante minds. We do not
        // omit it -- SAPAnnouncer::buildPacket writes it, because the AES67
        // Linux daemon drops a packet without it -- so the claim was false and
        // the check that is settled was not being made: our header has to carry
        // the same sixteen bytes at the same offset Dante's own does.
        const char* kType = "application/sdp";
        const size_t typeLen = std::strlen(kType) + 1;
        const bool ours = ourPkt.size() >= 8 + typeLen &&
                          std::memcmp(ourPkt.data() + 8, kType, typeLen) == 0;
        const bool theirs = pkt.size() >= 8 + typeLen &&
                            std::memcmp(pkt.data() + 8, kType, typeLen) == 0;
        check("SAP MIME type", ours && theirs,
           ours ? "\"application/sdp\" at offset 8, NUL included, as Dante's own packets carry it"
                : "absent from our header, where Dante's own packets carry it");
    }

    std::printf("\n[5] US -> DANTE: our SDP, against SdpDocument and DefaultRtpFlowAdvertisement\n");
    std::printf("%s\n", ourSdp.c_str());
    check("v= line", has(ourSdp, "v=0"), "\"SDP document must contain version line (v=)\"");
    const std::string origin = line(ourSdp, "o=");
    check("o= line present", !origin.empty(), "\"SDP document must contain origin line (o=)\"");
    {
        // o=<username> <sess-id> <sess-version> <nettype> <addrtype> <unicast-address>
        // Dante keys the flow on the o= address and session id
        // (RtpFlowIdentity: srcIpAddress, sessionId).
        unsigned fields = 0;
        for (size_t at = 2; at < origin.size();) {
            const size_t sp = origin.find(' ', at);
            if (sp == std::string::npos) { if (at < origin.size()) ++fields; break; }
            if (sp > at) ++fields;
            at = sp + 1;
        }
        check("o= field order", has(origin, " IN IP4 "), "\"" + origin + "\": nettype IN, then addrtype IP4");
        check("o= six fields", fields == 6,
           fields == 6 ? "\"" + origin + "\""
                       : "\"" + origin + "\" has " + std::to_string(fields) +
                             " fields: the unicast address is the flow's identity "
                             "(RtpFlowIdentity.srcIpAddress) and cannot be empty");
    }
    check("t= line", has(ourSdp, "t=0 0"), "\"SDP document must contain timing information\"");
    check("m= line", has(ourSdp, "m=audio 5004 RTP/AVP 97"),
       "\"Failed to parse media field - invalid port / invalid format\"");
    check("a=rtpmap", has(ourSdp, "a=rtpmap:97 L24/48000/8"),
       "matches the dylib's \"a=rtpmap:%u L%u/%u/%u\": integer rate, channel count last");
    check("a=ptime", has(ourSdp, "a=ptime:1\n") || has(ourSdp, "a=ptime:1\r"),
       "\"" + line(ourSdp, "a=ptime") + "\", the 1 ms Dante's AES67 mode is fixed at");
    check("a=mediaclk", has(ourSdp, "a=mediaclk:direct=0"),
       "the dylib's \"a=mediaclk:direct=%u\" -> clockOffset 0");
    check("a=recvonly", has(ourSdp, "a=recvonly"),
       "the direction a sender advertises: what a reader of this description can do with the "
       "stream, which is what a Dante device writes for its own flows too. It read a=sendonly "
       "for as long as the persisted role was inferred from this line");
    {
        const std::string refclk = line(ourSdp, "a=ts-refclk");
        if (refclk.empty()) {
            unsettled("a=ts-refclk",
                 "absent: createTxStream() sets no grandmaster, so SDPParser::generate() writes no "
                 "ts-refclk. Dante reads ptpV2ClockDomain from this line (DiscoveredRtpFlows, "
                 "Integer.parseInt); what it does with a flow that has none is not in the strings");
        } else {
            // With a grandmaster known, generate() writes RFC 7273's
            // "domain-nmbr=0"; Dante writes and parses the bare ":0" that
            // AES67-2018's own example uses, through Integer.parseInt.
            check("a=ts-refclk domain form", !has(refclk, "domain-nmbr="),
               "\"" + refclk + "\" -- Integer.parseInt(\"domain-nmbr=0\") throws; the bare \":0\" "
               "form every AES67 device writes is what Dante reads");
        }
    }
    {
        SDPSession withClock = ours;
        withClock.ptpMasterMAC = "00-60-2B-11-22-33-44-55";
        withClock.ptpDomain = 0;
        const std::string refclk = line(SDPParser::generate(withClock), "a=ts-refclk");
        check("a=ts-refclk once a grandmaster is known", !has(refclk, "domain-nmbr="),
           "\"" + refclk + "\"" +
               (has(refclk, "domain-nmbr=")
                    ? " -- RFC 7273's form; Dante's DiscoveredRtpFlows reads the domain with "
                      "Integer.parseInt, which does not take \"domain-nmbr=0\". Dante devices, "
                      "RAVENNA and the AES67 daemon all write the bare \":0\""
                    : ""));
    }
    check("no keywds:Dante", !has(ourSdp, "keywds:Dante"),
       "sourceIsDante stays false: we are an AES67 source, and say so");

    std::printf("\n[6] US -> DANTE: our transmit flows under every profile (what Dante gets)\n");
    std::printf("    Dante receives: L24, 48 kHz, 1 ms, at most 8 channels per flow, 239.69.0.0/16\n");
    for (const auto& profile : CompatibilityProfile::all()) {
        // The flow width StreamManager::createTxStreamFlows() would use at
        // 48 kHz and 1 ms under this profile: the smallest of the profile's
        // cap, the transport's and the frame budget.
        const uint16_t budget = PacketBudget::maxChannelsPerPacket(3, 48);
        const uint16_t perFlow = std::min<uint16_t>(std::min<uint16_t>(profile.maxChannelsPerFlow, 64), budget);
        const bool danteTakesIt = perFlow <= 8;
        const bool danteAddress = profile.requiredMulticastPrefix == "239.69";
        std::printf("  [%s] %-34s -- flows of %u ch%s%s\n",
                    danteTakesIt ? "ok" : "--", profile.displayName.c_str(), perFlow,
                    danteTakesIt ? "" : ": wider than Dante's 8, a Dante receiver refuses the flow",
                    danteAddress ? ", 239.69.x.x enforced" : ", any multicast address (239.69.x.x is up to the user)");
    }
    {
        const auto dante = CompatibilityProfile::forKind(CompatibilityProfileKind::Dante);
        std::string err;
        check("Dante profile accepts our flow", dante.validate(describeStream(ours), /*isTransmit=*/true, &err),
           err.empty() ? "8 ch L24 48 kHz 1 ms at 239.69.1.10" : err);
        SDPSession offPrefix = ours;
        offPrefix.connectionAddress = "239.1.1.2";
        err.clear();
        check("Dante profile refuses an address outside 239.69", !dante.validate(describeStream(offPrefix), true, &err), err);
        SDPSession rate96 = ours;
        rate96.sampleRate = 96000;
        err.clear();
        check("Dante profile refuses 96 kHz", !dante.validate(describeStream(rate96), true, &err), err);
    }

    std::printf("\n[7] CLOCK: Dante's AES67 mode against our PTP\n");
    {
        const auto dante = CompatibilityProfile::forKind(CompatibilityProfileKind::Dante);
        check("PTPv2 domain", dante.domainIsFixed && dante.fixedDomain == 0,
           "Aes67ClockConfig.mPtpV2DomainNumber defaults to 0; our profile pins domain 0");
        const PtpProfile* media = ptpProfileByName("aes67");
        check("PTP intervals", media != nullptr && media->settings.logSyncInterval == -3 &&
                                media->settings.logAnnounceInterval == 0,
           "Sync 8/s, Announce 1/s, domain 0: the AES67 media profile Dante's v2 port runs");
        check("audio DSCP", dante.recommendedDscp == 46, "Dante marks audio EF/46; so do we under this profile");
        unsettled("PTP DSCP", "Dante marks PTP CS7/56 (Aes67ClockConfig.mPtpV2Dscp); aes67ptpd sends unmarked unless --dscp is given");
        unsettled("PTPv1", "Dante keeps PTPv1 multicast for its own clocking (mPtpV1MulticastEnabled); we ignore v1, which is correct -- AES67 mode syncs on v2");
    }

    std::printf("\n=== RESULT: %s (%d checks failed, %d left to a live Controller) ===\n",
                CheckReport::failedCount == 0 ? "THEY CONNECT" : "MISMATCH",
                CheckReport::failedCount, CheckReport::unsettledCount);
    return CheckReport::failedCount == 0 ? 0 : 1;
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
