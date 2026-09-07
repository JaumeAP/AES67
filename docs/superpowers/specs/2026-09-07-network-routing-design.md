# Network routing from the Manager: an NMOS controller, and the Mac as a node

Date: 2026-09-07

## What this is

Today the Manager's Channel Mapping grid places streams onto this Mac's own 128 device
channels, and nothing in the repository tells another device which stream to take. The
only path for that is NMOS IS-05, which `packages/aes67-ravenna` serves and the driver
serves too, and which no code here calls.

This design makes the Manager an NMOS controller: it finds every node on the link,
shows a senders-by-receivers matrix across all of them, and a click on a crosspoint
connects a receiver to a sender through IS-05. This Mac appears in that matrix as one
more node, so it can be routed like any card.

Only devices that speak NMOS IS-04 (peer-to-peer) and IS-05 are reachable: RAVENNA gear
and this repository's `ravenna-announce`. Dante exposes neither and stays out of scope.

## Decisions taken

1. This Mac is a node in the matrix, not only a controller of others.
2. The matrix is senders by receivers, one whole stream per crosspoint. Not channel by
   channel (IS-08), not a per-node list.
3. The controller is Swift inside the Manager (`NetServiceBrowser`, `URLSession`). No new
   C++/Swift bridge: `ManagerApp/build.sh` is plain `swiftc` and compiles no C++ today.
4. Peer-to-peer discovery over mDNS. No registry is required; registering with one stays
   what `NMOSSettings.enabled` already means and is untouched.
5. The Mac's node (Node API, IS-05, mDNS advertisement) is on whenever the device is
   active. No switch. It exposes an HTTP control on the segment, exactly as a RAVENNA
   card does.
6. Nothing is persisted on the Mac for network routing. The state lives in each device's
   IS-05 `active` endpoint and the matrix re-reads it.

## Section 1: the Mac as a node (driver)

What exists: `NetworkEngine/Discovery/NMOSRegistrationClient` builds IS-04 resources
(node, device, source, flow, sender, receiver) with UUIDs derived from stable inputs and
POSTs them to a registry; `NetworkEngine/Discovery/ConnectionAPIServer` serves IS-05
v1.1 on an ephemeral port inside `coreaudiod`, applies receiver PATCHes through
`AES67Device::applyConnectionPatch`, and refuses sender PATCHes with 501. Both start only
inside the `nmosSettings.enabled` block of `AES67Device`.

Changes:

1. **Node API.** `ConnectionAPIServer`'s HTTP server also routes
   `/x-nmos/node/v1.3/{self,devices,sources,flows,senders,receivers}` (collections and
   `/{id}`). The resources come from the same JSON builders `NMOSRegistrationClient`
   already has (today private: node, device, source, flow, sender, receiver JSON), made
   callable without a registry, and from the same derived ids, so a controller that reads
   the Node API and patches the Connection API is talking about the same things. The
   device resource lists the control `urn:x-nmos:control:sr-ctrl/v1.1` with an `href` on
   the same bound port. Unknown paths answer 404, unsupported methods 405, as the
   Connection API already does.
2. **mDNS advertisement.** The driver has a browser (`MDNSBrowser`) and no responder;
   `packages/aes67-ravenna` has `MdnsResponder`, with a hook meant for the NMOS node.
   The driver links `aes67_ravenna` (a new cross-package dependency, allowed by the
   monorepo build) and advertises `_nmos-node._tcp`: SRV to the bound port, TXT
   `api_ver=v1.3 api_proto=http`, A with the PTP interface's address. The advertisement
   goes out when the device becomes active and is withdrawn (zero TTL) when it stops.
3. **Start condition.** The Node API, the Connection API and the advertisement start
   whenever the device is active, outside the `nmosSettings.enabled` block. Registration
   with a registry keeps that block and its meaning.

## Section 2: the controller (Manager model)

1. **Discovery.** `Models/NmosController.swift`, an `ObservableObject`. A
   `NetServiceBrowser` on `_nmos-node._tcp` resolves each service to host, port and TXT
   (`NWBrowser` would need a connection to learn them). Each resolved service is a node;
   one the browser withdraws leaves the list.
2. **Reading IS-04.** Per node, `URLSession` GETs `/x-nmos/node/v1.3/self`, `/devices`,
   `/senders`, `/receivers`. The IS-05 root is the `href` of the device control
   `urn:x-nmos:control:sr-ctrl/v1.1`; a node whose device lists no such control is shown
   read-only.
