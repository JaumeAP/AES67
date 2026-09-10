# Network Routing Implementation Plan

> **Status: done.** The boxes below were never ticked, so this read as pending
> work; it is not. What the plan describes is in the tree: the IS-04 Node API
> and the `_nmos-node._tcp` advertisement in `packages/aes67-ravenna`
> (`Ravenna/NodeApi.*`, `Ravenna/DnsSd.h`), the driver serving both
> (`Driver/AES67Device.cpp`, covered by `Tests/TestNodeAPI.cpp`), and the
> Manager's side in `ManagerApp/Views/RoutingMatrixView.swift` with
> `NmosResources.swift` under `ManagerApp/Tests/NmosResourcesTests.swift`.
> Kept as the record of why it is shaped the way it is.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Manager finds every NMOS node on the link, shows a senders-by-receivers matrix across all of them, and a click connects a receiver to a sender over IS-05; this Mac's driver is one of those nodes.

**Architecture:** The driver already serves IS-05 (`ConnectionAPIServer`) and builds IS-04 resources for a registry (`NMOSRegistrationClient`). It gains a pure IS-04 Node API router served on the same HTTP port, an mDNS `_nmos-node._tcp` advertisement through `aes67-ravenna`'s `MdnsResponder`, and both start whenever the device runs. The Manager gains a Swift controller (`NetServiceBrowser` + `URLSession`) whose pure parts (JSON, PATCH bodies, matrix) live in `NmosResources.swift`, and a `RoutingMatrixView` sheet.

**Tech Stack:** C++17/20, doctest, CMake, POSIX sockets (driver); Swift 5, SwiftUI, Foundation (`NetServiceBrowser`, `URLSession`, `JSONSerialization`), plain `swiftc` via `ManagerApp/build.sh` and `ManagerApp/run-tests.sh` (Manager).

**Spec:** `docs/superpowers/specs/2026-09-07-network-routing-design.md`

## Global Constraints

- Everything written into the repository is in English: code, comments, commit messages, docs.
- Includes in the driver package are written relative to the package root (`NetworkEngine/...`, `Driver/...`), and `Ravenna/...` for the ravenna package.
- Driver tests are doctest suites with `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`, registered in `packages/aes67-macos-driver/Tests/CMakeLists.txt` with `LABELS` and `TIMEOUT 60`. Never a bare `assert()`.
- Manager tests are plain `swiftc` binaries built by `ManagerApp/run-tests.sh` with `-warnings-as-errors`; checks use the `check` / `checkEqual` helpers from `Tests/PrivilegedScriptTests.swift`.
- New Swift source files must be added to the `swiftc` source list in `ManagerApp/build.sh` or the app does not compile them.
- IS-04 version served: `v1.3`. IS-05 version served: `v1.1`. Node API root: `/x-nmos/node/v1.3`. Connection API root: `/x-nmos/connection/v1.1`.
- Node, device, source, flow, sender and receiver ids are the ones `NMOSRegistrationClient::deriveId` derives: `deriveId(nodeId, "device")`, `deriveId(nodeId, "source:" + name)`, `"flow:" + name`, `"sender:" + name`, `"receiver:" + name`.
- Registration with a registry (`NMOSSettings.enabled`) keeps its meaning and its code path.
- Nothing about network routing is persisted on the Mac.
- Build and test commands, from `packages/aes67-macos-driver`: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j` and `ctest --test-dir build -R <Name> --output-on-failure`. Manager: `cd ManagerApp && ./run-tests.sh` and `./build.sh --force`.
- Commits end with the session's attribution trailer (see the session instructions).

---

## File structure

Driver (`packages/aes67-macos-driver`):

- Modify `NetworkEngine/Discovery/NMOSRegistrationClient.h/.cpp`: split each `build*Body` into a `build*Data` (the bare resource object) plus a wrapper; add `apiHost`/`apiPort` to `NMOSNodeInfo` so the node's `api.endpoints` can name the served port.
- Create `NetworkEngine/Discovery/NodeAPIRouter.h/.cpp`: the pure IS-04 Node API (`route(method, path)`), built from listers of `NMOSSenderResource` / `NMOSReceiverResource`.
- Modify `NetworkEngine/Discovery/ConnectionAPIServer.h/.cpp`: a fallback router for paths outside `/x-nmos/connection`, and `/x-nmos/` listing both APIs.
- Create `NetworkEngine/Discovery/NodeAdvertiser.h/.cpp`: `_nmos-node._tcp` over `Ravenna::MdnsResponder`, on its own thread; plus the pure `nodeAdvertisement(...)` builder.
- Modify `Driver/AES67Device.h/.cpp`: node id always present, Connection API + Node API + advertiser start with the device, registry path unchanged.
- Modify `CMakeLists.txt` (driver): add the two new sources, add `aes67-ravenna` as a subdirectory and link `aes67_ravenna` into `AES67Driver`.
- Modify `../aes67-ravenna/CMakeLists.txt`: an option to skip its tool and tests when built as a dependency.
- Create `Tests/TestNodeAPI.cpp`; modify `Tests/CMakeLists.txt`.

Manager (`packages/aes67-macos-driver/ManagerApp`):

- Create `Models/NmosResources.swift`: value types, IS-04/IS-05 decoding, PATCH bodies, `RoutingMatrix`.
- Create `Models/NmosController.swift`: discovery, reads, connect/disconnect, refresh, errors.
- Create `Views/RoutingMatrixView.swift`; modify `Views/ContentView.swift` (sidebar button + sheet), `Resources/Info.plist` (Bonjour keys), `build.sh` (sources), `run-tests.sh` and `Tests/main.swift` (tests).
- Create `Tests/NmosResourcesTests.swift`.

---

### Task 1: Bare resource objects in `NMOSRegistrationClient`

The Node API serves each resource as a bare object; the registry takes it wrapped in `{"type": ..., "data": ...}`. Split the builders so both come from one place, with the registration bodies byte-identical to today (the existing `TestNMOSRegistration` suite compares them).

**Files:**
- Modify: `packages/aes67-macos-driver/NetworkEngine/Discovery/NMOSRegistrationClient.h:51-63` (struct `NMOSNodeInfo`), `:160-196` (builders)
- Modify: `packages/aes67-macos-driver/NetworkEngine/Discovery/NMOSRegistrationClient.cpp:51-290`
- Test: `packages/aes67-macos-driver/Tests/TestNMOSRegistration.cpp` (existing; add cases)

**Interfaces:**
- Produces, all `static` on `NMOSRegistrationClient`:
  - `std::string buildNodeData(const NMOSNodeInfo&, int64_t versionSeconds, int32_t versionNanos)`
  - `std::string buildDeviceData(deviceId, nodeId, label, senderIds, receiverIds, controlHref, versionSeconds, versionNanos)` (same parameters as `buildDeviceBody`)
  - `std::string buildSourceData(...)`, `buildFlowData(...)`, `buildSenderData(...)`, `buildReceiverData(...)` (same parameters as their `Body` twins)
  - `std::string wrapResource(const std::string& type, const std::string& data)`
  - `NMOSNodeInfo` gains `std::string apiHost;` and `uint16_t apiPort{0};`. When `apiPort != 0`, `buildNodeData` writes `"endpoints": [{ "host": "<apiHost>", "port": <apiPort>, "protocol": "http" }]`; otherwise `[]` as today.

- [ ] **Step 1: Write the failing tests**

Append to `Tests/TestNMOSRegistration.cpp` (inside the file's `using namespace AES67;` scope):

```cpp
TEST_CASE("a registration body is the bare resource wrapped in type and data") {
    NMOSNodeInfo node;
    node.id = "0f1e2d3c-4b5a-4697-8877-665544332211";
    node.hostname = "studio-mac";
    const std::string data = NMOSRegistrationClient::buildNodeData(node, 10, 20);
    CHECK(NMOSRegistrationClient::buildRegistrationBody(node, 10, 20) ==
          NMOSRegistrationClient::wrapResource("node", data));
    CHECK(data.front() == '{');
    CHECK(data.find("\"type\": \"node\"") == std::string::npos);
    CHECK(data.find("\"id\": \"0f1e2d3c-4b5a-4697-8877-665544332211\"") != std::string::npos);
}

TEST_CASE("a node with an API port lists it as an endpoint") {
    NMOSNodeInfo node;
    node.id = "0f1e2d3c-4b5a-4697-8877-665544332211";
    node.hostname = "studio-mac";
    CHECK(NMOSRegistrationClient::buildNodeData(node, 1, 0).find("\"endpoints\": []") !=
          std::string::npos);
    node.apiHost = "192.168.1.50";
    node.apiPort = 51234;
    const std::string data = NMOSRegistrationClient::buildNodeData(node, 1, 0);
    CHECK(data.find("\"endpoints\": [{ \"host\": \"192.168.1.50\", \"port\": 51234, "
                    "\"protocol\": \"http\" }]") != std::string::npos);
}

TEST_CASE("device, source, flow, sender and receiver bodies wrap their data") {
    NMOSSenderResource sender;
    sender.name = "Mix A";
    NMOSReceiverResource receiver;
    receiver.name = "Return 1";
    const std::vector<std::string> none;
    CHECK(NMOSRegistrationClient::buildDeviceBody("d", "n", "L", none, none, "", 1, 0) ==
          NMOSRegistrationClient::wrapResource(
              "device", NMOSRegistrationClient::buildDeviceData("d", "n", "L", none, none, "", 1, 0)));
    CHECK(NMOSRegistrationClient::buildSourceBody("s", "d", sender, 1, 0) ==
          NMOSRegistrationClient::wrapResource(
              "source", NMOSRegistrationClient::buildSourceData("s", "d", sender, 1, 0)));
    CHECK(NMOSRegistrationClient::buildFlowBody("f", "s", "d", sender, 1, 0) ==
          NMOSRegistrationClient::wrapResource(
              "flow", NMOSRegistrationClient::buildFlowData("f", "s", "d", sender, 1, 0)));
    CHECK(NMOSRegistrationClient::buildSenderBody("x", "f", "d", sender, 1, 0) ==
          NMOSRegistrationClient::wrapResource(
              "sender", NMOSRegistrationClient::buildSenderData("x", "f", "d", sender, 1, 0)));
    CHECK(NMOSRegistrationClient::buildReceiverBody("r", "d", receiver, 1, 0) ==
          NMOSRegistrationClient::wrapResource(
              "receiver", NMOSRegistrationClient::buildReceiverData("r", "d", receiver, 1, 0)));
}
```

- [ ] **Step 2: Build and run to see them fail**

Run: `cd packages/aes67-macos-driver && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j --target TestNMOSRegistration`
Expected: compile error, `buildNodeData` is not a member of `NMOSRegistrationClient`.

- [ ] **Step 3: Implement**

In `NMOSRegistrationClient.h`, add to `NMOSNodeInfo` after `href`:

```cpp
    /// Where the node's own IS-04 Node API answers, for `api.endpoints`.
    /// Port 0 means none is served and the list stays empty.
    std::string apiHost;
    uint16_t apiPort{0};
```

Declare, next to the existing `build*Body` statics:

```cpp
    /// The bare resource objects. A registry takes them wrapped by
    /// wrapResource(); the Node API serves them as they are.
    static std::string buildNodeData(const NMOSNodeInfo& node,
                                     int64_t versionSeconds, int32_t versionNanos);
    static std::string buildDeviceData(const std::string& deviceId,
                                       const std::string& nodeId,
                                       const std::string& label,
                                       const std::vector<std::string>& senderIds,
                                       const std::vector<std::string>& receiverIds,
                                       const std::string& controlHref,
                                       int64_t versionSeconds, int32_t versionNanos);
    static std::string buildSourceData(const std::string& sourceId,
                                       const std::string& deviceId,
                                       const NMOSSenderResource& sender,
                                       int64_t versionSeconds, int32_t versionNanos);
    static std::string buildFlowData(const std::string& flowId,
                                     const std::string& sourceId,
                                     const std::string& deviceId,
                                     const NMOSSenderResource& sender,
                                     int64_t versionSeconds, int32_t versionNanos);
    static std::string buildSenderData(const std::string& senderId,
                                       const std::string& flowId,
                                       const std::string& deviceId,
                                       const NMOSSenderResource& sender,
                                       int64_t versionSeconds, int32_t versionNanos);
    static std::string buildReceiverData(const std::string& receiverId,
                                         const std::string& deviceId,
                                         const NMOSReceiverResource& receiver,
                                         int64_t versionSeconds, int32_t versionNanos);
    /// `{"type": <type>, "data": <data>}`, the shape a registration POST takes.
    static std::string wrapResource(const std::string& type, const std::string& data);
