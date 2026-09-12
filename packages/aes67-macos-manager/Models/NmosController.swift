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
import Network

@MainActor
final class NmosController: NSObject, ObservableObject {
    static let serviceType = "_nmos-node._tcp."
    static let refreshInterval: TimeInterval = 5

    @Published private(set) var nodes: [NmosNode] = []
    @Published private(set) var matrix = RoutingMatrix.build(from: [])
    @Published private(set) var inFlight: Set<String> = []
    @Published private(set) var lastRead: Date? = nil
    /// Every IS-04 sender with the transport file IS-05 serves for it, so the
    /// sessions this app discovers over NMOS can join the one list the driver
    /// publishes for what it hears itself (SessionList.merge). Read on every
    /// refresh: a transport file is small, and a sender whose destination
    /// changed is exactly what a list like this exists to show.
    @Published private(set) var sessionCandidates: [NmosSessionCandidate] = []
    @Published var lastError: String? = nil

    /// NWBrowser, not NetServiceBrowser: the latter is deprecated as of
    /// macOS 15, and it is the API whose interaction with Local Network
    /// privacy is the one people trip over. Network framework resolves the
    /// endpoint for us -- there is no separate resolve step and no strong
    /// reference to keep alive while it happens.
    private var browser: NWBrowser?
    /// Connections opened only to learn a host and port, closed as soon as
    /// they are ready. NWBrowser hands out a service name, not an address.
    private var resolving: [String: NWConnection] = [:]
    /// Resolved endpoints by service name, kept until Bonjour withdraws them.
    private var endpoints: [String: (host: String, port: Int)] = [:]
    private var refreshTimer: Timer?
    private var started = false
    /// One pass at a time: a pass is serial over every node, so it can outlast
    /// the timer's interval. A refresh asked for while one is running is
    /// coalesced into a single re-run, so an older snapshot can never finish
    /// last and overwrite a newer one.
    private var refreshing = false
    private var refreshAgain = false
    private let session: URLSession = {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 3
        return URLSession(configuration: configuration)
    }()

    override init() {
        super.init()
    }

