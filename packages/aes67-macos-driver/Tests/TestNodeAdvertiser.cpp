//
// TestNodeAdvertiser.cpp
// AES67 macOS Driver
//
// The `_nmos-node._tcp` record set this driver advertises, which is how a
// controller finds it without a registry (IS-04 peer-to-peer).
//
// nodeAdvertisement() is the part worth pinning: it is pure, and it is where
// a record that mDNSResponder silently drops gets built. start() joins mDNS
// and is left to the discovery tier.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/NodeAdvertiser.h"

#include <algorithm>
#include <string>

using namespace AES67;

namespace {

bool hasEntry(const Ravenna::SessionAdvertisement& node, const std::string& entry) {
    return std::find(node.txtEntries.begin(), node.txtEntries.end(), entry) !=
           node.txtEntries.end();
}

} // namespace

TEST_CASE("An instance name is one DNS label") {
    // A dot inside the instance name is a separator on the wire, so a label
    // built from gethostname() -- "AES67 on macmini.local" -- makes a PTR of
    // two labels and mDNSResponder drops the record without a word. The
    // driver would advertise nothing and nobody would know why.
    const auto node = nodeAdvertisement("AES67 on macmini.local", "macmini.local", 0x0A000002, 8080);

    CHECK(node.instanceName.find('.') == std::string::npos);
    CHECK(node.instanceName == "AES67 on macmini local");

    // The host name is a different field and keeps its dots: that one is a
    // name, not a label.
    CHECK(node.hostName == "macmini.local");
}

TEST_CASE("A label with no dots is left alone") {
    const auto node = nodeAdvertisement("AES67 Driver", "mac.local", 0x7F000001, 8080);
    CHECK(node.instanceName == "AES67 Driver");
}

TEST_CASE("The service is the one a controller browses for") {
    const auto node = nodeAdvertisement("AES67", "mac.local", 0x7F000001, 8080);

    CHECK(node.serviceType == Ravenna::kNmosNodeService);
    // No subtype: this node is not scoped to a registry's browse domain.
    CHECK(node.subtype.empty());
}

TEST_CASE("The address and port are carried as given") {
    // The SRV points at the API a controller opens next, and advertising a
    // port nothing listens on is the failure mode the whole record exists to
    // avoid.
    const auto node = nodeAdvertisement("AES67", "mac.local", 0xC0A80105, 49152);

    CHECK(node.addressV4 == 0xC0A80105);  // host byte order, 192.168.1.5
    CHECK(node.port == 49152);
}

TEST_CASE("The TXT record says what IS-04 requires of it") {
    // IS-04 section 3: what a controller reads before it opens a connection.
    // api_ver wrong or missing and it does not try.
    const auto node = nodeAdvertisement("AES67", "mac.local", 0x7F000001, 8080);

    CHECK(hasEntry(node, "api_ver=v1.3"));
    CHECK(hasEntry(node, "api_proto=http"));
    CHECK(hasEntry(node, "api_auth=false"));
    CHECK(hasEntry(node, "ver_slf=0"));
    CHECK(node.txtEntries.size() == 4);
}

TEST_CASE("Stopping one that never started is not an error") {
    // ~NodeAdvertiser calls stop(), and AES67Device's teardown calls it too:
    // the second of those can run without the first ever having started.
    NodeAdvertiser advertiser;
    advertiser.stop();
    advertiser.stop();
}