```

In `NMOSRegistrationClient.cpp`, for each existing `build*Body`: rename its function to `build*Data`, drop the two leading lines `"{\n  \"type\": ..." "  \"data\": {\n"` so the stream starts with `"{\n"`, keep every inner line exactly as it is (four-space indentation), and end with `"  }"` instead of `"  }\n}\n"`. Then:

```cpp
std::string NMOSRegistrationClient::wrapResource(const std::string& type, const std::string& data) {
    return "{\n  \"type\": \"" + type + "\",\n  \"data\": " + data + "\n}\n";
}

std::string NMOSRegistrationClient::buildRegistrationBody(const NMOSNodeInfo& node,
                                                          int64_t versionSeconds,
                                                          int32_t versionNanos) {
    return wrapResource("node", buildNodeData(node, versionSeconds, versionNanos));
}
```

and the same one-liner for `buildDeviceBody` (`"device"`), `buildSourceBody` (`"source"`), `buildFlowBody` (`"flow"`), `buildSenderBody` (`"sender"`), `buildReceiverBody` (`"receiver"`), each forwarding its parameters unchanged.

In `buildNodeData`, replace the `"endpoints": []` line with:

```cpp
         << "      \"endpoints\": "
         << (node.apiPort == 0
                 ? std::string("[]")
                 : "[{ \"host\": \"" + jsonEscape(node.apiHost) + "\", \"port\": " +
                       std::to_string(node.apiPort) + ", \"protocol\": \"http\" }]")
         << "\n"
```

Check the surrounding commas match the original (the line before ends with `,\n`, and `endpoints` is the last member of `api`, so no trailing comma).

- [ ] **Step 4: Build and run the suite**

Run: `cmake --build build -j --target TestNMOSRegistration && ctest --test-dir build -R NMOSRegistration --output-on-failure`
Expected: PASS, including every pre-existing case (they compare full bodies, which proves the wrapper reproduces the old text byte for byte).

- [ ] **Step 5: Commit**

```bash
git add packages/aes67-macos-driver/NetworkEngine/Discovery/NMOSRegistrationClient.h \
        packages/aes67-macos-driver/NetworkEngine/Discovery/NMOSRegistrationClient.cpp \
        packages/aes67-macos-driver/Tests/TestNMOSRegistration.cpp
