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
        Task { [weak self] in
            guard let self else { return }
            var read: [NmosNode] = []
            for (name, endpoint) in targets {
                read.append(await self.readNode(name: name, host: endpoint.host, port: endpoint.port))
            }
            self.publish(read)
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
            return unreachable()
        }
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
