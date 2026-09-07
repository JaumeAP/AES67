//
// TestNodeAPI.cpp
// AES67 macOS Driver
//
// The IS-04 Node API as a controller reads it: what a node says about
// itself, its one device, and the sources, flows, senders and receivers
// derived from the streams. Routing only, no socket: the server it hangs
// off is ConnectionAPIServer's, which has its own loopback tests.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/NodeAPIRouter.h"
#include "NetworkEngine/Discovery/NodeAdvertiser.h"

#include <string>
#include <vector>

using namespace AES67;

namespace {

const std::string kNodeId = "0f1e2d3c-4b5a-4697-8877-665544332211";

NMOSNodeInfo testNode() {
    NMOSNodeInfo node;
    node.id = kNodeId;
    node.label = "Studio Mac";
    node.hostname = "studio-mac";
    node.apiHost = "192.168.1.50";
    node.apiPort = 51234;
    return node;
}

std::vector<NMOSSenderResource> twoSenders() {
    NMOSSenderResource a;
    a.name = "Mix A";
    a.multicastAddress = "239.69.0.1";
    a.port = 5004;
    a.channels = 2;
    NMOSSenderResource b = a;
    b.name = "Mix B";
    b.multicastAddress = "239.69.0.2";
    b.channels = 8;
    return {a, b};
}

std::vector<NMOSReceiverResource> oneReceiver() {
    NMOSReceiverResource r;
    r.name = "Return 1";
    r.active = false;
    return {r};
}

NodeAPIRouter testRouter(const std::string& controlHref = "http://192.168.1.50:51234/x-nmos/connection/v1.1/") {
    return NodeAPIRouter(testNode(), controlHref, twoSenders, oneReceiver);
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("the roots list what is under them") {
    const NodeAPIRouter router = testRouter();
    CHECK(router.route("GET", "/x-nmos/node/").body == "[\"v1.3/\"]");
    CHECK(router.route("GET", "/x-nmos/node").body == "[\"v1.3/\"]");
    const auto root = router.route("GET", "/x-nmos/node/v1.3/");
    CHECK(root.status == 200);
    CHECK(root.body ==
          "[\"self/\",\"devices/\",\"sources/\",\"flows/\",\"senders/\",\"receivers/\",\"subscriptions/\"]");
}

TEST_CASE("self is the node, with its API endpoint") {
    const auto reply = testRouter().route("GET", "/x-nmos/node/v1.3/self");
    CHECK(reply.status == 200);
    CHECK(reply.contentType == "application/json");
    CHECK(contains(reply.body, "\"id\": \"" + kNodeId + "\""));
    CHECK(contains(reply.body, "\"label\": \"Studio Mac\""));
    CHECK(contains(reply.body, "\"port\": 51234"));
    CHECK_FALSE(contains(reply.body, "\"type\": \"node\""));
}

TEST_CASE("the one device carries every sender and receiver and the control") {
    const NodeAPIRouter router = testRouter();
    const auto list = router.route("GET", "/x-nmos/node/v1.3/devices/");
    CHECK(list.status == 200);
    CHECK(list.body.front() == '[');
    CHECK(contains(list.body, "\"id\": \"" + router.deviceId() + "\""));
    CHECK(contains(list.body, NMOSRegistrationClient::deriveId(kNodeId, "sender:Mix A")));
    CHECK(contains(list.body, NMOSRegistrationClient::deriveId(kNodeId, "sender:Mix B")));
    CHECK(contains(list.body, NMOSRegistrationClient::deriveId(kNodeId, "receiver:Return 1")));
    CHECK(contains(list.body, "urn:x-nmos:control:sr-ctrl/v1.1"));
    CHECK(contains(list.body, "http://192.168.1.50:51234/x-nmos/connection/v1.1/"));

    const auto one = router.route("GET", "/x-nmos/node/v1.3/devices/" + router.deviceId());
    CHECK(one.status == 200);
    CHECK(one.body.front() == '{');
    CHECK(router.deviceId() == NMOSRegistrationClient::deriveId(kNodeId, "device"));
}

TEST_CASE("a device without a connection API advertises no control") {
    const auto reply = testRouter("").route("GET", "/x-nmos/node/v1.3/devices");
    CHECK(contains(reply.body, "\"controls\": []"));
}

TEST_CASE("sources, flows and senders come one per transmit stream, by derived id") {
    const NodeAPIRouter router = testRouter();
    const std::string senderA = NMOSRegistrationClient::deriveId(kNodeId, "sender:Mix A");
    const std::string flowA = NMOSRegistrationClient::deriveId(kNodeId, "flow:Mix A");
    const std::string sourceA = NMOSRegistrationClient::deriveId(kNodeId, "source:Mix A");

    const auto senders = router.route("GET", "/x-nmos/node/v1.3/senders");
    CHECK(senders.status == 200);
    CHECK(contains(senders.body, senderA));
    CHECK(contains(senders.body, "\"flow_id\": \"" + flowA + "\""));

    const auto flow = router.route("GET", "/x-nmos/node/v1.3/flows/" + flowA);
    CHECK(flow.status == 200);
    CHECK(contains(flow.body, "\"source_id\": \"" + sourceA + "\""));

    const auto source = router.route("GET", "/x-nmos/node/v1.3/sources/" + sourceA);
    CHECK(source.status == 200);
    CHECK(contains(source.body, "\"id\": \"" + sourceA + "\""));
}

TEST_CASE("receivers come one per receive stream") {
    const auto reply = testRouter().route("GET", "/x-nmos/node/v1.3/receivers/");
    CHECK(reply.status == 200);
    CHECK(contains(reply.body, NMOSRegistrationClient::deriveId(kNodeId, "receiver:Return 1")));
}

TEST_CASE("what is not there is a 404 error object, not an empty list") {
    const NodeAPIRouter router = testRouter();
    const auto missing = router.route("GET", "/x-nmos/node/v1.3/senders/not-an-id");
    CHECK(missing.status == 404);
    CHECK(contains(missing.body, "\"code\": 404"));
    CHECK(contains(missing.body, "\"debug\": null"));
    CHECK(router.route("GET", "/x-nmos/node/v1.3/nothing").status == 404);
    CHECK(router.route("GET", "/x-nmos/node/v1.0/self").status == 404);
}

TEST_CASE("subscriptions are not served and say so") {
    CHECK(testRouter().route("GET", "/x-nmos/node/v1.3/subscriptions").status == 501);
}

TEST_CASE("only GET is answered") {
    const auto reply = testRouter().route("POST", "/x-nmos/node/v1.3/self");
    CHECK(reply.status == 405);
    CHECK(contains(reply.body, "\"code\": 405"));
}

TEST_CASE("the endpoint and control can be set after construction") {
    NodeAPIRouter router(testNode(), "", twoSenders, oneReceiver);
    CHECK(contains(router.route("GET", "/x-nmos/node/v1.3/devices").body, "\"controls\": []"));
    router.setEndpoint("10.0.0.2", 4444, "http://10.0.0.2:4444/x-nmos/connection/v1.1/");
    CHECK(contains(router.route("GET", "/x-nmos/node/v1.3/self").body, "\"port\": 4444"));
    CHECK(contains(router.route("GET", "/x-nmos/node/v1.3/self").body, "\"href\": \"http://10.0.0.2:4444/\""));
    CHECK(contains(router.route("GET", "/x-nmos/node/v1.3/devices").body,
                   "http://10.0.0.2:4444/x-nmos/connection/v1.1/"));
}

TEST_CASE("touch moves every version forward") {
    NodeAPIRouter router = testRouter();
    const std::string before = router.route("GET", "/x-nmos/node/v1.3/self").body;
    router.touch();
    // The version is seconds:nanoseconds of the touch; two touches in a row
    // still differ in nanoseconds on any real clock.
    CHECK(router.route("GET", "/x-nmos/node/v1.3/self").body != before);
}

TEST_CASE("the node advertisement is an _nmos-node._tcp service with IS-04's TXT") {
    const Ravenna::SessionAdvertisement ad =
        nodeAdvertisement("Studio Mac", "studio-mac.local", 0xC0A80132u, 51234);
    CHECK(ad.instanceName == "Studio Mac");
    CHECK(ad.hostName == "studio-mac.local");
    CHECK(ad.port == 51234);
    CHECK(ad.addressV4 == 0xC0A80132u);
    CHECK(ad.serviceType == std::string(Ravenna::kNmosNodeService));
    CHECK(ad.subtype.empty());
    CHECK(ad.txtEntries == std::vector<std::string>{"api_ver=v1.3", "api_proto=http",
                                                    "api_auth=false", "ver_slf=0"});
}

TEST_CASE("a label carrying a domain still makes one mDNS instance label") {
    // gethostname() answers "macmini.local" on a plain Mac, and the driver
    // builds its label out of it. A dot inside an instance name is a label
    // separator on the wire, so the PTR would carry two labels and
    // mDNSResponder would drop the record: the node would serve its APIs
    // and nothing would ever find them.
    const Ravenna::SessionAdvertisement ad = nodeAdvertisement(
        "AES67 macOS Driver on macmini.local", "macmini.local", 0xC0A80132u, 51234);
    CHECK(ad.instanceName == "AES67 macOS Driver on macmini local");
    // The host name is a real domain name and keeps its dots.
    CHECK(ad.hostName == "macmini.local");
}
