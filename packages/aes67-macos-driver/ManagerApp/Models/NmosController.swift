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