3. **Crosspoint state.** Per receiver, GET IS-05 `single/receivers/{id}/active`:
   `sender_id` and `master_enable` are the truth. The IS-04 receiver's `subscription` is
   not used.
4. **Connect and disconnect.** Connect: GET `single/senders/{id}/transportfile` from the
   sender's node, then PATCH `single/receivers/{id}/staged` on the receiver's node with
   `sender_id`, `master_enable: true`, `transport_file` (`data`, `type:
   application/sdp`) and `activation: {mode: activate_immediate}`. Disconnect: PATCH
   `master_enable: false`, `sender_id: null`, same activation. After either, `active` is
   re-read and the matrix shows what happened, not what was asked.
5. **Refresh.** While the matrix window is open, every node is re-read every 5 seconds
   (the pattern `DiscoveredSessionsView` uses). A node that fails to answer is marked
   unreachable in its row and column, and is not removed until the browser withdraws it.
6. **Errors.** A non-2xx reply to a PATCH raises an alert with the `error` and `debug`
   fields of the IS-05 error body. No automatic retry.
7. **Testable without a network.** IS-04 and IS-05 JSON decoding, PATCH body building,
   and matrix construction (rows, columns, each cell's state, unreachable marking) live in
   `Models/NmosResources.swift` as pure values, covered by `ManagerApp/run-tests.sh`.
   `NmosController` only orchestrates.

## Section 3: the interface

1. **Entry.** A "Network Routing" button in `ContentView`'s sidebar next to "Channel
   Mapping", opening `Views/RoutingMatrixView.swift` as a sheet, the same way the channel
   grid opens.
2. **Grid.** Columns are senders, rows are receivers, grouped by node with the node's
   `label` as the group header and each sender's or receiver's `label` on its axis, with
   the channel count in parentheses. This Mac is one node among the others. Order: nodes
   by name, then labels within a node.
3. **Cell.** Empty circle: not connected. Filled circle: `active` names this sender with
   `master_enable` true. Spinner: a PATCH is in flight. Click on empty connects; click on
   filled disconnects. No confirmation dialog: either is undone by another click. A
   receiver holds one sender: connecting another replaces it, as IS-05 does.
4. **Unreachable nodes.** Row and column greyed with the text "unreachable"; their cells
   take no clicks until the node answers again.
5. **Footer.** Node, sender and receiver counts, the time of the last read, and the last
   PATCH error if any (in addition to the alert).
6. **Out of scope.** No presets, no export of the matrix. The local Channel Mapping grid
   is unchanged: it says which device channels of this Mac each stream lands on, which is
   a different thing from which sender each receiver takes.

## Section 4: verification and order of work

1. **Driver.** New doctest suite `Tests/TestNodeAPIServer.cpp` beside
   `TestConnectionAPI.cpp`: `route()` with fake senders and receivers, JSON checked byte
   for byte (self, devices with the control, senders, receivers, 404, 405). The mDNS
   records are already tested in `aes67-ravenna`; the driver only links and starts the
   responder. Live check: `dns-sd -B _nmos-node._tcp` and `curl` on the Node API.
2. **Manager.** `run-tests.sh` gains `Tests/NmosResourcesTests.swift`: IS-04 and IS-05
   fixtures decoded, PATCH body, matrix construction, unreachable marking. `Info.plist`
   gains `NSLocalNetworkUsageDescription` and `NSBonjourServices` listing
   `_nmos-node._tcp`, which macOS requires for Bonjour browsing from an app.
3. **End to end without a card.** `ravenna-announce` on the loopback is the second node
   (it already serves IS-04 and IS-05). The matrix must show two nodes, connect a
   receiver of the Mac to the announcer's sender, and read it back in `active`. Loopback
   works on this Mac; `en0` depends on the Local Network permission.
4. **Order.** Driver Node API with tests; link `aes67_ravenna` and advertise over mDNS;
   `NmosResources` with tests; `NmosController`; `RoutingMatrixView` and the sidebar
   button; end-to-end on loopback; monorepo gate (the `pre-push` hook runs it).
5. **Known risk.** Sending mDNS multicast from inside `coreaudiod`: `SAPAnnouncer`
   already sends multicast from that process, so the path exists.