    func start() {
        guard !started else { return }
        started = true
        startBrowsing()
        refreshTimer = Timer.scheduledTimer(withTimeInterval: Self.refreshInterval, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
    }

    func stop() {
        guard started else { return }
        started = false
        browser?.cancel()
        browser = nil
        for connection in resolving.values { connection.cancel() }
        resolving.removeAll()
        refreshTimer?.invalidate()
        refreshTimer = nil
    }

    func refresh() {
        guard !refreshing else {
            refreshAgain = true
            return
        }
        refreshing = true
        let targets = endpoints
        // What is known now, read on the actor that owns it and handed to the
        // reads, which run off it.
        let known = nodes
        Task { [weak self] in
            guard let self else { return }
            // Every node at once. A plant is a dozen devices and each one is
            // half a dozen requests deep; asking them one after another made
            // a refresh take as long as the sum of the whole room, and the
            // slowest box in it set the pace for all of them.
            var read: [NmosNode] = await withTaskGroup(of: NmosNode.self) { group in
                for (name, endpoint) in targets {
                    let previous = known.first { $0.host == endpoint.host && $0.port == endpoint.port }
                    group.addTask {
                        await self.readNode(name: name, host: endpoint.host, port: endpoint.port,
                                            previous: previous)
                    }
                }
                var nodes: [NmosNode] = []
                for await node in group { nodes.append(node) }
                return nodes
            }
            // Discovery order is whatever answers first, which is not an
            // order to show anything in.
            read.sort { $0.label.localizedStandardCompare($1.label) == .orderedAscending }
            let candidates = await self.readSessionCandidates(from: read)
            self.publish(read)
            self.sessionCandidates = candidates
            self.refreshing = false
            if self.refreshAgain {
                self.refreshAgain = false
                self.refresh()
            }
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

    /// The IS-08 map of one node: what feeds every output channel, and the
    /// ports both ends of the grid are drawn from. Empty for a node that maps
    /// no channels, which is most gear -- IS-08 is optional, and a device that
    /// routes whole streams and nothing finer declares no cm-ctrl control.
    func channelMap(of node: NmosNode) async -> NmosChannelMap {
        guard let root = node.channelMappingRoot else { return .empty }
        do {
            let io = try NmosDecoding.channelMapIO(try await getData(root.appendingPathComponent("io")))
            let active = try NmosDecoding.channelMapActive(
                try await getData(root.appendingPathComponent("map/active")))
            return NmosChannelMap(inputs: io.inputs, outputs: io.outputs, active: active)
        } catch {
            lastError = error.localizedDescription
            return .empty
        }
    }

    /// Sets one crosspoint on a node's channel map, or mutes it when `input`
    /// is nil. Immediate, like every other write here: a staged activation
    /// nobody triggers is a routing change that silently did not happen.
    func setCrosspoint(on node: NmosNode, output: String, outputChannel: Int,
                       input: String?, inputChannel: Int?) async {
        guard let root = node.channelMappingRoot else {
            lastError = "\(node.label) maps no channels: it has no IS-08 control."
            return
        }
        do {
            // POST map/activate, not PATCH: IS-08 writes the map through an
            // activation resource rather than by patching the map itself, and
            // this repository's own server (Ravenna/ChannelMappingApi.cpp)
            // answers 405 to anything else there.
            try await post(root.appendingPathComponent("map/activate"),
                           body: NmosPatch.mapChannel(output: output, outputChannel: outputChannel,
                                                      input: input, inputChannel: inputChannel))
        } catch {
            lastError = error.localizedDescription
        }
    }

    /// Re-addresses a sender: where it transmits, patched through IS-05.
    ///
    /// This is the only way to route into gear that has no control protocol
    /// of its own -- a Dolby Atmos Connect unit listens where its manual says
    /// and answers nothing, so the end a controller can configure is the
    /// source. The driver applies it by re-creating the transmit stream at
    /// that destination, which is also what assigns the per-flow source ports
    /// that scheme identifies flows by.
    /// Stops `previous` and only then points `sender` at the destination.
    ///
    /// Two PATCHes from two detached tasks race, and the one that loses can be
    /// the stop -- which leaves both senders on one address, the collision the
    /// ordering exists to avoid.
    func move(from previous: NmosSender?, to sender: NmosSender,
              destination multicastAddress: String, port: Int) {
        Task { [weak self] in
            guard let self else { return }
            if let previous {
                await self.stopSending(previous)
            }
            await self.sendTo(sender, multicastAddress: multicastAddress, port: port)
            self.refresh()
        }
    }

    func send(sender: NmosSender, to multicastAddress: String, port: Int) {
        guard let node = nodes.first(where: { $0.id == sender.nodeId }),
              let root = node.connectionRoot else {
            lastError = "That sender is on a node without a connection API."
            return
        }
        inFlight.insert(sender.id)
        Task { [weak self] in
            guard let self else { return }
            defer { self.inFlight.remove(sender.id) }
            do {
                try await self.patch(root.appendingPathComponent("single/senders/\(sender.id)/staged"),
                                     body: NmosPatch.sendTo(multicastAddress: multicastAddress,
                                                            port: port))
            } catch {
                self.lastError = error.localizedDescription
            }
            self.refresh()
        }
    }

    /// Stops a sender: master_enable false, applied immediately. What undoes
    /// a feed into gear that cannot be told anything.
    func stop(sender: NmosSender) {
        guard let node = nodes.first(where: { $0.id == sender.nodeId }),
              let root = node.connectionRoot else {
            lastError = "That sender is on a node without a connection API."
            return
        }
        inFlight.insert(sender.id)
        Task { [weak self] in
            guard let self else { return }
            defer { self.inFlight.remove(sender.id) }
            do {
                try await self.patch(root.appendingPathComponent("single/senders/\(sender.id)/staged"),
                                     body: NmosPatch.stopSending())
            } catch {
                self.lastError = error.localizedDescription
            }
            self.refresh()
        }
    }

    /// Points a receiver at an address nobody advertises: gear that announces
    /// nothing still transmits somewhere, and this is the end that can be told
    /// to listen there. The description is built from what that convention
    /// says, since the device itself serves none.
    func listen(receiver: NmosReceiver, at multicastAddress: String, port: Int, label: String) {
        guard let node = nodes.first(where: { $0.id == receiver.nodeId }),
              let root = node.connectionRoot else {
            lastError = "That receiver is on a node without a connection API."
            return
        }
        inFlight.insert(receiver.id)
        Task { [weak self] in
            guard let self else { return }
            defer { self.inFlight.remove(receiver.id) }
            do {
                try await self.patch(
                    root.appendingPathComponent("single/receivers/\(receiver.id)/staged"),
                    body: NmosPatch.listenAt(multicastAddress: multicastAddress, port: port,
                                             label: label))
            } catch {
                self.lastError = error.localizedDescription
            }
            self.refresh()
        }
    }

    private func stopSending(_ sender: NmosSender) async {
        guard let node = nodes.first(where: { $0.id == sender.nodeId }),
              let root = node.connectionRoot else { return }
        inFlight.insert(sender.id)
        defer { inFlight.remove(sender.id) }
        do {
            try await patch(root.appendingPathComponent("single/senders/\(sender.id)/staged"),
                            body: NmosPatch.stopSending())
        } catch {
            lastError = error.localizedDescription
        }
    }

    private func sendTo(_ sender: NmosSender, multicastAddress: String, port: Int) async {
        guard let node = nodes.first(where: { $0.id == sender.nodeId }),
              let root = node.connectionRoot else {
            lastError = "That sender is on a node without a connection API."
            return
        }
        inFlight.insert(sender.id)
        defer { inFlight.remove(sender.id) }
        do {
            try await patch(root.appendingPathComponent("single/senders/\(sender.id)/staged"),
                            body: NmosPatch.sendTo(multicastAddress: multicastAddress, port: port))
        } catch {
            lastError = error.localizedDescription
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

    // nonisolated: these touch nothing but `session`, which is a let, and
    // being on the main actor gained nothing while costing the ability to run
    // more than one of them at a time.
    nonisolated private func getData(_ url: URL) async throws -> Data {
        let (data, response) = try await session.data(from: url)
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        guard (200..<300).contains(status) else {
            throw HTTPError(status: status, text: NmosDecoding.errorText(data))
        }
        return data
    }

    nonisolated private func getText(_ url: URL) async throws -> String {
        guard let text = String(data: try await getData(url), encoding: .utf8) else {
            throw NmosDecodingError.shape("transport file is not UTF-8")
        }
        return text
    }

    private func post(_ url: URL, body: Data) async throws {
        try await send(url, method: "POST", body: body)
    }

    private func patch(_ url: URL, body: Data) async throws {
        try await send(url, method: "PATCH", body: body)
    }

    private func send(_ url: URL, method: String, body: Data) async throws {
        var request = URLRequest(url: url)
        request.httpMethod = method
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
    /// `previous` is what was known of this node before, passed in rather
    /// than read here: this runs off the main actor so that a room's nodes
    /// are read at once, and the published list belongs to that actor.
    nonisolated private func readNode(name: String, host: String, port: Int,
                                      previous: NmosNode?) async -> NmosNode {
        func unreachable() -> NmosNode {
            if var known = previous {
                known.reachable = false
                return known
            }
            var unknown = NmosNode(id: "\(host):\(port)", label: name, host: host, port: port)
            unknown.reachable = false
            return unknown
        }
        guard let root = URL(string: "http://\(host):\(port)/x-nmos/node/v1.3/") else { return unreachable() }
        do {
            // The six resources a node describes itself with, fetched
            // together: they do not depend on each other.
            async let selfBytes = getData(root.appendingPathComponent("self"))
            async let devicesBytes = getData(root.appendingPathComponent("devices"))
            async let sendersBytes = getData(root.appendingPathComponent("senders"))
            async let flowsBytes = getData(root.appendingPathComponent("flows"))
            async let sourcesBytes = getData(root.appendingPathComponent("sources"))
            async let receiversBytes = getData(root.appendingPathComponent("receivers"))

            let selfData = try await selfBytes
            let identity = try NmosDecoding.nodeSelf(selfData)
            var node = NmosNode(id: identity.id, label: identity.label, host: host, port: port)
            // Which clock this node follows, and whether it is locked to it:
            // the column a routing screen is read for as much as the grid.
            node.clocks = (try? NmosDecoding.clocks(nodeSelf: selfData)) ?? []
            let devicesData = try await devicesBytes
            node.connectionRoot = try NmosDecoding.connectionRoot(devices: devicesData)
            node.channelMappingRoot = try? NmosDecoding.channelMappingRoot(devices: devicesData)
            node.senders = try NmosDecoding.senders(
                try await sendersBytes,
                flows: try await flowsBytes,
                sources: try await sourcesBytes,
                nodeId: node.id)
            node.receivers = try NmosDecoding.receivers(try await receiversBytes, nodeId: node.id)
            if let connectionRoot = node.connectionRoot {
                // Where each sender is transmitting. A fixed sink -- gear
                // that cannot be told anything -- is shown as taken by
                // whichever sender is already addressed at it, and that is
                // the only way to know: the device itself reports nothing.
                // One request per sender and per receiver, all in flight at
                // once rather than one behind the other: a 32-sender device
                // was 64 sequential round trips of its own.
                let senderIds = node.senders.map(\.id)
                let senderActives: [String: (destination: String, port: Int, masterEnable: Bool)] =
                    await withTaskGroup(
                        of: (String, (destination: String, port: Int, masterEnable: Bool)?).self
                    ) { group in
                        for id in senderIds {
                            group.addTask { [self] in
                                let url = connectionRoot.appendingPathComponent(
                                    "single/senders/\(id)/active")
                                guard let data = try? await getData(url),
                                      let active = try? NmosDecoding.senderActive(data) else {
                                    return (id, nil)
                                }
                                return (id, active)
                            }
                        }
                        var found: [String: (destination: String, port: Int, masterEnable: Bool)] = [:]
                        for await (id, active) in group {
                            if let active { found[id] = active }
                        }
                        return found
                    }
                for index in node.senders.indices {
                    guard let active = senderActives[node.senders[index].id] else { continue }
                    node.senders[index].destination = active.destination
                    node.senders[index].destinationPort = active.port
                    node.senders[index].enabled = active.masterEnable
                }

                let receiverIds = node.receivers.map(\.id)
                let receiverActives: [String: (senderId: String?, masterEnable: Bool)] =
                    await withTaskGroup(of: (String, (senderId: String?, masterEnable: Bool)?).self) { group in
                        for id in receiverIds {
                            group.addTask { [self] in
                                let url = connectionRoot.appendingPathComponent(
                                    "single/receivers/\(id)/active")
                                guard let data = try? await getData(url),
                                      let active = try? NmosDecoding.active(data) else {
                                    return (id, nil)
                                }
                                return (id, active)
                            }
                        }
                        var found: [String: (senderId: String?, masterEnable: Bool)] = [:]
                        for await (id, active) in group {
                            if let active { found[id] = active }
                        }
                        return found
                    }
                for index in node.receivers.indices {
                    guard let active = receiverActives[node.receivers[index].id] else { continue }
                    node.receivers[index].activeSenderId = active.senderId
                    node.receivers[index].masterEnable = active.masterEnable
                }
            }
            return node
        } catch {
            return unreachable()
        }
    }

    /// The transport file of every sender on every node that has an IS-05
    /// root. A sender whose file cannot be read is still offered, with an
    /// empty description: the app lists it and refuses to subscribe rather
    /// than hiding a node that is plainly there.
    nonisolated private func readSessionCandidates(from read: [NmosNode]) async
        -> [NmosSessionCandidate] {
        struct Request { let senderId: String; let label: String; let host: String; let url: URL }
        var requests: [Request] = []
        for node in read {
            guard let root = node.connectionRoot else { continue }
            for sender in node.senders {
                requests.append(Request(
                    senderId: sender.id, label: sender.label, host: node.host,
                    url: root.appendingPathComponent("single/senders/\(sender.id)/transportfile")))
            }
        }

        // Every transport file at once. One per sender on every node in the
        // plant, and they were fetched one after another while the window
        // waited.
        var candidates: [NmosSessionCandidate] = await withTaskGroup(
            of: NmosSessionCandidate.self
        ) { group in
            for request in requests {
                group.addTask { [self] in
                    let sdp = (try? await getText(request.url)) ?? ""
                    return NmosSessionCandidate(senderId: request.senderId,
                                                label: request.label,
                                                host: request.host,
                                                sdp: sdp)
                }
            }
            var read: [NmosSessionCandidate] = []
            for await candidate in group { read.append(candidate) }
            return read
        }
        candidates.sort { $0.label.localizedStandardCompare($1.label) == .orderedAscending }
        return candidates
    }

    private func publish(_ read: [NmosNode]) {
        nodes = read
        matrix = RoutingMatrix.build(from: read)
        lastRead = Date()
    }
}

// MARK: - Bonjour

private extension NmosController {
    /// Browse for the service type, and resolve what turns up.
    ///
    /// NWBrowser reports endpoints as `.service(name:type:domain:interface:)`,
    /// which is a name and not an address. The way Network framework turns one
    /// into a host and port is to open a connection to it and read
    /// `currentPath` once it is ready -- so that is what this does, and closes
    /// it again. The alternative is resolving by hand over DNS-SD, which is
    /// the layer this moved off.
    func startBrowsing() {
        // Without the trailing dot. NetServiceBrowser took "_nmos-node._tcp."
        // and NWBrowser does not: Descriptor.bonjour(type:domain:) wants the
        // bare type, with the domain given separately, and a type ending in a
        // dot matches nothing -- silently, which is how a browser that finds
        // no nodes looks exactly like a network with none.
        let type = Self.serviceType.hasSuffix(".")
            ? String(Self.serviceType.dropLast())
            : Self.serviceType
        let descriptor = NWBrowser.Descriptor.bonjour(type: type, domain: "local.")
        let parameters = NWParameters()
        parameters.includePeerToPeer = false
        let browser = NWBrowser(for: descriptor, using: parameters)
        self.browser = browser

        browser.browseResultsChangedHandler = { [weak self] results, changes in
            Task { @MainActor in
                guard let self else { return }
                for change in changes {
                    switch change {
                    case .added(let result):
                        self.resolve(result)
                    case .removed(let result):
                        self.forget(result)
                    default:
                        break
                    }
                }
                // A pass that ends with no results at all still has to publish
                // the empty list, or a node that went away stays on screen.
                if results.isEmpty && self.endpoints.isEmpty {
                    self.publish([])
                }
            }
        }
        browser.stateUpdateHandler = { [weak self] state in
            Task { @MainActor in
                guard case .failed(let error) = state else { return }
                self?.lastError = "Bonjour browsing stopped: \(error.localizedDescription)"
            }
        }
        browser.start(queue: .main)
    }

    func serviceName(of result: NWBrowser.Result) -> String? {
        guard case .service(let name, _, _, _) = result.endpoint else { return nil }
        return name
    }

    func resolve(_ result: NWBrowser.Result) {
        guard let name = serviceName(of: result), resolving[name] == nil else { return }

        let connection = NWConnection(to: result.endpoint, using: .tcp)
        resolving[name] = connection
        connection.stateUpdateHandler = { [weak self] state in
            Task { @MainActor in
                guard let self else { return }
                switch state {
                case .ready:
                    if let endpoint = connection.currentPath?.remoteEndpoint,
                       case .hostPort(let host, let port) = endpoint {
                        // "name.local." keeps its trailing dot in some forms;
                        // URLSession resolves it either way, the dot is only
                        // dropped for display.
                        var text = "\(host)"
                        if let percent = text.firstIndex(of: "%") { text = String(text[..<percent]) }
                        if text.hasSuffix(".") { text = String(text.dropLast()) }
                        self.endpoints[name] = (text, Int(port.rawValue))
                        self.refresh()
                    }
                    connection.cancel()
                    self.resolving[name] = nil
                case .failed, .cancelled:
                    self.resolving[name] = nil
                default:
                    break
                }
            }
        }
        connection.start(queue: .main)
    }

    func forget(_ result: NWBrowser.Result) {
        guard let name = serviceName(of: result),
              let gone = endpoints.removeValue(forKey: name) else { return }
        resolving[name]?.cancel()
        resolving[name] = nil
        publish(nodes.filter { !($0.host == gone.host && $0.port == gone.port) })
    }
}
