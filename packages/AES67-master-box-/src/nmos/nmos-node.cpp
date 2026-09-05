//
// nmos-node.cpp
// t41-ptp
//

#include "nmos/nmos-node.h"

#include <cstdio>
#include <cstring>

namespace {

/// The status code out of an HTTP status line, or 0 when the line is not
/// one. No std::stoi anywhere near a board: it throws, and this parses
/// what another machine sent.
int statusFromLine(const char *line, size_t length)
{
    // "HTTP/1.1 201 Created"
    const char *space = static_cast<const char *>(memchr(line, ' ', length));
    if (space == nullptr) return 0;
    int status = 0;
    int digits = 0;
    for (const char *p = space + 1; p < line + length && *p >= '0' && *p <= '9'; ++p)
    {
        status = status * 10 + (*p - '0');
        if (++digits > 3) return 0;
    }
    return digits == 3 ? status : 0;
}

/// Copies a string into a JSON string body, escaping what JSON cannot
/// carry raw. The label comes from the sketch, and a label holding a
/// quote or a backslash built a body no registry could parse -- the
/// registration then failed for a reason nothing here could report.
void escapeJson(const char *in, char *out, size_t size)
{
    size_t at = 0;
    for (const char *p = in; *p != '\0' && at + 2 < size; ++p)
    {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '"' || c == '\\')
        {
            out[at++] = '\\';
            out[at++] = static_cast<char>(c);
        }
        else if (c < 0x20)
        {
            // Control characters need a six-character escape; skipping
            // them keeps the body well formed without a length surprise.
            continue;
        }
        else
        {
            out[at++] = static_cast<char>(c);
        }
    }
    out[at] = '\0';
}

} // namespace

NMOSNode::NMOSNode(PTPBase &ptp_) : ptp(ptp_) {}

void NMOSNode::buildNodeId()
{
    // A UUID's worth of bytes out of the clock identity, which is this
    // board's MAC mapped to EUI-64: the same board is the same node after
    // a reboot, with nothing stored anywhere. The version and variant
    // nibbles are forced so what goes on the wire is a well-formed UUID.
    const uint8_t *id = ptp.getClockIdentity();
    uint8_t bytes[16];
    for (int i = 0; i < 8; i++)
    {
        bytes[i] = id[i];
        // The second half is the first, inverted: a UUID needs sixteen
        // bytes and this identity has eight. Deriving rather than padding
        // with zeros keeps two boards with adjacent MACs apart in more
        // than one place.
        bytes[i + 8] = static_cast<uint8_t>(~id[i]);
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);

    snprintf(nodeId, sizeof(nodeId),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);

    if (label[0] == '\0')
    {
        snprintf(label, sizeof(label), "t41-ptp %.8s", nodeId);
    }
}

void NMOSNode::begin(const IPAddress &registry, uint16_t port)
{
    registryAddress = registry;
    registryPort = port;
    started = true;
    registered = false;
    beaten = false;
    lastBeatMillis = 0;
    buildNodeId();
}

void NMOSNode::setLabel(const char *newLabel)
{
    if (newLabel == nullptr) return;
    snprintf(label, sizeof(label), "%s", newLabel);
}

int NMOSNode::request(const char *method, const char *path, const char *body)
{
    qindesign::network::EthernetClient client;
    client.setConnectionTimeout(CONNECT_TIMEOUT_MS);
    if (!client.connect(registryAddress, registryPort))
    {
        return 0;
    }

    const size_t bodyLength = (body == nullptr) ? 0 : strlen(body);

    // The authority, with the port when it is not the default one. It
    // used to be the address alone whatever port begin() was given, and a
    // registry is rarely on 80 -- nmos-cpp listens on 8010 and 8235. RFC
    // 7230 makes the port part of Host, and a registry behind a proxy or
    // sharing a host with anything else routes the request by it.
    char host[32];
    if (registryPort == 80)
    {
        snprintf(host, sizeof(host), "%u.%u.%u.%u", registryAddress[0], registryAddress[1],
                 registryAddress[2], registryAddress[3]);
    }
    else
    {
        snprintf(host, sizeof(host), "%u.%u.%u.%u:%u", registryAddress[0], registryAddress[1],
                 registryAddress[2], registryAddress[3], registryPort);
    }

    char head[256];
    const int headLength = snprintf(head, sizeof(head),
                                    "%s %s HTTP/1.1\r\n"
                                    "Host: %s\r\n"
                                    "Content-Type: application/json\r\n"
                                    "Content-Length: %u\r\n"
                                    "Connection: close\r\n\r\n",
                                    method, path, host,
                                    static_cast<unsigned>(bodyLength));
    // Truncation is a failure, not something to send anyway. snprintf
    // returns the length it WOULD have written, and only the negative
    // case was tested: a header that did not fit was handed to
    // writeFully() with that larger length, which reads past the buffer
    // and puts whatever follows it on the wire. The old 192 bytes left
    // one byte spare on a DELETE to a fifteen-character address, so a
    // longer API version or one more header would have reached it.
    if (headLength <= 0 || static_cast<size_t>(headLength) >= sizeof(head))
    {
        client.stop();
        return 0;
    }

    client.writeFully(head, static_cast<size_t>(headLength));
    if (bodyLength > 0) client.writeFully(body, bodyLength);
    client.flush();

    // The status line and nothing else: the answer's body says nothing
    // this needs, and reading it all would hold loop() for longer.
    char line[64];
    size_t length = 0;
    // Compared as a difference, not as two absolute values: millis()
    // wraps every 49.7 days, and `millis() < deadline` across the wrap is
    // false on the first test, so the answer was never read at all. Every
    // registration and every heartbeat in that window came back as 0 --
    // unreachable -- and the node dropped out of the registry it was
    // talking to perfectly well.
    const unsigned long deadline = millis() + CONNECT_TIMEOUT_MS;
    while ((long)(millis() - deadline) < 0 && length < sizeof(line))
    {
        const int c = client.read();
        if (c < 0)
        {
            if (!client.connected()) break;
            continue;
        }
        if (c == '\r' || c == '\n') break;
        line[length++] = static_cast<char>(c);
    }
    client.stop();

    return statusFromLine(line, length);
}

