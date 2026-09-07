//
// TestSessionCatalogue.cpp
// AES67 RAVENNA session layer
// What is offered, what a DESCRIBE gets back, and what is refused before it
// ever reaches the wire.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Ravenna/SessionCatalogue.h"

using namespace AES67;
using namespace AES67::Ravenna;

namespace {

RavennaSession stereoSession(const std::string& name = "Mix A") {
    RavennaSession session;
    session.name = name;

    session.sdp.sessionName = name;
    session.sdp.originAddress = "192.168.1.50";
    session.sdp.connectionAddress = "239.69.1.10";
    session.sdp.port = 5004;
    session.sdp.encoding = "L24";
    session.sdp.sampleRate = 48000;
    session.sdp.numChannels = 2;
    session.sdp.ptimeUs = 1000;
    session.sdp.direction = "sendonly";

    // The routing matrix is the core's, not a second one here: these two
    // channels are device channels 0 and 1.
    session.mapping.deviceChannelStart = 0;
    session.mapping.deviceChannelCount = 2;
    session.mapping.streamChannelCount = 2;
    return session;
}

}  // namespace

TEST_CASE("A session is described at the path its name gives it") {
    SessionCatalogue catalogue;
    std::string error;
    REQUIRE(catalogue.add(stereoSession(), error));
    CHECK(error.empty());

    const auto sdp = catalogue.describe("/by-name/Mix A");
    REQUIRE(sdp.has_value());
    CHECK(sdp->find("s=Mix A") != std::string::npos);
    CHECK(sdp->find("L24/48000/2") != std::string::npos);
    CHECK(sdp->find("239.69.1.10") != std::string::npos);
}

TEST_CASE("A path that names nothing gets nothing, not an empty description") {
    SessionCatalogue catalogue;
    std::string error;
    REQUIRE(catalogue.add(stereoSession(), error));

    CHECK(catalogue.describe("/by-name/Mix B").has_value() == false);
    CHECK(catalogue.describe("/").has_value() == false);
    CHECK(catalogue.describe("").has_value() == false);
}

TEST_CASE("A mapping that does not carry what the SDP announces is refused") {
    // The whole failure this guards against is silent: the session advertises
    // eight channels, the device puts two on the wire, and nothing on either
    // side ever says so.
    RavennaSession session = stereoSession();
    session.sdp.numChannels = 8;

    SessionCatalogue catalogue;
    std::string error;
    CHECK(catalogue.add(session, error) == false);
    CHECK(error.find("2 device channels") != std::string::npos);
    CHECK(error.find("announces 8") != std::string::npos);
    CHECK(catalogue.size() == 0);
}

TEST_CASE("An SDP the core calls invalid is refused, with its reasons") {
    RavennaSession session = stereoSession();
    session.sdp.connectionAddress.clear();

    SessionCatalogue catalogue;
    std::string error;
    CHECK(catalogue.add(session, error) == false);
    CHECK(error.find("Connection address") != std::string::npos);
}

TEST_CASE("A session with no name is refused") {
    RavennaSession session = stereoSession();
    session.name.clear();

    SessionCatalogue catalogue;
    std::string error;
    CHECK(catalogue.add(session, error) == false);
    CHECK(catalogue.size() == 0);
}

TEST_CASE("Adding the same name twice replaces rather than duplicates") {
    // Two sessions with one name are one instance in DNS-SD and a coin toss
    // in a DESCRIBE.
    SessionCatalogue catalogue;
    std::string error;
    REQUIRE(catalogue.add(stereoSession(), error));

    RavennaSession changed = stereoSession();
    changed.sdp.connectionAddress = "239.69.1.20";
    REQUIRE(catalogue.add(changed, error));

    CHECK(catalogue.size() == 1);
    const auto sdp = catalogue.describe("/by-name/Mix A");
    REQUIRE(sdp.has_value());
    CHECK(sdp->find("239.69.1.20") != std::string::npos);
}

TEST_CASE("What is advertised carries the path and the channel count") {
    SessionCatalogue catalogue;
    std::string error;
    REQUIRE(catalogue.add(stereoSession("Mix A"), error));
    REQUIRE(catalogue.add(stereoSession("Mix B"), error));

    const auto advertised = catalogue.advertisements("box.local", 8554, 0xC0A80132);
    REQUIRE(advertised.size() == 2);

    for (const SessionAdvertisement& entry : advertised) {
        CHECK(entry.hostName == "box.local");
        CHECK(entry.port == 8554);
        CHECK(entry.addressV4 == 0xC0A80132);

        bool hasPath = false;
        bool hasChannels = false;
        for (const std::string& txt : entry.txtEntries) {
            if (txt == "path=/by-name/" + entry.instanceName) hasPath = true;
            if (txt == "channels=2") hasChannels = true;
        }
        CHECK(hasPath);
        CHECK(hasChannels);
    }
}

TEST_CASE("A removed session stops being offered and stops being advertised") {
    SessionCatalogue catalogue;
    std::string error;
    REQUIRE(catalogue.add(stereoSession(), error));

    CHECK(catalogue.remove("Mix A"));
    CHECK(catalogue.remove("Mix A") == false);
    CHECK(catalogue.describe("/by-name/Mix A").has_value() == false);
    CHECK(catalogue.advertisements("box.local", 8554, 0).empty());
}
