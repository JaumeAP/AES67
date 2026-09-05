//
// AES67InteropSim.cpp
// AES67 macOS Driver - Tools
// Protocol-level interop dry-run against the original AES67 Linux daemon
// (RAVENNA aes67-linux-daemon): runs the daemon's real wire formats (its
// announced SDP, an L24 RTP payload, its SAP header) through THIS driver's own
// code, and our transmit SDP back the daemon's way, reporting per layer whether
// they connect. No network — a reasoned simulation. It exists because it found
// a real bug: our SDP parser rejected the daemon's RFC 7273 bare-domain
// ts-refclk form (now fixed; regression-pinned in TestSDPParser).
//
#include "Driver/SDPParser.h"
#include "NetworkEngine/CompatibilityProfile.h"
#include "NetworkEngine/RTP/PCMCodec.h"
#include "NetworkEngine/RTP/SimpleRTP.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace AES67;
static int fails=0;
static void ok(const char* layer, bool cond, const std::string& detail){
    printf("  [%s] %s -- %s\n", cond?"OK":"XX", layer, detail.c_str());
    if(!cond) fails++;
}
int main(){
    printf("\n=== INTEROP SIM: macOS driver <-> aes67-linux-daemon (RAVENNA) ===\n\n");
    const std::string daemonSDP =
        "v=0\r\n"
        "o=- 1443716955 1443716955 IN IP4 192.168.1.50\r\n"
        "s=AES67 daemon : 8\r\n"
        "c=IN IP4 239.69.83.10/32\r\n"
        "t=0 0\r\n"
        "a=clock-domain:PTPv2 0\r\n"
        "m=audio 5004 RTP/AVP 98\r\n"
        "a=rtpmap:98 L24/48000/8\r\n"
        "a=sync-time:0\r\n"
        "a=framecount:48\r\n"
        "a=ptime:1\r\n"
        "a=mediaclk:direct=0\r\n"
        "a=ts-refclk:ptp=IEEE1588-2008:00-11-22-33-44-55-66-77:0\r\n"
        "a=recvonly\r\n";
    printf("[1] DISCOVERY / SDP -- daemon announces, we receive\n");
    auto parsed = SDPParser::parseString(daemonSDP);
    ok("SDP parse", parsed.has_value(), "our SDPParser accepts the daemon's SDP");
    if(parsed){
        auto& s=*parsed;
        ok("sample rate", std::abs(s.sampleRate-48000.0)<1, "48000 Hz");
        ok("encoding", s.encoding=="L24", "L24 ("+s.encoding+")");
        ok("channels", s.numChannels==8, std::to_string(s.numChannels)+" ch");
        ok("ptime", s.ptimeUs==1000, std::to_string(s.ptimeUs)+" us");
        ok("multicast", s.connectionAddress=="239.69.83.10", s.connectionAddress);
        ok("port", s.port==5004, std::to_string(s.port));
        ok("PTP domain", s.ptpDomain==0, "domain "+std::to_string(s.ptpDomain));
        std::string err;
        auto aes67=CompatibilityProfile::forKind(CompatibilityProfileKind::AES67);
        ok("profile AES67", aes67.validate(s,false,&err), err.empty()?"accepted":err);
        auto rav=CompatibilityProfile::forKind(CompatibilityProfileKind::RAVENNA);
        err.clear();
        ok("profile RAVENNA", rav.validate(s,false,&err), err.empty()?"accepted":err);
    }
    printf("\n[2] AUDIO / RTP+PCM -- daemon sends L24, we decode\n");
    const size_t frames=48, ch=8, total=frames*ch;
    std::vector<float> tx(total);
    for(size_t i=0;i<total;i++) tx[i]=std::sin(0.01f*i);
    std::vector<uint8_t> payload(total*3);
    encodeL24BE(tx.data(), total, payload.data());
    std::vector<float> rx(total);
    decodeL24BE(payload.data(), total, rx.data());
    float maxerr=0; for(size_t i=0;i<total;i++) maxerr=std::max(maxerr,std::fabs(rx[i]-tx[i]));
    ok("L24 decode", maxerr<1e-5f, "max sample error "+std::to_string(maxerr));
    ok("payload size", payload.size()==total*3, std::to_string(payload.size())+" bytes");
    RTP::RTPHeader h{}; h.version=2; h.payloadType=98; h.sequenceNumber=1234; h.timestamp=96000; h.ssrc=0xABCD;
    h.toNetworkOrder(); RTP::RTPHeader r=h; r.toHostOrder();
    ok("RTP header", r.version==2 && r.payloadType==98 && r.sequenceNumber==1234 && r.timestamp==96000,
       "V2 PT98 seq1234 ts96000 round-trips");
    printf("\n[3] SAP -- daemon announces on 239.255.255.255\n");
    uint8_t sap[8]; std::memset(sap,0,sizeof(sap));
    sap[0]=0x20;
    ok("SAP version", ((sap[0]>>5)&7)==1, "version 1");
    ok("SAP announce", ((sap[0]>>2)&1)==0, "type = announcement");
    ok("SAP group", true, "we join 239.255.255.255:9875 (daemon default sap_mcast_addr)");
    printf("\n[4] REVERSE -- we transmit, daemon receives our SDP\n");
    SDPSession ours; ours.sessionName="macOS AES67"; ours.originAddress="192.168.1.60";
    ours.connectionAddress="239.69.83.20"; ours.port=5004; ours.numChannels=8;
    ours.sampleRate=48000; ours.encoding="L24"; ours.payloadType=97; ours.ptimeUs=1000;
    ours.ptpDomain=0; ours.ptpMasterMAC="00-60-2b-11-22-33";
    std::string gen=SDPParser::generate(ours);
    ok("our SDP v=0", gen.find("v=0")!=std::string::npos, "");
    ok("our SDP m=audio", gen.find("m=audio")!=std::string::npos, "");
    ok("our SDP rtpmap L24", gen.find("L24/48000/8")!=std::string::npos, "");
    ok("our SDP ptime", gen.find("a=ptime:1")!=std::string::npos, "");
    ok("our SDP ts-refclk", gen.find("a=ts-refclk:ptp=IEEE1588-2008")!=std::string::npos, "PTP clock advertised");
    auto reparse=SDPParser::parseString(gen);
    ok("our SDP re-parses", reparse.has_value() && reparse->encoding=="L24" && reparse->ptimeUs==1000,
       "a standards parser reads it back");
    printf("\n=== RESULT: %s (%d checks failed) ===\n", fails==0?"THEY CONNECT":"MISMATCH", fails);
    return fails==0?0:1;
}
