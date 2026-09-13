//
// TestRegistryBrowser.cpp
// AES67 RAVENNA session layer
// Which registry a node picks out of what the link advertises.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Ravenna/RegistryBrowser.h"

using namespace AES67::Ravenna;

namespace {

DiscoveredService advertised(const std::string& name, uint16_t port,
                             const std::vector<std::string>& txt, uint32_t addressV4 = 0) {
    DiscoveredService service;
    service.instanceName = name + "._nmos-register._tcp.local";
    service.hostName = name + ".local";
    service.port = port;
    service.addressV4 = addressV4;
    service.txtEntries = txt;
    return service;
}

}  // namespace

TEST_CASE("The lowest priority is the one a node uses") {
    // IS-04 sec 3.1: a node takes the lowest `pri` it can reach and only moves
    // on when that registry fails. Taking whichever answered first would make
    // a plant's backup registry the one it registers with half the time.
    const std::vector<NmosRegistry> registries = registriesFrom(
        {advertised("slow", 8010, {"pri=100", "api_ver=v1.3"}),
         advertised("main", 8011, {"pri=0", "api_ver=v1.2,v1.3"}),
         advertised("spare", 8012, {"pri=10", "api_ver=v1.3"})},
        "v1.3");

    REQUIRE(registries.size() == 3);
    CHECK(registries[0].port == 8011);
    CHECK(registries[1].port == 8012);
    CHECK(registries[2].port == 8010);
    CHECK(registries[0].registrationPath() == "/x-nmos/registration/v1.3");
}

TEST_CASE("A registry this node cannot talk to is left out") {
    const std::vector<NmosRegistry> registries = registriesFrom(
        {// Another version of the API entirely.
         advertised("old", 8010, {"api_ver=v1.0,v1.1"}),
         // HTTPS, and nothing here speaks TLS.
         advertised("secure", 8011, {"api_ver=v1.3", "api_proto=https"}),
         // No SRV came with it, so there is nowhere to send anything.
         advertised("halfway", 0, {"api_ver=v1.3"}),
         advertised("usable", 8013, {"api_ver=v1.3"})},
        "v1.3");

    REQUIRE(registries.size() == 1);
    CHECK(registries[0].port == 8013);
}

TEST_CASE("A registry that advertises no version is taken at the default") {
    // Not every implementation fills the TXT in, and dropping one for a key
    // it never wrote would leave a node with nowhere to register.
    const std::vector<NmosRegistry> registries =
        registriesFrom({advertised("quiet", 8010, {})}, "v1.3");
    REQUIRE(registries.size() == 1);
    CHECK(registries[0].apiVersion == "v1.3");
    // And with no priority, it sits behind anything that named one.
    CHECK(registries[0].priority == 100);
}

TEST_CASE("The address is used when there is one, and the name when there is not") {
    const std::vector<NmosRegistry> withAddress =
        registriesFrom({advertised("reg", 8010, {"api_ver=v1.3"}, 0xC0A8000A)}, "v1.3");
    REQUIRE(withAddress.size() == 1);
    CHECK(withAddress[0].host == "192.168.0.10");

    const std::vector<NmosRegistry> nameOnly =
        registriesFrom({advertised("reg", 8010, {"api_ver=v1.3"})}, "v1.3");
    REQUIRE(nameOnly.size() == 1);
    CHECK(nameOnly[0].host == "reg.local");
}

TEST_CASE("A priority that is not a number does not order anything") {
    const std::vector<NmosRegistry> registries = registriesFrom(
        {advertised("odd", 8010, {"api_ver=v1.3", "pri=soon"}),
         advertised("plain", 8011, {"api_ver=v1.3", "pri=5"})},
        "v1.3");
    REQUIRE(registries.size() == 2);
    CHECK(registries[0].port == 8011);
    CHECK(registries[1].priority == 100);
}