bool NMOSNode::postNode()
{
    // The clock is what this node HAS: a board carrying PTP and no audio.
    // "ptp" while the port is locked and the lock still means something,
    // "internal" otherwise -- a node claiming a PTP clock it is not
    // holding is a node a controller will slave things to.
    const bool locked = ptp.getLockCount() > 0 && ptp.isSyncReceiptValid();

    // IS-04 versions a resource by the time it changed, as
    // "<seconds>:<nanoseconds>" TAI, and a registry keeps the newest it
    // has seen. millis() is not that: it counts from this boot, so the
    // version a board sends after a restart is SMALLER than the one the
    // registry already holds for it, and the registration is refused or
    // ignored as stale for as long as the board has been up. The PTP
    // clock is the one thing on this board that does carry TAI, and it is
    // the reason the board exists.
    timespec resourceTime = {0, 0};
    if (!qindesign::network::EthernetIEEE1588.readTimer(resourceTime))
    {
        resourceTime.tv_sec = 0;
        resourceTime.tv_nsec = 0;
    }

    char safeLabel[sizeof(label) * 2];
    escapeJson(label, safeLabel, sizeof(safeLabel));

    char body[768];
    const int length = snprintf(
        body, sizeof(body),
        "{\"type\":\"node\",\"data\":{"
        "\"id\":\"%s\","
        "\"version\":\"%lu:%lu\","
        "\"label\":\"%s\","
        "\"description\":\"IEEE 1588 clock on a Teensy 4.1\","
        "\"tags\":{},"
        "\"href\":\"\","
        "\"hostname\":\"\","
        "\"caps\":{},"
        "\"api\":{\"versions\":[\"%s\"],\"endpoints\":[]},"
        "\"services\":[],"
        "\"clocks\":[{\"name\":\"clk0\",\"ref_type\":\"%s\"%s}],"
        "\"interfaces\":[]"
        "}}",
        nodeId, static_cast<unsigned long>(resourceTime.tv_sec),
        static_cast<unsigned long>(resourceTime.tv_nsec), safeLabel, API_VERSION,
        locked ? "ptp" : "internal",
        locked ? ",\"traceable\":false,\"version\":\"IEEE1588-2008\",\"locked\":true" : "");
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(body))
    {
        return false;
    }

    char path[64];
    snprintf(path, sizeof(path), "/x-nmos/registration/%s/resource", API_VERSION);

    const int status = request("POST", path, body);
    // 201 is a new registration, 200 the registry recognising an id it
    // already holds. Both mean it took.
    registered = (status == 200 || status == 201);
    if (!registered) failureCount++;
    return registered;
}

bool NMOSNode::heartbeat()
{
    char path[96];
    snprintf(path, sizeof(path), "/x-nmos/registration/%s/health/nodes/%s", API_VERSION, nodeId);

    const int status = request("POST", path, "");
    if (status == 200) return true;

    // 404 is the registry saying it has forgotten this node, which is
    // what it says after collecting one that went quiet. Registering
    // again is the way back in; anything else is counted and retried on
    // the next beat.
    registered = false;
    failureCount++;
    if (status == 404) return postNode();
    return false;
}

void NMOSNode::update()
{
    if (!started) return;

    const unsigned long now = millis();
    if (beaten && (now - lastBeatMillis) < HEARTBEAT_INTERVAL_MS)
    {
        return;
    }
    beaten = true;
    lastBeatMillis = now;

    if (!registered)
    {
        postNode();
        return;
    }
    heartbeat();
}

void NMOSNode::unregisterNode()
{
    if (!started || !registered) return;

    char path[96];
    snprintf(path, sizeof(path), "/x-nmos/registration/%s/resource/nodes/%s", API_VERSION, nodeId);
    request("DELETE", path, "");
    registered = false;
    // The node is off the registry and stays off: update() registered it
    // again on its very next call, so a sketch that unregisters before a
    // planned reboot and goes on calling update() -- which is where it is
    // called from -- put the node straight back in. begin() again to
    // register again.
    started = false;
}