git commit -m "refactor(driver): IS-04 resources as bare objects, wrapped for the registry"
```

---

### Task 2: `NodeAPIRouter`, the pure IS-04 Node API

**Files:**
- Create: `packages/aes67-macos-driver/NetworkEngine/Discovery/NodeAPIRouter.h`
- Create: `packages/aes67-macos-driver/NetworkEngine/Discovery/NodeAPIRouter.cpp`
- Create: `packages/aes67-macos-driver/Tests/TestNodeAPI.cpp`
- Modify: `packages/aes67-macos-driver/CMakeLists.txt:220-222` (source list), `packages/aes67-macos-driver/Tests/CMakeLists.txt:273-284,584,630`

**Interfaces:**
- Consumes: `NMOSRegistrationClient::deriveId`, `build*Data`, `NMOSNodeInfo`, `NMOSSenderResource`, `NMOSReceiverResource`; `ConnectionAPIServer::Reply`.
- Produces:

```cpp
namespace AES67 {
using NMOSSenderLister = std::function<std::vector<NMOSSenderResource>()>;
using NMOSReceiverLister = std::function<std::vector<NMOSReceiverResource>()>;

class NodeAPIRouter {
public:
    static constexpr const char* kApiVersion = "v1.3";
    /// `node.id` must be set; the device id is derived from it. `controlHref`
    /// is the IS-05 root the device advertises, empty for none.
    NodeAPIRouter(NMOSNodeInfo node, std::string controlHref,
                  NMOSSenderLister senders, NMOSReceiverLister receivers);
    /// Marks every resource as changed now. Called at construction and
    /// whenever the streams change.
    void touch();
    /// The port is only known once the server has bound it, which is after
    /// this router has to exist: sets api.endpoints, href and the control.
    void setEndpoint(const std::string& apiHost, uint16_t apiPort, const std::string& controlHref);
    ConnectionAPIServer::Reply route(const std::string& method, const std::string& path) const;
    std::string deviceId() const;
};
}
```

`route()`, `touch()` and `setEndpoint()` take one `mutable std::mutex`: the serving thread reads while the device thread sets the endpoint once.

Routes (all GET; any other method answers 405 with an IS-04 error object):

| Path | Body |
|---|---|
| `/x-nmos/node/` | `["v1.3/"]` |
| `/x-nmos/node/v1.3/` | `["self/","devices/","sources/","flows/","senders/","receivers/","subscriptions/"]` |
| `.../self` | node data |
| `.../devices` | `[<device data>]`; `.../devices/<id>` the object or 404 |
| `.../sources`, `.../flows`, `.../senders` | arrays, one per transmit stream, and by id |
| `.../receivers` | array, one per receive stream, and by id |
| `.../subscriptions` | 501 error object (websocket subscriptions are not served) |
| anything else under `/x-nmos/node/` | 404 error object |

Error object: `{"code": <status>, "error": "<text>", "debug": null}`. A trailing slash is accepted everywhere.

- [ ] **Step 1: Write the failing tests**

`Tests/TestNodeAPI.cpp`:

```cpp
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
```

- [ ] **Step 2: Register the suite and see it fail to build**

In `Tests/CMakeLists.txt`, after the `TestConnectionAPI` block (line 284):

```cmake
add_executable(TestNodeAPI
    TestNodeAPI.cpp
)
target_include_directories(TestNodeAPI PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(TestNodeAPI PRIVATE aes67_net)
target_link_libraries(TestNodeAPI PRIVATE doctest_headers)
target_compile_features(TestNodeAPI PRIVATE cxx_std_17)
```

After `add_test(NAME ConnectionAPI COMMAND TestConnectionAPI)`: `add_test(NAME NodeAPI COMMAND TestNodeAPI)`.
After the `ConnectionAPI PROPERTIES` line: `set_tests_properties(NodeAPI PROPERTIES LABELS "unit" TIMEOUT 60)`.

In the driver `CMakeLists.txt`, in the `AES67_NET_SOURCES` list next to `NetworkEngine/Discovery/ConnectionAPIServer.cpp`, add `NetworkEngine/Discovery/NodeAPIRouter.cpp`.

Run: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j --target TestNodeAPI`
Expected: fails, `NodeAPIRouter.h` not found.

- [ ] **Step 3: Implement**

`NetworkEngine/Discovery/NodeAPIRouter.h`:

```cpp
//
// NodeAPIRouter.h
// AES67 macOS Driver
//
// The IS-04 Node API, peer-to-peer: what a controller browsing the link
// reads to learn that this device exists and what it is made of. The
// registry client already builds every resource; this serves the same
// objects, unwrapped, at the paths IS-04 names, so a controller that reads
// them here and patches the Connection API is talking about one device.
//
// Pure: a request in, a reply out. The socket is ConnectionAPIServer's.
//
#pragma once

#include "NetworkEngine/Discovery/ConnectionAPIServer.h"
#include "NetworkEngine/Discovery/NMOSRegistrationClient.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace AES67 {

using NMOSSenderLister = std::function<std::vector<NMOSSenderResource>()>;
using NMOSReceiverLister = std::function<std::vector<NMOSReceiverResource>()>;

class NodeAPIRouter {
public:
    static constexpr const char* kApiVersion = "v1.3";

    /// `node.id` must be set: every other id is derived from it.
    /// `controlHref` is the IS-05 root the device advertises; empty means
    /// the device lists no control.
    NodeAPIRouter(NMOSNodeInfo node, std::string controlHref,
                  NMOSSenderLister senders, NMOSReceiverLister receivers);

    /// Marks every resource as changed now. IS-04 orders updates by a
    /// version stamp; a controller re-reads what moved.
    void touch();

    /// The port is only known once the server has bound it, which is after
    /// this router has to exist for the server to call. Sets the node's
    /// api.endpoints and href, and the device's control.
    void setEndpoint(const std::string& apiHost, uint16_t apiPort, const std::string& controlHref);

    ConnectionAPIServer::Reply route(const std::string& method, const std::string& path) const;

    std::string deviceId() const;

private:
    void touchLocked();
    ConnectionAPIServer::Reply error(int status, const std::string& text) const;
    std::string nodeData() const;
    std::string deviceData() const;

    /// The serving thread routes while the device thread sets the endpoint
    /// once at start-up; one lock covers both.
    mutable std::mutex mutex_;
    NMOSNodeInfo node_;
    std::string controlHref_;
    NMOSSenderLister senders_;
    NMOSReceiverLister receivers_;
    int64_t versionSeconds_{0};
    int32_t versionNanos_{0};
};

}  // namespace AES67
```

`NetworkEngine/Discovery/NodeAPIRouter.cpp`:

```cpp
//
// NodeAPIRouter.cpp
// AES67 macOS Driver
//

#include "NetworkEngine/Discovery/NodeAPIRouter.h"

#include <chrono>
#include <sstream>

namespace AES67 {

namespace {

std::vector<std::string> pathPieces(const std::string& path) {
    std::vector<std::string> pieces;
    std::string piece;
    for (const char c : path) {
        if (c == '/') {
            if (!piece.empty()) pieces.push_back(piece);
            piece.clear();
        } else {
            piece += c;
        }
    }
    if (!piece.empty()) pieces.push_back(piece);
    return pieces;
}

std::string jsonStringList(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ",";
        out += "\"" + items[i] + "\"";
    }
    return out + "]";
}

std::string jsonArrayOf(const std::vector<std::string>& objects) {
    std::string out = "[";
    for (size_t i = 0; i < objects.size(); ++i) {
        if (i) out += ",";
        out += objects[i];
    }
    return out + "]";
}

}  // namespace

NodeAPIRouter::NodeAPIRouter(NMOSNodeInfo node, std::string controlHref,
                             NMOSSenderLister senders, NMOSReceiverLister receivers)
    : node_(std::move(node)), controlHref_(std::move(controlHref)),
      senders_(std::move(senders)), receivers_(std::move(receivers)) {
    touchLocked();
}

void NodeAPIRouter::touchLocked() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
    versionSeconds_ = seconds.count();
    versionNanos_ = static_cast<int32_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds).count());
}

void NodeAPIRouter::touch() {
    std::lock_guard<std::mutex> lock(mutex_);
    touchLocked();
}

void NodeAPIRouter::setEndpoint(const std::string& apiHost, uint16_t apiPort,
                                const std::string& controlHref) {
    std::lock_guard<std::mutex> lock(mutex_);
    node_.apiHost = apiHost;
    node_.apiPort = apiPort;
    node_.href = "http://" + apiHost + ":" + std::to_string(apiPort) + "/";
    controlHref_ = controlHref;
    touchLocked();
}

std::string NodeAPIRouter::deviceId() const {
    return NMOSRegistrationClient::deriveId(node_.id, "device");
}

ConnectionAPIServer::Reply NodeAPIRouter::error(int status, const std::string& text) const {
    return {status, "application/json",
            "{\"code\": " + std::to_string(status) + ", \"error\": \"" + text +
                "\", \"debug\": null}"};
}

std::string NodeAPIRouter::nodeData() const {
    return NMOSRegistrationClient::buildNodeData(node_, versionSeconds_, versionNanos_);
}

std::string NodeAPIRouter::deviceData() const {
    std::vector<std::string> senderIds;
    for (const NMOSSenderResource& sender : senders_()) {
        senderIds.push_back(NMOSRegistrationClient::deriveId(node_.id, "sender:" + sender.name));
    }
    std::vector<std::string> receiverIds;
    for (const NMOSReceiverResource& receiver : receivers_()) {
        receiverIds.push_back(
            NMOSRegistrationClient::deriveId(node_.id, "receiver:" + receiver.name));
    }
    return NMOSRegistrationClient::buildDeviceData(deviceId(), node_.id, node_.label, senderIds,
                                                   receiverIds, controlHref_, versionSeconds_,
                                                   versionNanos_);
}

ConnectionAPIServer::Reply NodeAPIRouter::route(const std::string& method,
                                                const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::vector<std::string> pieces = pathPieces(path);
    if (pieces.size() < 2 || pieces[0] != "x-nmos" || pieces[1] != "node") {
        return error(404, "not found");
    }
    if (method != "GET") return error(405, "method not allowed");
    if (pieces.size() == 2) return {200, "application/json", jsonStringList({"v1.3/"})};
    if (pieces[2] != kApiVersion) return error(404, "version not served");
    if (pieces.size() == 3) {
        return {200, "application/json",
                jsonStringList({"self/", "devices/", "sources/", "flows/", "senders/",
                                "receivers/", "subscriptions/"})};
    }

    const std::string& collection = pieces[3];
    const bool wantsOne = pieces.size() >= 5;
    const std::string wantedId = wantsOne ? pieces[4] : std::string{};

    if (collection == "self") return {200, "application/json", nodeData()};
    if (collection == "subscriptions") return error(501, "subscriptions are not served");

    // Every collection is built the same way: the objects with their ids,
    // then either the whole list or the one asked for.
    std::vector<std::pair<std::string, std::string>> objects;  // id, data

    if (collection == "devices") {
        objects.emplace_back(deviceId(), deviceData());
    } else if (collection == "sources" || collection == "flows" || collection == "senders") {
        for (const NMOSSenderResource& sender : senders_()) {
            const std::string sourceId =
                NMOSRegistrationClient::deriveId(node_.id, "source:" + sender.name);
            const std::string flowId =
                NMOSRegistrationClient::deriveId(node_.id, "flow:" + sender.name);
            const std::string senderId =
                NMOSRegistrationClient::deriveId(node_.id, "sender:" + sender.name);
            if (collection == "sources") {
                objects.emplace_back(sourceId, NMOSRegistrationClient::buildSourceData(
                                                   sourceId, deviceId(), sender,
                                                   versionSeconds_, versionNanos_));
            } else if (collection == "flows") {
                objects.emplace_back(flowId, NMOSRegistrationClient::buildFlowData(
                                                 flowId, sourceId, deviceId(), sender,
                                                 versionSeconds_, versionNanos_));
            } else {
                objects.emplace_back(senderId, NMOSRegistrationClient::buildSenderData(
                                                   senderId, flowId, deviceId(), sender,
                                                   versionSeconds_, versionNanos_));
            }
        }
    } else if (collection == "receivers") {
        for (const NMOSReceiverResource& receiver : receivers_()) {
            const std::string receiverId =
                NMOSRegistrationClient::deriveId(node_.id, "receiver:" + receiver.name);
            objects.emplace_back(receiverId, NMOSRegistrationClient::buildReceiverData(
                                                 receiverId, deviceId(), receiver,
                                                 versionSeconds_, versionNanos_));
        }
    } else {
        return error(404, "not found");
    }

    if (!wantsOne) {
        std::vector<std::string> datas;
        for (const auto& object : objects) datas.push_back(object.second);
        return {200, "application/json", jsonArrayOf(datas)};
    }
    for (const auto& object : objects) {
        if (object.first == wantedId) return {200, "application/json", object.second};
    }
    return error(404, "no such resource");
}

}  // namespace AES67
```

- [ ] **Step 4: Build and run**

Run: `cmake --build build -j --target TestNodeAPI && ctest --test-dir build -R '^NodeAPI$' --output-on-failure`
Expected: PASS, 11 cases.

- [ ] **Step 5: Commit**

```bash
git add packages/aes67-macos-driver/NetworkEngine/Discovery/NodeAPIRouter.h \
        packages/aes67-macos-driver/NetworkEngine/Discovery/NodeAPIRouter.cpp \
        packages/aes67-macos-driver/Tests/TestNodeAPI.cpp \
        packages/aes67-macos-driver/Tests/CMakeLists.txt packages/aes67-macos-driver/CMakeLists.txt
git commit -m "feat(driver): IS-04 Node API, served peer-to-peer"
```

---

### Task 3: `ConnectionAPIServer` hands other `/x-nmos/` paths to a fallback router

**Files:**
- Modify: `packages/aes67-macos-driver/NetworkEngine/Discovery/ConnectionAPIServer.h:86-125`
- Modify: `packages/aes67-macos-driver/NetworkEngine/Discovery/ConnectionAPIServer.cpp:419-430` (`Impl::route`), plus `Impl` members and the public forwarder
- Test: `packages/aes67-macos-driver/Tests/TestConnectionAPI.cpp` (add cases)

**Interfaces:**
- Produces on `ConnectionAPIServer`:

```cpp
    using FallbackRouter = std::function<Reply(const std::string& method,
                                               const std::string& path,
                                               const std::string& body)>;
    /// Answers requests under /x-nmos/ that are not the Connection API
    /// (the Node API). Set before start(); the serving thread reads it.
    void setFallbackRouter(FallbackRouter router);
```

Behaviour: `GET /x-nmos/` and `GET /x-nmos` answer `["connection/","node/"]` when a fallback is set, `["connection/"]` otherwise. Any path whose second piece is not `connection` goes to the fallback when set; without one, 404 `[]` as today.

- [ ] **Step 1: Write the failing tests**

Append to `Tests/TestConnectionAPI.cpp`:

```cpp
TEST_CASE("paths outside the connection API go to the fallback router") {
    ConnectionAPIServer server(0);
    CHECK(server.route("GET", "/x-nmos/node/v1.3/self").status == 404);
    CHECK(server.route("GET", "/x-nmos/").body == "[\"connection/\"]");

    server.setFallbackRouter([](const std::string& method, const std::string& path,
                                const std::string&) {
        return ConnectionAPIServer::Reply{200, "text/plain", method + " " + path};
    });
    const auto reply = server.route("GET", "/x-nmos/node/v1.3/self");
    CHECK(reply.status == 200);
    CHECK(reply.body == "GET /x-nmos/node/v1.3/self");
    CHECK(server.route("GET", "/x-nmos/").body == "[\"connection/\",\"node/\"]");
    CHECK(server.route("GET", "/x-nmos").body == "[\"connection/\",\"node/\"]");
    // The Connection API itself is untouched.
    CHECK(server.route("GET", "/x-nmos/connection/v1.1/").body == "[\"single/\",\"bulk/\"]");
}
```

- [ ] **Step 2: Build and see it fail**

Run: `cmake --build build -j --target TestConnectionAPI`
Expected: compile error, no member `setFallbackRouter`.

- [ ] **Step 3: Implement**

In `ConnectionAPIServer.h`, inside the class after `parsePatch`:

```cpp
    using FallbackRouter = std::function<Reply(const std::string& method,
                                               const std::string& path,
                                               const std::string& body)>;
    /// Answers requests under /x-nmos/ that are not the Connection API:
    /// the Node API shares this port. Set before start(); the serving
    /// thread reads it without a lock.
    void setFallbackRouter(FallbackRouter router);
```

In `ConnectionAPIServer.cpp`, add to `Impl` a member `ConnectionAPIServer::FallbackRouter fallback_;` and the forwarder:

```cpp
void ConnectionAPIServer::setFallbackRouter(FallbackRouter router) {
    impl_->fallback_ = std::move(router);
}
```

Replace the head of `Impl::route` (the two checks for `pieces.size() < 3` and the version) with:

```cpp
    const std::vector<std::string> pieces = pathPieces(path);

    if (pieces.empty() || pieces[0] != "x-nmos") return {404, "application/json", "[]"};
    if (pieces.size() == 1) {
        // The APIs this port serves. The Node API is only listed when
        // something answers for it.
        return {200, "application/json",
                fallback_ ? jsonList({"connection/", "node/"}) : jsonList({"connection/"})};
    }
    if (pieces[1] != "connection") {
        if (fallback_) return fallback_(method, path, body);
        return {404, "application/json", "[]"};
    }
    if (pieces.size() < 3) return {404, "application/json", "[]"};
    if (pieces[2] != ConnectionAPIServer::kApiVersion) {
        return {404, "application/json", "[]"};
    }
```

Keep the rest of `route` as it is. `jsonList` already exists in that file (used at line 433).

- [ ] **Step 4: Build and run**

Run: `cmake --build build -j --target TestConnectionAPI && ctest --test-dir build -R '^ConnectionAPI$' --output-on-failure`
Expected: PASS, the new case and every existing one.

- [ ] **Step 5: Commit**

```bash
git add packages/aes67-macos-driver/NetworkEngine/Discovery/ConnectionAPIServer.h \
        packages/aes67-macos-driver/NetworkEngine/Discovery/ConnectionAPIServer.cpp \
        packages/aes67-macos-driver/Tests/TestConnectionAPI.cpp
git commit -m "feat(driver): the connection API's port also serves the node API"
```

---

### Task 4: `aes67_ravenna` as a driver dependency, and `NodeAdvertiser`

**Files:**
- Modify: `packages/aes67-ravenna/CMakeLists.txt:45-75`
- Modify: `packages/aes67-macos-driver/CMakeLists.txt:192-193` (after the core subdirectory), `:220-222` (sources), `:259-266` (link)
- Create: `packages/aes67-macos-driver/NetworkEngine/Discovery/NodeAdvertiser.h`
- Create: `packages/aes67-macos-driver/NetworkEngine/Discovery/NodeAdvertiser.cpp`
- Test: `packages/aes67-macos-driver/Tests/TestNodeAPI.cpp` (add a case for the pure builder), `Tests/CMakeLists.txt` (link)

**Interfaces:**
- Consumes: `AES67::Ravenna::MdnsResponder`, `AES67::Ravenna::SessionCatalogue`, `AES67::Ravenna::SessionAdvertisement`, `AES67::Ravenna::kNmosNodeService` from `Ravenna/MdnsResponder.h` and `Ravenna/DnsSd.h`.
- Produces:

```cpp
namespace AES67 {
/// The `_nmos-node._tcp` record set for this node. `addressV4` in host
/// byte order. `hostName` must end in ".local".
Ravenna::SessionAdvertisement nodeAdvertisement(const std::string& label,
                                                const std::string& hostName,
                                                uint32_t addressV4, uint16_t apiPort);

class NodeAdvertiser {
public:
    NodeAdvertiser();
    ~NodeAdvertiser();  // withdraws (goodbye) and stops
    /// Joins mDNS on `interfaceName` and announces `advertisement` three
    /// times a second apart, then answers queries on its own thread.
    bool start(const std::string& interfaceName,
               const Ravenna::SessionAdvertisement& advertisement, std::string& error);
    void stop();
};
}
```

- [ ] **Step 1: Write the failing test**

Append to `Tests/TestNodeAPI.cpp` (add `#include "NetworkEngine/Discovery/NodeAdvertiser.h"` at the top):

```cpp
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
```

- [ ] **Step 2: Wire the build and see the test fail**

`packages/aes67-ravenna/CMakeLists.txt`: wrap the tool and the tests in one option so a package that only wants the library does not build them. Replace from `add_executable(ravenna-announce ...` to the end of the file with:

```cmake
# The tool and the tests are what the package builds on its own. Another
# package that links the library (the macOS driver, for the mDNS responder)
# turns them off: it has a gate of its own and this package has one too.
option(AES67_RAVENNA_TOOLS_AND_TESTS "Build ravenna-announce and the test suites" ON)

if(AES67_RAVENNA_TOOLS_AND_TESTS)
    add_executable(ravenna-announce Tools/ravenna-announce.cpp)
    target_link_libraries(ravenna-announce PRIVATE aes67_ravenna)
    target_compile_options(ravenna-announce PRIVATE -Wall -Wextra -Wpedantic)

    option(BUILD_TESTS "Build the tests" ON)

    if(BUILD_TESTS)
        # (the existing BUILD_TESTS block, unchanged, indented one level)
    endif()
endif()
```

Keep the body of the `BUILD_TESTS` block exactly as it is today. Check `packages/aes67-ravenna/scripts/gate.sh` still configures with the default (ON): `grep -n cmake packages/aes67-ravenna/scripts/gate.sh`.

Driver `CMakeLists.txt`, right after the core `add_subdirectory` (line 193):

```cmake
# RAVENNA's session layer, for the mDNS responder that advertises the NMOS
# node. Library only: its tool and tests run in its own gate.
set(AES67_RAVENNA_TOOLS_AND_TESTS OFF CACHE BOOL "" FORCE)
if(NOT TARGET aes67_ravenna)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../aes67-ravenna
                     ${CMAKE_CURRENT_BINARY_DIR}/aes67-ravenna)
endif()
```

Sources: add `NetworkEngine/Discovery/NodeAdvertiser.cpp` next to `NodeAPIRouter.cpp` in `AES67_NET_SOURCES`. Link: `target_link_libraries(aes67_net PUBLIC aes67_core aes67_ravenna)` at line 227 (the advertiser is part of `aes67_net`, so the library carries the dependency and `AES67Driver` and the tests get it through it).

Run: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j --target TestNodeAPI`
Expected: fails, `NodeAdvertiser.h` not found. If CMake complains that `aes67_ravenna` requires C++20 while `aes67_net` is C++17, that is fine: the standard is per target and the header the driver includes (`Ravenna/MdnsResponder.h`) must compile as C++17. If it does not, report which construct breaks rather than raising the driver's standard.

- [ ] **Step 3: Implement**

`NetworkEngine/Discovery/NodeAdvertiser.h`:

```cpp
//
// NodeAdvertiser.h
// AES67 macOS Driver
//
// What makes a controller find this node without a registry: the
// _nmos-node._tcp service over mDNS (IS-04 peer-to-peer discovery). The
// records and the responder are aes67-ravenna's; this is the thread that
// runs them inside the driver, and the one record set the driver has to
// advertise.
//
#pragma once

#include "Ravenna/DnsSd.h"
#include "Ravenna/MdnsResponder.h"
#include "Ravenna/SessionCatalogue.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace AES67 {

/// The `_nmos-node._tcp` record set for this node. `addressV4` in host
/// byte order; `hostName` is the name the SRV points at and must end in
/// ".local".
Ravenna::SessionAdvertisement nodeAdvertisement(const std::string& label,
                                                const std::string& hostName,
                                                uint32_t addressV4, uint16_t apiPort);

class NodeAdvertiser {
public:
    NodeAdvertiser();
    ~NodeAdvertiser();

    NodeAdvertiser(const NodeAdvertiser&) = delete;
    NodeAdvertiser& operator=(const NodeAdvertiser&) = delete;

    /// Joins mDNS on `interfaceName`, announces three times a second apart
    /// (RFC 6762 sec 8.3), then answers queries until stop().
    bool start(const std::string& interfaceName,
               const Ravenna::SessionAdvertisement& advertisement, std::string& error);

    /// Withdraws the service with a zero TTL and joins the thread.
    void stop();

private:
    /// The responder wants a catalogue of RTSP sessions; the driver's
    /// sessions are announced over SAP, not RTSP, so this one stays empty
    /// and only the node is advertised.
    Ravenna::SessionCatalogue noSessions_;
    std::unique_ptr<Ravenna::MdnsResponder> responder_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace AES67
```

`NetworkEngine/Discovery/NodeAdvertiser.cpp`:

```cpp
//
// NodeAdvertiser.cpp
// AES67 macOS Driver
//

#include "NetworkEngine/Discovery/NodeAdvertiser.h"

#include <chrono>

namespace AES67 {

Ravenna::SessionAdvertisement nodeAdvertisement(const std::string& label,
                                                const std::string& hostName,
                                                uint32_t addressV4, uint16_t apiPort) {
    Ravenna::SessionAdvertisement node;
    node.instanceName = label;
    node.hostName = hostName;
    node.port = apiPort;
    node.addressV4 = addressV4;
    node.serviceType = Ravenna::kNmosNodeService;
    node.subtype.clear();
    // IS-04 sec 3: what a controller reads before it opens a connection.
    node.txtEntries = {"api_ver=v1.3", "api_proto=http", "api_auth=false", "ver_slf=0"};
    return node;
}

NodeAdvertiser::NodeAdvertiser() = default;

NodeAdvertiser::~NodeAdvertiser() { stop(); }

bool NodeAdvertiser::start(const std::string& interfaceName,
                           const Ravenna::SessionAdvertisement& advertisement,
                           std::string& error) {
    stop();
    responder_ = std::make_unique<Ravenna::MdnsResponder>(noSessions_);
    responder_->alsoAdvertise(advertisement);
    // The RTSP port the responder is told about is never advertised: the
    // catalogue is empty, so no session record carries it.
    if (!responder_->start(interfaceName, advertisement.hostName, advertisement.addressV4,
                           advertisement.port, error)) {
        responder_.reset();
        return false;
    }
    running_.store(true);
    thread_ = std::thread([this] {
        int announcements = 0;
        auto nextAnnouncement = std::chrono::steady_clock::now();
        while (running_.load()) {
            if (announcements < 3 && std::chrono::steady_clock::now() >= nextAnnouncement) {
                responder_->announce();
                ++announcements;
                nextAnnouncement += std::chrono::seconds(1);
            }
            responder_->service();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    return true;
}

void NodeAdvertiser::stop() {
    if (!running_.exchange(false)) {
        responder_.reset();
        return;
    }
    if (thread_.joinable()) thread_.join();
    if (responder_) responder_->goodbye();
    responder_.reset();
}

}  // namespace AES67
```

- [ ] **Step 4: Build and run**

Run: `cmake --build build -j && ctest --test-dir build -R '^NodeAPI$' --output-on-failure`
Expected: whole driver builds (this proves `AES67Driver` links `aes67_ravenna`), PASS with 12 cases.

- [ ] **Step 5: Commit**

```bash
git add packages/aes67-ravenna/CMakeLists.txt packages/aes67-macos-driver/CMakeLists.txt \
        packages/aes67-macos-driver/NetworkEngine/Discovery/NodeAdvertiser.h \
        packages/aes67-macos-driver/NetworkEngine/Discovery/NodeAdvertiser.cpp \
        packages/aes67-macos-driver/Tests/TestNodeAPI.cpp
git commit -m "feat(driver): advertise the NMOS node over mDNS with aes67-ravenna's responder"
```

---

### Task 5: The node starts with the device

**Files:**
- Modify: `packages/aes67-macos-driver/Driver/AES67Device.h:234-282` (members, declarations)
- Modify: `packages/aes67-macos-driver/Driver/AES67Device.cpp:362-456` (start), `:676-712` (`syncNMOSResources`), `:728-740` (destructor)

**Interfaces:**
- Consumes: `NodeAPIRouter`, `NodeAdvertiser`, `nodeAdvertisement`, `ConnectionAPIServer::setFallbackRouter`, `NMOSSettingsManager`, `NetworkInterfaceDetection::detectPTPInterface()` / `getInterfaceIPAddress()`.
- Produces on `AES67Device` (private): `std::vector<NMOSSenderResource> nmosSenderResources()`, `std::vector<NMOSReceiverResource> nmosReceiverResources()`, members `std::unique_ptr<NodeAPIRouter> nodeRouter_; std::unique_ptr<NodeAdvertiser> nodeAdvertiser_;`.

No unit test: this is wiring inside `coreaudiod`. Verified by the existing `DeviceActivationPlugIn` integration test still passing, the full driver building, and the live check in Task 12.

- [ ] **Step 1: Header**

In `AES67Device.h`, add includes:

```cpp
#include "NetworkEngine/Discovery/NodeAPIRouter.h"
#include "NetworkEngine/Discovery/NodeAdvertiser.h"
```

Add members next to `connectionServer_`:

```cpp
    /// The IS-04 Node API, on the Connection API's port, and the mDNS
    /// advertisement that lets a controller find it. Both run whenever the
    /// device does; registering with a registry (nmosClient_) is separate.
    std::unique_ptr<NodeAPIRouter> nodeRouter_;
    std::unique_ptr<NodeAdvertiser> nodeAdvertiser_;
```

Add declarations next to `connectionSenders()`:

```cpp
    /// The streams as IS-04 describes them, for the registry and the Node
    /// API alike.
    std::vector<NMOSSenderResource> nmosSenderResources();
    std::vector<NMOSReceiverResource> nmosReceiverResources();
```

- [ ] **Step 2: Extract the resource builders from `syncNMOSResources`**

In `AES67Device.cpp`, move the two loops of `syncNMOSResources()` (lines 678-701, building `senders` and `receivers`) into:

```cpp
std::vector<NMOSSenderResource> AES67Device::nmosSenderResources() {
    std::vector<NMOSSenderResource> senders;
    if (!streamManager_) return senders;
    for (const SDPSession& sdp : streamManager_->getTransmitSessions()) {
        NMOSSenderResource sender;
        sender.name = sdp.sessionName;
        sender.description = sdp.sessionInfo;
        sender.multicastAddress = sdp.connectionAddress;
        sender.port = sdp.port;
        sender.sourceAddress = sdp.originAddress;
        sender.sampleRate = static_cast<uint32_t>(sdp.sampleRate);
        sender.channels = sdp.numChannels;
        sender.encoding = sdp.encoding.empty() ? "L24" : sdp.encoding;
        senders.push_back(std::move(sender));
    }
    return senders;
}

std::vector<NMOSReceiverResource> AES67Device::nmosReceiverResources() {
    std::vector<NMOSReceiverResource> receivers;
    if (!streamManager_) return receivers;
    for (const SDPSession& sdp : streamManager_->getReceiveSessions()) {
        NMOSReceiverResource receiver;
        receiver.name = sdp.sessionName;
        receiver.description = sdp.sessionInfo;
        receiver.subscribedMulticastAddress = sdp.connectionAddress;
        // A receive stream that exists is a receiver that is taking
        // something: this driver does not keep idle receivers around.
        receiver.active = true;
        receivers.push_back(std::move(receiver));
    }
    return receivers;
}
```

and make `syncNMOSResources()` start with:

```cpp
    if (!nmosClient_ || !streamManager_) return;
    const std::vector<NMOSSenderResource> senders = nmosSenderResources();
    const std::vector<NMOSReceiverResource> receivers = nmosReceiverResources();
```

keeping the `controlHref` and `syncResources` lines that follow.

- [ ] **Step 3: Restructure the start**

Replace the block from `// NMOS. Off unless the installation asked for it:` (line 362) through the closing of `if (nmosSettings.enabled) { ... }` (line 456) with the following. The registry part is the existing code moved, not rewritten; keep its comments.

```cpp
    // NMOS. The node -- IS-04 Node API, IS-05 Connection API, and the mDNS
    // advertisement that lets a controller find them -- runs whenever the
    // device does: it is what makes this Mac one more device a controller
    // can route, and a RAVENNA card does the same without asking.
    // Registering with a registry is separate and stays opt-in: it puts
    // this machine in whatever reads the plant's registry, which is a
    // decision somebody makes rather than something a driver starts doing
    // on its own.
    {
        NMOSSettingsManager nmosSettingsManager;
        NMOSSettings nmosSettings = nmosSettingsManager.load();
        // The node id has to be the same across restarts, or every restart
        // looks like a new device. First run has none: generate and persist.
        if (nmosSettings.nodeId.empty()) {
            nmosSettingsManager.save(nmosSettings);
        }
        nmosNodeId_ = nmosSettings.nodeId;

        // The address the node is reached at: the interface the audio
        // uses, as an IP rather than a hostname, so a controller on the
        // segment needs no resolver to follow the hrefs.
        const std::string nodeInterface = NetworkInterfaceDetection::detectPTPInterface();
        const std::string nodeAddress = nodeInterface.empty()
            ? std::string{}
            : NetworkInterfaceDetection::getInterfaceIPAddress(nodeInterface);
        const std::string apiHost = nodeAddress.empty() ? localHostname() : nodeAddress;

        NMOSNodeInfo node;
        node.id = nmosNodeId_;
        node.hostname = localHostname();
        node.label = nmosSettings.label.empty()
                         ? ("AES67 macOS Driver on " + node.hostname)
                         : nmosSettings.label;

        // IS-05. Bound to an ephemeral port: this is a user-space driver
        // and the port it gets is what it advertises.
        connectionServer_ = std::make_unique<ConnectionAPIServer>(0);
        std::string controlHref;
        nodeRouter_ = std::make_unique<NodeAPIRouter>(
            node, std::string{}, [this] { return nmosSenderResources(); },
            [this] { return nmosReceiverResources(); });
        connectionServer_->setFallbackRouter(
            [this](const std::string& method, const std::string& path, const std::string&) {
                return nodeRouter_->route(method, path);
            });
        const bool connectionStarted = connectionServer_->start(
            [this] { return connectionSenders(); },
            [this] { return connectionReceivers(); },
            [this](const std::string& id, const ConnectionPatch& patch) {
                return applyConnectionPatch(id, patch);
            });
        if (connectionStarted) {
            controlHref = connectionServer_->controlHref(apiHost);
            node.apiHost = apiHost;
            node.apiPort = connectionServer_->boundPort();
            node.href = "http://" + apiHost + ":" + std::to_string(node.apiPort) + "/";
            // The router had to exist before start() so the first request
            // finds it; the port it now names is the one the server got.
            nodeRouter_->setEndpoint(apiHost, node.apiPort, controlHref);
            AES67_LOGF("AES67Device: NMOS node API and IS-05 connection API on %s:%u",
                       apiHost.c_str(), connectionServer_->boundPort());

            // mDNS, so a controller browsing the link finds the node. The
            // SRV name is <host>.local: whatever gethostname() returns,
            // minus any domain it carries.
            std::string shortHost = node.hostname.substr(0, node.hostname.find('.'));
            if (shortHost.empty()) shortHost = "aes67";
            uint32_t addressV4 = 0;
            {
                struct in_addr parsed{};
                if (!nodeAddress.empty() && ::inet_aton(nodeAddress.c_str(), &parsed) == 1) {
                    addressV4 = ntohl(parsed.s_addr);
                }
            }
            if (!nodeInterface.empty() && addressV4 != 0) {
                nodeAdvertiser_ = std::make_unique<NodeAdvertiser>();
                std::string error;
                if (!nodeAdvertiser_->start(nodeInterface,
                                            nodeAdvertisement(node.label, shortHost + ".local",
                                                              addressV4,
                                                              connectionServer_->boundPort()),
                                            error)) {
                    AES67_LOGF("AES67Device: NMOS node not advertised over mDNS: %s",
                               error.c_str());
                    nodeAdvertiser_.reset();
                } else {
                    AES67_LOGF("AES67Device: NMOS node advertised as \"%s\" on %s",
                               node.label.c_str(), nodeInterface.c_str());
                }
            } else {
                AES67_LOG("AES67Device: no interface address - NMOS node not advertised");
            }
        } else {
            AES67_LOG("AES67Device: IS-05 connection API unavailable - node not served");
            connectionServer_.reset();
            nodeRouter_.reset();
        }

        // A stream added or removed changes what the node describes: the
        // Node API's versions move, and the registry, when there is one,
        // is told from its own thread.
        streamManager_->setStreamAddedCallback([this](const StreamInfo&) {
            if (nodeRouter_) nodeRouter_->touch();
            requestNMOSSync();
        });
        streamManager_->setStreamRemovedCallback([this](const StreamInfo&) {
            if (nodeRouter_) nodeRouter_->touch();
            requestNMOSSync();
        });

        if (nmosSettings.enabled) {
            nmosClient_ = std::make_unique<NMOSRegistrationClient>(node);

            std::optional<NMOSRegistry> registry;
            if (!nmosSettings.registryOverride.empty()) {
                registry = parseRegistryOverride(nmosSettings.registryOverride);
                if (!registry.has_value()) {
                    AES67_LOGF("AES67Device: NMOS registry override '%s' is not host:port",
                               nmosSettings.registryOverride.c_str());
                }
            } else {
                registry = NMOSRegistrationClient::discoverRegistry();
            }

            if (registry.has_value() && nmosClient_->registerWith(*registry)) {
                nmosClient_->startHeartbeats();
                AES67_LOGF("AES67Device: registered with the NMOS registry at %s:%u as %s",
                           registry->host.c_str(), registry->port, node.id.c_str());
                {
                    std::lock_guard<std::mutex> lock(nmosSyncMutex_);
                    nmosSyncRunning_ = true;
                }
                nmosSyncThread_ = std::thread([this] {
                    for (;;) {
                        {
                            std::unique_lock<std::mutex> lock(nmosSyncMutex_);
                            nmosSyncSignal_.wait(lock, [this] {
                                return nmosSyncRequested_ || !nmosSyncRunning_;
                            });
                            if (!nmosSyncRunning_) return;
                            nmosSyncRequested_ = false;
                        }
                        syncNMOSResources();
                    }
                });
                requestNMOSSync();
            } else {
                AES67_LOG("AES67Device: no NMOS registry registered with - continuing without it");
                nmosClient_.reset();
            }
        }
    }
```

Check the file already includes `<arpa/inet.h>` (for `inet_aton`, `ntohl`); add it if not. The `node` passed to `NMOSRegistrationClient` for the registry carries the same `apiHost`, `apiPort` and `href`, so the registry's copy of the node names the served endpoint too.

- [ ] **Step 4: Destructor**

In `~AES67Device()`, before `if (connectionServer_) connectionServer_->stop();`, add `if (nodeAdvertiser_) nodeAdvertiser_->stop();` (the goodbye goes out while the port it names is still bound).

- [ ] **Step 5: Build everything and run the touched suites**

Run: `cmake --build build -j && ctest --test-dir build -R 'NodeAPI|ConnectionAPI|NMOSRegistration|DeviceActivation' --output-on-failure`
Expected: build clean, all PASS.

- [ ] **Step 6: Commit**

```bash
git add packages/aes67-macos-driver/Driver/AES67Device.h packages/aes67-macos-driver/Driver/AES67Device.cpp
git commit -m "feat(driver): the NMOS node runs whenever the device does"
```

---

### Task 6: `NmosResources.swift`, the pure half of the controller

**Files:**
- Create: `packages/aes67-macos-driver/ManagerApp/Models/NmosResources.swift`
- Create: `packages/aes67-macos-driver/ManagerApp/Tests/NmosResourcesTests.swift`
- Modify: `packages/aes67-macos-driver/ManagerApp/Tests/main.swift`, `packages/aes67-macos-driver/ManagerApp/run-tests.sh:12-17`, `packages/aes67-macos-driver/ManagerApp/build.sh` (source list)

**Interfaces (produces):**

```swift
struct NmosSender: Identifiable, Equatable { let id: String; let label: String; let nodeId: String; let channels: Int? }
struct NmosReceiver: Identifiable, Equatable {
    let id: String; let label: String; let nodeId: String
    var activeSenderId: String? = nil; var masterEnable: Bool = false
}
struct NmosNode: Identifiable, Equatable {
    let id: String; let label: String; let host: String; let port: Int
    var connectionRoot: URL? = nil      // IS-05 root, nil = read-only node
    var senders: [NmosSender] = []; var receivers: [NmosReceiver] = []
    var reachable: Bool = true
    var nodeRoot: URL { URL(string: "http://\(host):\(port)/x-nmos/node/v1.3/")! }
}
enum NmosDecodingError: Error { case shape(String) }
enum NmosDecoding {
    static func nodeSelf(_ data: Data) throws -> (id: String, label: String)
    static func connectionRoot(devices data: Data) throws -> URL?
    static func senders(_ senders: Data, flows: Data, sources: Data, nodeId: String) throws -> [NmosSender]
    static func receivers(_ data: Data, nodeId: String) throws -> [NmosReceiver]
    static func active(_ data: Data) throws -> (senderId: String?, masterEnable: Bool)
    static func errorText(_ data: Data) -> String?      // IS-05 error body → "error (debug)"
}
enum NmosPatch {
    static func connect(senderId: String, sdp: String) -> Data
    static func disconnect() -> Data
}
struct RoutingMatrix: Equatable {
    struct Column: Identifiable, Equatable { let id: String; let nodeLabel: String; let label: String; let reachable: Bool; let sender: NmosSender }
    struct Row: Identifiable, Equatable { let id: String; let nodeLabel: String; let label: String; let reachable: Bool; let writable: Bool; let receiver: NmosReceiver; let node: NmosNode }
    enum Cell: Equatable { case off, on, unavailable }
    let columns: [Column]; let rows: [Row]
    static func build(from nodes: [NmosNode]) -> RoutingMatrix
    func cell(row: Row, column: Column) -> Cell
    func node(forSender id: String) -> NmosNode?
}
```

`Cell.on`: the row's receiver has `masterEnable` and `activeSenderId == column.id`. `.unavailable`: either node unreachable, or the row's node has no `connectionRoot` (not writable). Otherwise `.off`. Sort: nodes by label (case-insensitive), then senders/receivers by label. Sender channel count: the sender's `flow_id` → flow's `source_id` → source's `channels` array count; `nil` when any link is missing. Receivers carry no channel count in IS-04, so the view shows none for them (the spec's "channel count in parentheses" applies where IS-04 provides one).

- [ ] **Step 1: Write the failing tests**

`Tests/NmosResourcesTests.swift`:

```swift
//
// NmosResourcesTests.swift
// AES67 Manager
//
// The pure half of the NMOS controller: what IS-04 and IS-05 JSON means,
// what a PATCH says, and how nodes become a matrix. No sockets.
//

import Foundation

private let senders = """
[{"id":"s1","label":"Mix A","flow_id":"f1","device_id":"d1"},
 {"id":"s2","label":"Mix B","flow_id":"f2","device_id":"d1"}]
""".data(using: .utf8)!
private let flows = """
[{"id":"f1","source_id":"src1"},{"id":"f2","source_id":"missing"}]
""".data(using: .utf8)!
private let sources = """
[{"id":"src1","channels":[{"label":"L"},{"label":"R"}]}]
""".data(using: .utf8)!
private let receivers = """
[{"id":"r1","label":"Return 1","device_id":"d1","subscription":{"sender_id":null,"active":false}}]
""".data(using: .utf8)!
private let devices = """
[{"id":"d1","label":"Dev","controls":[
  {"href":"http://10.0.0.5:8080/x-nmos/connection/v1.1/","type":"urn:x-nmos:control:sr-ctrl/v1.1"}]}]
""".data(using: .utf8)!
private let devicesNoControl = """
[{"id":"d1","label":"Dev","controls":[]}]
""".data(using: .utf8)!
private let selfBody = """
{"id":"n1","label":"Studio Mac","hostname":"studio-mac"}
""".data(using: .utf8)!

/// Every check in this file. Called from main.swift.
func runNmosResourcesTests() {
    // Decoding
    do {
        let node = try NmosDecoding.nodeSelf(selfBody)
        checkEqual(node.id, "n1", "self id")
        checkEqual(node.label, "Studio Mac", "self label")

        let root = try NmosDecoding.connectionRoot(devices: devices)
        checkEqual(root?.absoluteString, "http://10.0.0.5:8080/x-nmos/connection/v1.1/", "IS-05 root")
        check(try NmosDecoding.connectionRoot(devices: devicesNoControl) == nil, "no control means nil root")

        let decodedSenders = try NmosDecoding.senders(senders, flows: flows, sources: sources, nodeId: "n1")
        checkEqual(String(decodedSenders.count), "2", "two senders")
        checkEqual(decodedSenders[0].label, "Mix A", "sender label")
        check(decodedSenders[0].channels == 2, "channels through flow and source")
        check(decodedSenders[1].channels == nil, "a broken source link is no count")
        checkEqual(decodedSenders[0].nodeId, "n1", "sender carries its node")

        let decodedReceivers = try NmosDecoding.receivers(receivers, nodeId: "n1")
        checkEqual(decodedReceivers.first?.label, "Return 1", "receiver label")
        check(decodedReceivers.first?.activeSenderId == nil, "receiver starts with no sender")

        let active = try NmosDecoding.active("""
            {"sender_id":"s1","master_enable":true,"activation":{"mode":null}}
            """.data(using: .utf8)!)
        checkEqual(active.senderId, "s1", "active sender")
        check(active.masterEnable, "active master_enable")
        let idle = try NmosDecoding.active("""
            {"sender_id":null,"master_enable":false}
            """.data(using: .utf8)!)
        check(idle.senderId == nil && !idle.masterEnable, "idle receiver")
    } catch {
        check(false, "decoding threw: \(error)")
    }
    check((try? NmosDecoding.nodeSelf("[]".data(using: .utf8)!)) == nil, "wrong shape throws")
    checkEqual(NmosDecoding.errorText("""
        {"code":400,"error":"no room","debug":"needs 8 channels"}
        """.data(using: .utf8)!), "no room (needs 8 channels)", "error text with debug")
    checkEqual(NmosDecoding.errorText("""
        {"code":500,"error":"refused","debug":null}
        """.data(using: .utf8)!), "refused", "error text without debug")

    // PATCH bodies
    let connect = try! JSONSerialization.jsonObject(with: NmosPatch.connect(senderId: "s1", sdp: "v=0\r\n")) as! [String: Any]
    checkEqual(connect["sender_id"] as? String, "s1", "connect sender_id")
    check(connect["master_enable"] as? Bool == true, "connect master_enable")
    let file = connect["transport_file"] as? [String: Any]
    checkEqual(file?["data"] as? String, "v=0\r\n", "connect transport_file data")
    checkEqual(file?["type"] as? String, "application/sdp", "connect transport_file type")
    checkEqual((connect["activation"] as? [String: Any])?["mode"] as? String, "activate_immediate", "connect activation")

    let disconnect = try! JSONSerialization.jsonObject(with: NmosPatch.disconnect()) as! [String: Any]
    check(disconnect["master_enable"] as? Bool == false, "disconnect master_enable")
    check(disconnect["sender_id"] is NSNull, "disconnect sender_id is null")
    checkEqual((disconnect["activation"] as? [String: Any])?["mode"] as? String, "activate_immediate", "disconnect activation")

    // Matrix
    var mac = NmosNode(id: "n1", label: "Studio Mac", host: "10.0.0.5", port: 8080,
                       connectionRoot: URL(string: "http://10.0.0.5:8080/x-nmos/connection/v1.1/"))
    mac.senders = [NmosSender(id: "s1", label: "Mix A", nodeId: "n1", channels: 2)]
    mac.receivers = [NmosReceiver(id: "r1", label: "Return 1", nodeId: "n1", activeSenderId: "c1", masterEnable: true)]
    var card = NmosNode(id: "n2", label: "card", host: "10.0.0.6", port: 80, connectionRoot: nil)
    card.senders = [NmosSender(id: "c1", label: "Out", nodeId: "n2", channels: nil)]
    card.receivers = [NmosReceiver(id: "cr", label: "In", nodeId: "n2")]

    let matrix = RoutingMatrix.build(from: [mac, card])
    checkEqual(matrix.columns.map { $0.label }.joined(separator: ","), "Out,Mix A", "columns sorted by node label, case-insensitive")
    checkEqual(matrix.rows.map { $0.label }.joined(separator: ","), "In,Return 1", "rows sorted the same way")
    let macRow = matrix.rows[1], cardRow = matrix.rows[0]
    let outColumn = matrix.columns[0], mixColumn = matrix.columns[1]
    check(matrix.cell(row: macRow, column: outColumn) == .on, "active sender is on")
    check(matrix.cell(row: macRow, column: mixColumn) == .off, "other sender is off")
    check(matrix.cell(row: cardRow, column: mixColumn) == .unavailable, "a node without IS-05 is not writable")
    check(!cardRow.writable, "writable follows connectionRoot")
    checkEqual(matrix.node(forSender: "c1")?.id, "n2", "sender to node")

    card.reachable = false
    let withDown = RoutingMatrix.build(from: [mac, card])
    check(withDown.cell(row: withDown.rows[1], column: withDown.columns[0]) == .unavailable, "an unreachable sender node greys the column")
    check(!withDown.columns[0].reachable, "column carries reachability")
}
```

`Tests/main.swift` becomes:

```swift
import Foundation

runPrivilegedScriptTests()
runNmosResourcesTests()

print("\(checks) checks, \(failures) failures")
exit(failures == 0 ? 0 : 1)
```

`run-tests.sh` source list gains `Models/NmosResources.swift` and `Tests/NmosResourcesTests.swift` (before `Tests/main.swift`). `build.sh` source list gains `Models/NmosResources.swift` after `Models/MenuBarManager.swift`.

- [ ] **Step 2: Run to see it fail**

Run: `cd packages/aes67-macos-driver/ManagerApp && ./run-tests.sh`
Expected: compile error, `NmosDecoding` undefined.

- [ ] **Step 3: Implement `Models/NmosResources.swift`**

```swift
//
// NmosResources.swift
// AES67 Manager
//
// The pure half of the NMOS controller: what an IS-04 node says, what an
// IS-05 receiver holds, what a PATCH asks, and how a set of nodes becomes
// the routing matrix. No network here, so run-tests.sh can cover it.
//

import Foundation

struct NmosSender: Identifiable, Equatable {
    let id: String
    let label: String
    let nodeId: String
    /// From the sender's flow's source, when the chain is intact.
    let channels: Int?
}

struct NmosReceiver: Identifiable, Equatable {
    let id: String
    let label: String
    let nodeId: String
    /// IS-05 `active`: the truth about what this receiver is taking.
    var activeSenderId: String? = nil
    var masterEnable: Bool = false
}

struct NmosNode: Identifiable, Equatable {
    let id: String
    let label: String
    let host: String
    let port: Int
    /// The IS-05 root from the device's sr-ctrl control. nil: read-only.
    var connectionRoot: URL? = nil
    var senders: [NmosSender] = []
    var receivers: [NmosReceiver] = []
    var reachable: Bool = true

    var nodeRoot: URL { URL(string: "http://\(host):\(port)/x-nmos/node/v1.3/")! }
}

enum NmosDecodingError: Error {
    case shape(String)
}

enum NmosDecoding {
    private static func object(_ data: Data) throws -> [String: Any] {
        guard let object = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw NmosDecodingError.shape("expected a JSON object")
        }
        return object
    }

    private static func array(_ data: Data) throws -> [[String: Any]] {
        guard let array = try JSONSerialization.jsonObject(with: data) as? [[String: Any]] else {
            throw NmosDecodingError.shape("expected a JSON array of objects")
        }
        return array
    }

    static func nodeSelf(_ data: Data) throws -> (id: String, label: String) {
        let node = try object(data)
        guard let id = node["id"] as? String else { throw NmosDecodingError.shape("self has no id") }
        return (id, node["label"] as? String ?? id)
    }

    /// The first sr-ctrl control of the first device that has one.
    static func connectionRoot(devices data: Data) throws -> URL? {
        for device in try array(data) {
            for control in device["controls"] as? [[String: Any]] ?? [] {
                guard let type = control["type"] as? String,
                      type.hasPrefix("urn:x-nmos:control:sr-ctrl/v1."),
                      let href = control["href"] as? String,
                      let url = URL(string: href.hasSuffix("/") ? href : href + "/") else { continue }
                return url
            }
        }
        return nil
    }

    static func senders(_ senders: Data, flows: Data, sources: Data, nodeId: String) throws -> [NmosSender] {
        var sourceOfFlow: [String: String] = [:]
        for flow in try array(flows) {
            if let id = flow["id"] as? String, let source = flow["source_id"] as? String {
                sourceOfFlow[id] = source
            }
        }
        var channelsOfSource: [String: Int] = [:]
        for source in try array(sources) {
            if let id = source["id"] as? String, let channels = source["channels"] as? [Any] {
                channelsOfSource[id] = channels.count
            }
        }
        return try array(senders).map { sender in
            guard let id = sender["id"] as? String else { throw NmosDecodingError.shape("sender has no id") }
            var channels: Int? = nil
            if let flow = sender["flow_id"] as? String, let source = sourceOfFlow[flow] {
                channels = channelsOfSource[source]
            }
            return NmosSender(id: id, label: sender["label"] as? String ?? id, nodeId: nodeId, channels: channels)
        }
    }

    static func receivers(_ data: Data, nodeId: String) throws -> [NmosReceiver] {
        return try array(data).map { receiver in
            guard let id = receiver["id"] as? String else { throw NmosDecodingError.shape("receiver has no id") }
            return NmosReceiver(id: id, label: receiver["label"] as? String ?? id, nodeId: nodeId)
        }
    }

    static func active(_ data: Data) throws -> (senderId: String?, masterEnable: Bool) {
        let active = try object(data)
        return (active["sender_id"] as? String, active["master_enable"] as? Bool ?? false)
    }

    /// "error (debug)" from an IS-05 error body, or nil when it is not one.
    static func errorText(_ data: Data) -> String? {
        guard let body = try? object(data), let error = body["error"] as? String else { return nil }
        if let debug = body["debug"] as? String, !debug.isEmpty { return "\(error) (\(debug))" }
        return error
    }
}

enum NmosPatch {
    static func connect(senderId: String, sdp: String) -> Data {
        let body: [String: Any] = [
            "sender_id": senderId,
            "master_enable": true,
            "transport_file": ["data": sdp, "type": "application/sdp"],
            "activation": ["mode": "activate_immediate"],
        ]
        return try! JSONSerialization.data(withJSONObject: body)
    }

    static func disconnect() -> Data {
        let body: [String: Any] = [
            "sender_id": NSNull(),
            "master_enable": false,
            "activation": ["mode": "activate_immediate"],
        ]
        return try! JSONSerialization.data(withJSONObject: body)
    }
}

struct RoutingMatrix: Equatable {
    struct Column: Identifiable, Equatable {
        let id: String
        let nodeLabel: String
        let label: String
        let reachable: Bool
        let sender: NmosSender
    }

    struct Row: Identifiable, Equatable {
        let id: String
        let nodeLabel: String
        let label: String
        let reachable: Bool
        /// The node serves IS-05, so a click can do something.
        let writable: Bool
        let receiver: NmosReceiver
        let node: NmosNode
    }

    enum Cell: Equatable {
        case off
        case on
        /// Unreachable node on either axis, or a receiver nobody can patch.
        case unavailable
    }

    let columns: [Column]
    let rows: [Row]
    private let nodesById: [String: NmosNode]

    static func build(from nodes: [NmosNode]) -> RoutingMatrix {
        let byLabel: (String, String) -> Bool = {
            $0.localizedCaseInsensitiveCompare($1) == .orderedAscending
        }
        let sorted = nodes.sorted { byLabel($0.label, $1.label) }
        var columns: [Column] = []
        var rows: [Row] = []
        for node in sorted {
            for sender in node.senders.sorted(by: { byLabel($0.label, $1.label) }) {
                columns.append(Column(id: sender.id, nodeLabel: node.label, label: sender.label,
                                      reachable: node.reachable, sender: sender))
            }
            for receiver in node.receivers.sorted(by: { byLabel($0.label, $1.label) }) {
                rows.append(Row(id: receiver.id, nodeLabel: node.label, label: receiver.label,
                                reachable: node.reachable, writable: node.connectionRoot != nil,
                                receiver: receiver, node: node))
            }
        }
        return RoutingMatrix(columns: columns, rows: rows,
                             nodesById: Dictionary(uniqueKeysWithValues: nodes.map { ($0.id, $0) }))
    }

    func cell(row: Row, column: Column) -> Cell {
        if !row.reachable || !column.reachable || !row.writable { return .unavailable }
        if row.receiver.masterEnable && row.receiver.activeSenderId == column.id { return .on }
        return .off
    }

    func node(forSender id: String) -> NmosNode? {
        return nodesById[columns.first { $0.id == id }?.sender.nodeId ?? ""]
    }

    static func == (lhs: RoutingMatrix, rhs: RoutingMatrix) -> Bool {
        return lhs.columns == rhs.columns && lhs.rows == rhs.rows
    }
}
```

- [ ] **Step 4: Run the tests**

Run: `./run-tests.sh`
Expected: `N checks, 0 failures`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add packages/aes67-macos-driver/ManagerApp/Models/NmosResources.swift \
        packages/aes67-macos-driver/ManagerApp/Tests/NmosResourcesTests.swift \
        packages/aes67-macos-driver/ManagerApp/Tests/main.swift \
        packages/aes67-macos-driver/ManagerApp/run-tests.sh packages/aes67-macos-driver/ManagerApp/build.sh
git commit -m "feat(manager): NMOS resources, patches and the routing matrix as values"
```

---

### Task 7: `NmosController`, discovery and the network calls

**Files:**
- Create: `packages/aes67-macos-driver/ManagerApp/Models/NmosController.swift`
- Modify: `packages/aes67-macos-driver/ManagerApp/build.sh` (add the file after `Models/NmosResources.swift`)

**Interfaces:**
- Consumes: everything in Task 6.
- Produces:

```swift
@MainActor final class NmosController: NSObject, ObservableObject {
    @Published private(set) var nodes: [NmosNode]
    @Published private(set) var matrix: RoutingMatrix
    @Published private(set) var inFlight: Set<String>      // receiver ids with a PATCH pending
    @Published private(set) var lastRead: Date?
    @Published var lastError: String?                      // shown by the view as an alert
    func start()      // browse + first read; idempotent
    func stop()       // stop browsing and the refresh timer
    func refresh()    // re-read every node now
    func connect(receiver: NmosReceiver, to sender: NmosSender)
    func disconnect(receiver: NmosReceiver)
}
```

No host test: it is sockets and timers end to end. Verified in Task 9 against `ravenna-announce`.

- [ ] **Step 1: Implement**

```swift
//
// NmosController.swift
// AES67 Manager
//
// The NMOS controller: finds nodes on the link (IS-04 peer-to-peer over
// Bonjour), reads what each one has, and patches receivers onto senders
// over IS-05. What the JSON means and how nodes become a matrix is
// NmosResources.swift; this is the part that talks.
//
// State lives on the devices. Nothing here is saved: the matrix is
// re-read, and after every PATCH the receiver's `active` is what is shown,
// not what was asked.
//

import Foundation

@MainActor
final class NmosController: NSObject, ObservableObject {
    static let serviceType = "_nmos-node._tcp."
    static let refreshInterval: TimeInterval = 5

    @Published private(set) var nodes: [NmosNode] = []
    @Published private(set) var matrix = RoutingMatrix.build(from: [])
    @Published private(set) var inFlight: Set<String> = []
    @Published private(set) var lastRead: Date? = nil
    @Published var lastError: String? = nil

    private let browser = NetServiceBrowser()
    /// Services being resolved. NetService needs a strong reference until
    /// its delegate hears back.
    private var resolving: [NetService] = []
    /// Resolved endpoints by service name, kept until Bonjour withdraws them.
    private var endpoints: [String: (host: String, port: Int)] = [:]
    private var refreshTimer: Timer?
    private var started = false
    private let session: URLSession = {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 3
        return URLSession(configuration: configuration)
    }()

    override init() {
        super.init()
        browser.delegate = self
    }

    func start() {
        guard !started else { return }
        started = true
        browser.searchForServices(ofType: Self.serviceType, inDomain: "local.")
        refreshTimer = Timer.scheduledTimer(withTimeInterval: Self.refreshInterval, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
    }

    func stop() {
        guard started else { return }
        started = false
        browser.stop()
        refreshTimer?.invalidate()
        refreshTimer = nil
    }

    func refresh() {
        let targets = endpoints
        Task { [weak self] in
            guard let self else { return }
            var read: [NmosNode] = []
            for (name, endpoint) in targets {
                read.append(await self.readNode(name: name, host: endpoint.host, port: endpoint.port))
            }
            self.publish(read)
        }
    }

    func connect(receiver: NmosReceiver, to sender: NmosSender) {
        guard let receiverNode = nodes.first(where: { $0.id == receiver.nodeId }),
              let senderNode = nodes.first(where: { $0.id == sender.nodeId }),
              let receiverRoot = receiverNode.connectionRoot,
              let senderRoot = senderNode.connectionRoot else {
            lastError = "That receiver or sender is on a node without a connection API."
            return
        }
        inFlight.insert(receiver.id)
        Task { [weak self] in
            guard let self else { return }
            defer { self.inFlight.remove(receiver.id) }
            do {
                let sdp = try await self.getText(senderRoot.appendingPathComponent("single/senders/\(sender.id)/transportfile"))
                try await self.patch(receiverRoot.appendingPathComponent("single/receivers/\(receiver.id)/staged"),
                                     body: NmosPatch.connect(senderId: sender.id, sdp: sdp))
            } catch {
                self.lastError = error.localizedDescription
            }
            self.refresh()
        }
    }

    func disconnect(receiver: NmosReceiver) {
        guard let receiverNode = nodes.first(where: { $0.id == receiver.nodeId }),
              let receiverRoot = receiverNode.connectionRoot else { return }
        inFlight.insert(receiver.id)
        Task { [weak self] in
            guard let self else { return }
            defer { self.inFlight.remove(receiver.id) }
            do {
                try await self.patch(receiverRoot.appendingPathComponent("single/receivers/\(receiver.id)/staged"),
                                     body: NmosPatch.disconnect())
            } catch {
                self.lastError = error.localizedDescription
            }
            self.refresh()
        }
    }

    // MARK: - Reading a node

    private struct HTTPError: LocalizedError {
        let status: Int
        let text: String?
        var errorDescription: String? { text.map { "HTTP \(status): \($0)" } ?? "HTTP \(status)" }
    }

    private func getData(_ url: URL) async throws -> Data {
        let (data, response) = try await session.data(from: url)
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        guard (200..<300).contains(status) else {
            throw HTTPError(status: status, text: NmosDecoding.errorText(data))
        }
        return data
    }

    private func getText(_ url: URL) async throws -> String {
        guard let text = String(data: try await getData(url), encoding: .utf8) else {
            throw NmosDecodingError.shape("transport file is not UTF-8")
        }
        return text
    }

    private func patch(_ url: URL, body: Data) async throws {
        var request = URLRequest(url: url)
        request.httpMethod = "PATCH"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = body
        let (data, response) = try await session.data(for: request)
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        guard (200..<300).contains(status) else {
            throw HTTPError(status: status, text: NmosDecoding.errorText(data))
        }
    }

    /// Everything the matrix needs from one node. A node that fails at any
    /// step comes back unreachable with whatever was known before.
    private func readNode(name: String, host: String, port: Int) async -> NmosNode {
        let previous = nodes.first { $0.host == host && $0.port == port }
        let root = URL(string: "http://\(host):\(port)/x-nmos/node/v1.3/")!
        do {
            let identity = try NmosDecoding.nodeSelf(try await getData(root.appendingPathComponent("self")))
            var node = NmosNode(id: identity.id, label: identity.label, host: host, port: port)
            node.connectionRoot = try NmosDecoding.connectionRoot(devices: try await getData(root.appendingPathComponent("devices")))
            node.senders = try NmosDecoding.senders(
                try await getData(root.appendingPathComponent("senders")),
                flows: try await getData(root.appendingPathComponent("flows")),
                sources: try await getData(root.appendingPathComponent("sources")),
                nodeId: node.id)
            node.receivers = try NmosDecoding.receivers(try await getData(root.appendingPathComponent("receivers")), nodeId: node.id)
            if let connectionRoot = node.connectionRoot {
                for index in node.receivers.indices {
                    let active = try NmosDecoding.active(try await getData(
                        connectionRoot.appendingPathComponent("single/receivers/\(node.receivers[index].id)/active")))
                    node.receivers[index].activeSenderId = active.senderId
                    node.receivers[index].masterEnable = active.masterEnable
                }
            }
            return node
        } catch {
            if var known = previous {
                known.reachable = false
                return known
            }
            var unknown = NmosNode(id: "\(host):\(port)", label: name, host: host, port: port)
            unknown.reachable = false
            return unknown
        }
    }

    private func publish(_ read: [NmosNode]) {
        nodes = read
        matrix = RoutingMatrix.build(from: read)
        lastRead = Date()
    }
}

// MARK: - Bonjour

extension NmosController: NetServiceBrowserDelegate, NetServiceDelegate {
    nonisolated func netServiceBrowser(_ browser: NetServiceBrowser, didFind service: NetService, moreComing: Bool) {
        Task { @MainActor in
            service.delegate = self
            self.resolving.append(service)
            service.resolve(withTimeout: 5)
        }
    }

    nonisolated func netServiceBrowser(_ browser: NetServiceBrowser, didRemove service: NetService, moreComing: Bool) {
        Task { @MainActor in
            guard let gone = self.endpoints.removeValue(forKey: service.name) else { return }
            self.resolving.removeAll { $0 == service }
            self.publish(self.nodes.filter { !($0.host == gone.host && $0.port == gone.port) })
        }
    }

    nonisolated func netServiceDidResolveAddress(_ service: NetService) {
        Task { @MainActor in
            defer { self.resolving.removeAll { $0 == service } }
            guard let host = service.hostName, service.port > 0 else { return }
            // Bonjour hands back "name.local." with a trailing dot; URLSession
            // resolves it either way, the dot is only dropped for display.
            let trimmed = host.hasSuffix(".") ? String(host.dropLast()) : host
            self.endpoints[service.name] = (trimmed, service.port)
            self.refresh()
        }
    }

    nonisolated func netService(_ service: NetService, didNotResolve errorDict: [String: NSNumber]) {
        Task { @MainActor in
            self.resolving.removeAll { $0 == service }
        }
    }
}
```

- [ ] **Step 2: Build the app**

Add `Models/NmosController.swift` to `build.sh` after `Models/NmosResources.swift`. Run: `./build.sh --force`
Expected: `Build complete: AES67Manager.app`. If `swiftc` reports that `NetServiceBrowserDelegate` methods cannot be `nonisolated` on a `@MainActor` class in this toolchain, drop `@MainActor` from the class, keep every mutation inside `Task { @MainActor in ... }` as written, and mark the class `@unchecked Sendable`.

- [ ] **Step 3: Commit**

```bash
git add packages/aes67-macos-driver/ManagerApp/Models/NmosController.swift packages/aes67-macos-driver/ManagerApp/build.sh
git commit -m "feat(manager): NMOS controller, discovery and IS-05 connections"
```

---

### Task 8: `RoutingMatrixView` and the sidebar button

**Files:**
- Create: `packages/aes67-macos-driver/ManagerApp/Views/RoutingMatrixView.swift`
- Modify: `packages/aes67-macos-driver/ManagerApp/Views/ContentView.swift:15` (state), `:297-300` (button), `:345-348` (sheet)
- Modify: `packages/aes67-macos-driver/ManagerApp/Resources/Info.plist` (Bonjour keys)
- Modify: `packages/aes67-macos-driver/ManagerApp/build.sh` (source)

- [ ] **Step 1: The view**

```swift
//
// RoutingMatrixView.swift
// AES67 Manager
//
// Every NMOS node on the link as one matrix: senders across, receivers
// down, a click on a crosspoint connects or disconnects over IS-05. This
// Mac is one of the nodes. What the local Channel Mapping grid does --
// where each stream lands on this device's channels -- is a different
// thing and stays where it is.
//

import SwiftUI

struct RoutingMatrixView: View {
    @StateObject private var controller = NmosController()
    @Environment(\.dismiss) private var dismiss

    private let cellSize: CGFloat = 28
    private let rowHeaderWidth: CGFloat = 220
    private let columnHeaderHeight: CGFloat = 140

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            if controller.matrix.rows.isEmpty && controller.matrix.columns.isEmpty {
                empty
            } else {
                grid
            }
            Divider()
            footer
        }
        .frame(minWidth: 800, minHeight: 500)
        .onAppear { controller.start() }
        .onDisappear { controller.stop() }
        .alert("Connection refused", isPresented: Binding(
            get: { controller.lastError != nil },
            set: { if !$0 { controller.lastError = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(controller.lastError ?? "")
        }
    }

    private var header: some View {
        HStack {
            VStack(alignment: .leading, spacing: 4) {
                Text("Network Routing").font(.title2).fontWeight(.semibold)
                Text("Senders across, receivers down. Click a crosspoint to connect or disconnect over NMOS IS-05.")
                    .font(.caption).foregroundColor(.secondary)
            }
            Spacer()
            Button("Refresh") { controller.refresh() }
            Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
        }
        .padding()
    }

    private var empty: some View {
        VStack(spacing: 8) {
            Image(systemName: "point.3.connected.trianglepath.dotted").font(.largeTitle).foregroundColor(.secondary)
            Text("No NMOS nodes found yet").font(.headline)
            Text("Nodes announce themselves as _nmos-node._tcp over Bonjour. This Mac appears here once its driver is active.")
                .font(.caption).foregroundColor(.secondary).multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }

    private var grid: some View {
        ScrollView([.horizontal, .vertical]) {
            VStack(alignment: .leading, spacing: 0) {
                HStack(spacing: 0) {
                    Color.clear.frame(width: rowHeaderWidth, height: columnHeaderHeight)
                    ForEach(controller.matrix.columns) { column in
                        columnHeader(column)
                    }
                }
                ForEach(controller.matrix.rows) { row in
                    HStack(spacing: 0) {
                        rowHeader(row)
                        ForEach(controller.matrix.columns) { column in
                            cell(row: row, column: column)
                        }
                    }
                }
            }
            .padding()
        }
    }

    private func columnHeader(_ column: RoutingMatrix.Column) -> some View {
        VStack(spacing: 2) {
            Spacer()
            Text(column.nodeLabel).font(.caption2).foregroundColor(.secondary).lineLimit(1)
            Text(column.sender.channels.map { "\(column.label) (\($0))" } ?? column.label)
                .font(.caption).lineLimit(1)
            if !column.reachable {
                Text("unreachable").font(.caption2).foregroundColor(.red)
            }
        }
        .fixedSize()
        .rotationEffect(.degrees(-60), anchor: .bottomLeading)
        .frame(width: cellSize, height: columnHeaderHeight, alignment: .bottomLeading)
        .opacity(column.reachable ? 1 : 0.5)
    }

    private func rowHeader(_ row: RoutingMatrix.Row) -> some View {
        HStack(spacing: 6) {
            VStack(alignment: .leading, spacing: 1) {
                Text(row.label).font(.caption).lineLimit(1)
                Text(row.nodeLabel).font(.caption2).foregroundColor(.secondary).lineLimit(1)
            }
            Spacer()
            if !row.reachable {
                Text("unreachable").font(.caption2).foregroundColor(.red)
            } else if !row.writable {
                Text("read-only").font(.caption2).foregroundColor(.secondary)
            }
        }
        .padding(.horizontal, 6)
        .frame(width: rowHeaderWidth, height: cellSize, alignment: .leading)
        .opacity(row.reachable ? 1 : 0.5)
    }

    private func cell(row: RoutingMatrix.Row, column: RoutingMatrix.Column) -> some View {
        let state = controller.matrix.cell(row: row, column: column)
        let pending = controller.inFlight.contains(row.id)
        return Button {
            guard !pending else { return }
            switch state {
            case .off: controller.connect(receiver: row.receiver, to: column.sender)
            case .on: controller.disconnect(receiver: row.receiver)
            case .unavailable: break
            }
        } label: {
            ZStack {
                Rectangle().fill(Color(nsColor: .controlBackgroundColor))
                    .border(Color(nsColor: .separatorColor), width: 0.5)
                if pending {
                    ProgressView().controlSize(.mini)
                } else {
                    switch state {
                    case .on: Circle().fill(Color.accentColor).frame(width: 14, height: 14)
                    case .off: Circle().stroke(Color.secondary, lineWidth: 1).frame(width: 14, height: 14)
                    case .unavailable: Rectangle().fill(Color.gray.opacity(0.15))
                    }
                }
            }
        }
        .buttonStyle(.plain)
        .frame(width: cellSize, height: cellSize)
        .disabled(state == .unavailable || pending)
        .help(state == .on
              ? "Disconnect \(row.label) from \(column.label)"
              : "Connect \(row.label) to \(column.label)")
    }

    private var footer: some View {
        HStack {
            Text("\(controller.nodes.count) node\(controller.nodes.count == 1 ? "" : "s"), "
                 + "\(controller.matrix.columns.count) sender\(controller.matrix.columns.count == 1 ? "" : "s"), "
                 + "\(controller.matrix.rows.count) receiver\(controller.matrix.rows.count == 1 ? "" : "s")")
                .font(.caption).foregroundColor(.secondary)
            Spacer()
            if let error = controller.lastError {
                Text(error).font(.caption).foregroundColor(.red).lineLimit(1)
            }
            if let read = controller.lastRead {
                Text("Read \(read.formatted(date: .omitted, time: .standard))")
                    .font(.caption).foregroundColor(.secondary)
            }
        }
        .padding(8)
    }
}
```

- [ ] **Step 2: Wire it into `ContentView`**

Next to `@State private var showChannelMapping = false` add `@State private var showNetworkRouting = false`.

After the Channel Mapping button:

```swift
                Button(action: { showNetworkRouting = true }) {
                    Label("Network Routing", systemImage: "point.3.connected.trianglepath.dotted")
                }
                .help("Connect receivers to senders on every NMOS node on this network")
```

After the Channel Mapping sheet:

```swift
        .sheet(isPresented: $showNetworkRouting) {
            RoutingMatrixView()
        }
```

- [ ] **Step 3: Bonjour permission keys**

In `Resources/Info.plist`, before `</dict>`:

```xml
    <key>NSLocalNetworkUsageDescription</key>
    <string>AES67 Manager looks for NMOS audio devices on the local network so you can route streams between them.</string>
    <key>NSBonjourServices</key>
    <array>
        <string>_nmos-node._tcp</string>
    </array>
```

Add `Views/RoutingMatrixView.swift` to `build.sh` after `Views/AudioStatusView.swift`.

- [ ] **Step 4: Build, run the tests, open the app**

Run: `./run-tests.sh && ./build.sh --force && open AES67Manager.app`
Expected: tests pass, build completes, the sidebar shows "Network Routing" and the sheet opens with "No NMOS nodes found yet" (the driver is not installed on this Mac; that is Task 9).

- [ ] **Step 5: Commit**

```bash
git add packages/aes67-macos-driver/ManagerApp/Views/RoutingMatrixView.swift \
        packages/aes67-macos-driver/ManagerApp/Views/ContentView.swift \
        packages/aes67-macos-driver/ManagerApp/Resources/Info.plist packages/aes67-macos-driver/ManagerApp/build.sh
git commit -m "feat(manager): Network Routing matrix across every NMOS node"
```

---

### Task 9: End to end on the loopback

No code unless something is found. This is the check the spec asks for.

- [ ] **Step 1: A second node**

```bash
cd packages/aes67-ravenna && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/ravenna-announce --interface lo0 --address 127.0.0.1 --name "Loopback Mix" \
    --group 239.69.1.10 --channels 2 --ptp-gmid 00-1D-C1-FF-FE-00-00-01
```

Expected: it prints its RTSP and NMOS ports. Check the announcement with `dns-sd -B _nmos-node._tcp` in another shell. If `dns-sd` lists nothing on `lo0`, macOS's `mDNSResponder` is not bridging loopback; rerun the announcer with `--interface en0 --address <en0 IP>` (needs the terminal's Local Network permission, which on this Mac has failed before with `EHOSTUNREACH`; report it if so).

- [ ] **Step 2: The matrix**

Open `AES67Manager.app`, Network Routing. Expected: one node "Loopback Mix" (or the announcer's label) with one sender and one receiver. Click the crosspoint. Expected: the cell shows a spinner then a filled circle, the announcer prints `[ravenna] receiver receiver-1: 2 channels of "Loopback Mix" onto device channels ...`, and `curl` on its `.../single/receivers/receiver-1/active/` shows `master_enable: true`. Click again: empty circle, announcer prints `disabled, channels freed`.

- [ ] **Step 3: This Mac as a node**

Only if the driver can be installed on this Mac (the Manager's install switch, administrator password). Then: `dns-sd -B _nmos-node._tcp` lists the driver's node; `curl http://<en0 IP>:<port>/x-nmos/node/v1.3/self` answers; the matrix shows both nodes. Add a transmit stream in the Manager and a receive stream, and route the driver's receiver onto the announcer's sender. If the driver cannot be installed, say so in the report and leave this step listed as not run.

- [ ] **Step 4: Record what was seen**

Append to `packages/aes67-ravenna/README.md`'s "Trying it" section one paragraph saying the Manager's Network Routing sheet is the controller for this, and to `packages/aes67-macos-driver/README.md` a line under the Manager app's feature list: "Network Routing: an NMOS IS-04/IS-05 controller across every node on the link, this Mac included." Commit:

```bash
git commit -am "docs: network routing from the Manager"
```

---

### Task 10: Gate and merge

- [ ] **Step 1: The monorepo gate**

Run: `scripts/gate.sh` from the monorepo root.
Expected: `######## every package passed`. The driver's gate builds the Manager app and runs `ManagerAppUnit`; the ravenna gate still builds its tool and tests (the option defaults to ON).

- [ ] **Step 2: Merge**

Follow the `git-sync-and-merge` skill: `git merge --no-ff feat/network-routing` into `main`, push, delete the working branch.

---

## Self-review

- Spec section 1 (Node API, mDNS, start condition): Tasks 1-5. Section 2 (discovery, IS-04 reads, `active` as truth, connect/disconnect, refresh 5 s, errors, pure parts tested): Tasks 6-7. Section 3 (button, grid, cell states, unreachable, footer): Task 8. Section 4 (driver suite, Manager suite, Info.plist keys, loopback end to end, order, gate): Tasks 2, 4, 6, 8, 9, 10.
- Deviation stated: receivers show no channel count, IS-04 carries none; senders show the source's channel count.
- Names used across tasks: `NodeAPIRouter` (`route`, `touch`, `setEndpoint`, `deviceId`), `NodeAdvertiser` (`start`, `stop`), `nodeAdvertisement`, `ConnectionAPIServer::setFallbackRouter`, `NMOSRegistrationClient::build*Data` / `wrapResource`, `NmosDecoding`, `NmosPatch`, `RoutingMatrix` (`build`, `cell`, `node(forSender:)`), `NmosController` (`start`, `stop`, `refresh`, `connect`, `disconnect`, `nodes`, `matrix`, `inFlight`, `lastRead`, `lastError`).
