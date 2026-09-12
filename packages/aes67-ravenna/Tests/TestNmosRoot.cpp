//
// TestNmosRoot.cpp
// AES67 RAVENNA session layer
// The two listings above the APIs: where a controller actually starts.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Ravenna/ChannelMappingApi.h"
#include "Ravenna/NmosRoot.h"
#include "Ravenna/NodeApi.h"

using namespace AES67::Ravenna;

namespace {

JsonValue bodyOf(const ApiResponse& response) {
    JsonValue value;
    std::string error;
    parseJson(response.body, value, error);
    return value;
}

bool lists(const JsonValue& value, const std::string& entry) {
    if (!value.isArray()) return false;
    for (const JsonValue& item : value.asArray()) {
        if (item.isString() && item.asString() == entry) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("/x-nmos names the APIs this device has") {
    const auto response = nmosRootListing("GET", "/x-nmos/");
    REQUIRE(response);
    CHECK(response->status == 200);

    const JsonValue names = bodyOf(*response);
    CHECK(lists(names, "connection/"));
    CHECK(lists(names, "node/"));
    CHECK(lists(names, "channelmapping/"));

    // With or without the trailing slash, it is the one resource.
    REQUIRE(nmosRootListing("GET", "/x-nmos"));
    CHECK(bodyOf(*nmosRootListing("GET", "/x-nmos")).asArray().size() == 3);
}

TEST_CASE("Each API names the version it speaks") {
    const auto connection = nmosRootListing("GET", "/x-nmos/connection");
    REQUIRE(connection);
    CHECK(lists(bodyOf(*connection), "v1.1/"));

    const auto node = nmosRootListing("GET", "/x-nmos/node/");
    REQUIRE(node);
    CHECK(lists(bodyOf(*node), "v1.3/"));

    const auto mapping = nmosRootListing("GET", "/x-nmos/channelmapping");
    REQUIRE(mapping);
    CHECK(lists(bodyOf(*mapping), "v1.0/"));
}

TEST_CASE("Anything deeper belongs to an API, and this answers nothing") {
    CHECK_FALSE(nmosRootListing("GET", "/x-nmos/connection/v1.1/single/"));
    CHECK_FALSE(nmosRootListing("GET", "/x-nmos/node/v1.3/self/"));
    CHECK_FALSE(nmosRootListing("GET", "/x-nmos/nonsense"));
    CHECK_FALSE(nmosRootListing("GET", "/"));
}

TEST_CASE("The listings are readable and nothing else") {
    const auto response = nmosRootListing("POST", "/x-nmos/");
    REQUIRE(response);
    CHECK(response->status == 405);
}
