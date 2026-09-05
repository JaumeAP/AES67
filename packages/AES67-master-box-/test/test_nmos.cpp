//
// test_nmos.cpp
// What this board tells an NMOS registry about itself.
//
// A registry is an inventory: what goes in it is what a plant believes
// exists. So the two things worth pinning down are that the node is the
// SAME node after a reboot -- an id derived from the clock identity, with
// nothing stored -- and that the clock it claims is the clock it has. A
// board announcing a PTP clock while it is free-running is a board a
// controller will slave a room to.
//
#include "nmos/nmos-node.h"
#include "ptp_messages.h"
#include "stubs/stub_state.h"
#include "test_harness.h"

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

class NmosTestPTP : public PTPBase
{
public:
    NmosTestPTP(bool master_, bool slave_, bool p2p_) : PTPBase(master_, slave_, p2p_) {}

private:
    void initSockets() override {}
    void closeSockets() override {}
    void updateSockets() override {}
    void sendPTPMessage(const uint8_t *, int, bool, bool) override {}
};

const std::string &lastRequest()
{
    static const std::string empty;
    const auto &requests = ptptest::state().tcpRequests;
    return requests.empty() ? empty : requests.back();
}

bool contains(const std::string &haystack, const char *needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

static void testTheNodeIdComesFromTheClockIdentity()
{
    ptptest::state().reset();
    NmosTestPTP ptp(false, true, false);
    ptp.begin();

    NMOSNode node(ptp);
    node.begin(IPAddress(10, 0, 0, 9), 8010);

    const std::string id = node.getNodeId();
    CHECK_EQ(id.size(), 36);
    CHECK_EQ(id[14], '4');                                         // version
    CHECK(std::string("89ab").find(id[19]) != std::string::npos);  // variant

    // The same board is the same node after a reboot: nothing is stored,
    // so this has to fall out of the clock identity every time.
    NMOSNode again(ptp);
    again.begin(IPAddress(10, 0, 0, 9), 8010);
    CHECK(id == again.getNodeId());

    // The identity is in there: the first four bytes of the clock ID open
    // the UUID.
    uint8_t expected[8];
    expectedClockID(expected);
    char head[9];
    snprintf(head, sizeof(head), "%02x%02x%02x%02x", expected[0], expected[1], expected[2],
             expected[3]);
    CHECK(id.rfind(head, 0) == 0);
}

static void testRegisteringPostsTheNodeToTheRightPlace()
{
    ptptest::state().reset();
    NmosTestPTP ptp(false, true, false);
    ptp.begin();

    NMOSNode node(ptp);
    node.setLabel("Rack 3 clock");
    node.begin(IPAddress(10, 0, 0, 9), 8010);
    node.update();

    CHECK(node.isRegistered());
    CHECK_EQ(ptptest::state().tcpConnects.size(), 1);
    if (!ptptest::state().tcpConnects.empty())
    {
        CHECK(ptptest::state().tcpConnects[0].first == "10.0.0.9");
        CHECK_EQ(ptptest::state().tcpConnects[0].second, 8010);
    }

    const std::string &request = lastRequest();
    CHECK(contains(request, "POST /x-nmos/registration/v1.3/resource HTTP/1.1"));
    CHECK(contains(request, "Content-Type: application/json"));
    CHECK(contains(request, "\"type\":\"node\""));
    CHECK(contains(request, "\"label\":\"Rack 3 clock\""));
    CHECK(contains(request, node.getNodeId()));
}

static void testItClaimsThePtpClockOnlyWhileItHasOne()
{
    ptptest::state().reset();
    NmosTestPTP ptp(false, true, false);
    ptp.begin();

    NMOSNode node(ptp);
    node.begin(IPAddress(10, 0, 0, 9), 8010);
    node.update();

    // Free-running: "internal", and no claim of a lock.
    CHECK(contains(lastRequest(), "\"ref_type\":\"internal\""));
    CHECK(!contains(lastRequest(), "\"locked\":true"));
}

static void testTheHeartbeatGoesToItsOwnEndpoint()
{
    ptptest::state().reset();
    NmosTestPTP ptp(false, true, false);
    ptp.begin();

    NMOSNode node(ptp);
    node.begin(IPAddress(10, 0, 0, 9), 8010);
    node.update();  // registers
    CHECK(node.isRegistered());

    ptptest::state().tcpResponse = "HTTP/1.1 200 OK\r\n\r\n";
    ptptest::state().millisNow += NMOSNode::HEARTBEAT_INTERVAL_MS;
    node.update();  // beats

    std::string expected = "POST /x-nmos/registration/v1.3/health/nodes/";
    expected += node.getNodeId();
    CHECK(contains(lastRequest(), expected.c_str()));
    CHECK(node.isRegistered());
}

static void testItDoesNotTalkMoreOftenThanTheInterval()
{
    ptptest::state().reset();
    NmosTestPTP ptp(false, true, false);
    ptp.begin();

    NMOSNode node(ptp);
    node.begin(IPAddress(10, 0, 0, 9), 8010);
    node.update();
    const size_t afterFirst = ptptest::state().tcpConnects.size();

    // Every loop() calls update(); a board that opened a socket each time
    // would spend its time on HTTP instead of on the clock.
    for (int i = 0; i < 100; i++) node.update();
    CHECK_EQ(ptptest::state().tcpConnects.size(), afterFirst);

    ptptest::state().millisNow += NMOSNode::HEARTBEAT_INTERVAL_MS;
    node.update();
    CHECK_EQ(ptptest::state().tcpConnects.size(), afterFirst + 1);
}

static void testAForgottenNodeRegistersAgain()
{
    ptptest::state().reset();
    NmosTestPTP ptp(false, true, false);
    ptp.begin();

    NMOSNode node(ptp);
    node.begin(IPAddress(10, 0, 0, 9), 8010);
    node.update();
    CHECK(node.isRegistered());

    // 404 is what a registry says once it has collected a node that went
    // quiet. Treating it as a failure would leave this board invisible
    // until somebody power-cycled it.
    ptptest::state().tcpResponse = "HTTP/1.1 404 Not Found\r\n\r\n";
    ptptest::state().millisNow += NMOSNode::HEARTBEAT_INTERVAL_MS;
    node.update();

    const auto &requests = ptptest::state().tcpRequests;
    CHECK_EQ(requests.size(), 3);
    if (requests.size() == 3)
    {
        CHECK(contains(requests[1], "/health/nodes/"));
        CHECK(contains(requests[2], "POST /x-nmos/registration/v1.3/resource"));
    }
}

static void testARegistryThatIsNotThereIsCountedAndRetried()
{
    ptptest::state().reset();
    NmosTestPTP ptp(false, true, false);
    ptp.begin();

    ptptest::state().tcpConnectResult = false;

    NMOSNode node(ptp);
    node.begin(IPAddress(10, 0, 0, 9), 8010);
    node.update();

    CHECK(!node.isRegistered());
    CHECK_EQ(node.getFailureCount(), 1);
    CHECK_EQ(ptptest::state().tcpRequests.size(), 0);  // nothing was sent

    // It comes back on the next beat rather than giving up: a registry
    // that was rebooting is the normal case.
    ptptest::state().tcpConnectResult = true;
    ptptest::state().millisNow += NMOSNode::HEARTBEAT_INTERVAL_MS;
    node.update();
    CHECK(node.isRegistered());
}

static void testUnregisteringDeletesTheNode()
{
    ptptest::state().reset();
    NmosTestPTP ptp(false, true, false);
    ptp.begin();

    NMOSNode node(ptp);
    node.begin(IPAddress(10, 0, 0, 9), 8010);
    node.update();

    ptptest::state().tcpResponse = "HTTP/1.1 204 No Content\r\n\r\n";
    node.unregisterNode();

    std::string expected = "DELETE /x-nmos/registration/v1.3/resource/nodes/";
    expected += node.getNodeId();
    CHECK(contains(lastRequest(), expected.c_str()));
    CHECK(!node.isRegistered());

    // And nothing is sent for a node that was never registered.
    const size_t before = ptptest::state().tcpRequests.size();
    node.unregisterNode();
    CHECK_EQ(ptptest::state().tcpRequests.size(), before);
}


// The wait for the answer compared two absolute millis() values. Across
// the wrap -- every 49.7 days on the board -- the deadline is a smaller
// number than now, so the loop never ran, nothing was read, and every
// registration and heartbeat in that window came back as unreachable
// from a registry that was answering perfectly well.
static void testTheAnswerIsReadAcrossTheMillisWrap()
{
    ptptest::state().reset();
    NmosTestPTP ptp(false, true, false);
    ptp.begin();

    // Close enough to the top that adding the connect timeout wraps.
    ptptest::state().millisNow = std::numeric_limits<unsigned long>::max() - 50;
    ptptest::state().tcpResponse = "HTTP/1.1 201 Created\r\n\r\n";

    NMOSNode node(ptp);
    node.begin(IPAddress(10, 0, 0, 9), 8010);
    node.update();

    CHECK(node.isRegistered());
    CHECK_EQ(node.getFailureCount(), 0);
}

// IS-04 versions a resource by the time it changed, and a registry keeps
// the newest version it has seen. millis() counts from this boot, so the
// version a restarted board sent was SMALLER than the one the registry
// already held for it. The PTP clock is the TAI this board exists to
// carry.
static void testTheResourceVersionComesFromThePtpClock()
{
    ptptest::state().reset();
    NmosTestPTP ptp(false, true, false);
    ptp.begin();

    ptptest::state().hardwareTime.tv_sec = 1739000000;
    ptptest::state().hardwareTime.tv_nsec = 123456789;
    // A board that has been up for a while: the old version would have
    // been this, and it goes backwards on the next boot.
    ptptest::state().millisNow = 90000;

    NMOSNode node(ptp);
    node.begin(IPAddress(10, 0, 0, 9), 8010);
    node.update();

    CHECK(contains(lastRequest(), "\"version\":\"1739000000:123456789\""));
    CHECK(!contains(lastRequest(), "\"version\":\"90:0\""));
}

// The label is the sketch's to choose, and it lands inside a JSON string.
// One quote in it built a body no registry could parse.
static void testALabelWithAQuoteStaysInsideItsString()
{
    ptptest::state().reset();
    NmosTestPTP ptp(false, true, false);
    ptp.begin();

    NMOSNode node(ptp);
    node.setLabel("Rack \"A\" \\ stage");
    node.begin(IPAddress(10, 0, 0, 9), 8010);
    node.update();

    CHECK(contains(lastRequest(), "\"label\":\"Rack \\\"A\\\" \\\\ stage\""));
    CHECK(node.isRegistered());
}


// RFC 7230 makes the port part of Host, and a registry is rarely on 80:
// nmos-cpp listens on 8010 and 8235. The address alone routed a request
// to whatever the far end serves by default.
static void testTheHostHeaderCarriesThePort()
{
    ptptest::state().reset();
    NmosTestPTP ptp(false, true, false);
    ptp.begin();

    NMOSNode node(ptp);
    node.begin(IPAddress(10, 0, 0, 9), 8010);
    node.update();
    CHECK(contains(lastRequest(), "Host: 10.0.0.9:8010\r\n"));

    // The default port is left off, which is what the standard asks for.
    ptptest::state().reset();
    NMOSNode plain(ptp);
    plain.begin(IPAddress(10, 0, 0, 9), 80);
    plain.update();
    CHECK(contains(lastRequest(), "Host: 10.0.0.9\r\n"));
}

// The longest request this can build, against the buffer it is built in.
// snprintf reports the length it WOULD have written, and only the
// negative case was tested: a header that did not fit went to
// writeFully() with that larger length, reading past the buffer and
// putting whatever followed it on the wire. Truncation is now a failure,
// so a buffer grown too small shows up here as a request never sent.
static void testTheLongestRequestFitsItsBuffer()
{
    ptptest::state().reset();
    NmosTestPTP ptp(false, true, false);
    ptp.begin();

    NMOSNode node(ptp);
    node.begin(IPAddress(255, 255, 255, 255), 65535);
    node.update();
    CHECK(node.isRegistered());

    ptptest::state().tcpResponse = "HTTP/1.1 204 No Content\r\n\r\n";
    node.unregisterNode();

    // DELETE, the longest path, the longest authority.
    const std::string &request = lastRequest();
    CHECK(contains(request, "DELETE /x-nmos/registration/v1.3/resource/nodes/"));
    CHECK(contains(request, "Host: 255.255.255.255:65535\r\n"));
    // The whole header is there: a truncated one has no blank line.
    CHECK(contains(request, "Connection: close\r\n\r\n"));
}

// Unregistering takes the node off the registry and stops it. update()
// is called from loop(), so putting the registration back on the next
// call undid the only thing this method is for.
static void testUnregisteringStopsTheNode()
{
    ptptest::state().reset();
    NmosTestPTP ptp(false, true, false);
    ptp.begin();

    NMOSNode node(ptp);
    node.begin(IPAddress(10, 0, 0, 9), 8010);
    node.update();
    CHECK(node.isRegistered());

    ptptest::state().tcpResponse = "HTTP/1.1 204 No Content\r\n\r\n";
    node.unregisterNode();
    const size_t after = ptptest::state().tcpRequests.size();

    ptptest::state().tcpResponse = "HTTP/1.1 201 Created\r\n\r\n";
    ptptest::state().millisNow += NMOSNode::HEARTBEAT_INTERVAL_MS;
    for (int i = 0; i < 10; i++) node.update();
    CHECK_EQ(ptptest::state().tcpRequests.size(), after);
    CHECK(!node.isRegistered());

    // begin() is the way back on.
    node.begin(IPAddress(10, 0, 0, 9), 8010);
    node.update();
    CHECK(node.isRegistered());
}

void runNmosTests()
{
    testTheNodeIdComesFromTheClockIdentity();
    testRegisteringPostsTheNodeToTheRightPlace();
    testItClaimsThePtpClockOnlyWhileItHasOne();
    testTheHeartbeatGoesToItsOwnEndpoint();
    testItDoesNotTalkMoreOftenThanTheInterval();
    testAForgottenNodeRegistersAgain();
    testARegistryThatIsNotThereIsCountedAndRetried();
    testUnregisteringDeletesTheNode();
    testUnregisteringStopsTheNode();
    testTheHostHeaderCarriesThePort();
    testTheLongestRequestFitsItsBuffer();
    testTheAnswerIsReadAcrossTheMillisWrap();
    testTheResourceVersionComesFromThePtpClock();
    testALabelWithAQuoteStaysInsideItsString();
}
