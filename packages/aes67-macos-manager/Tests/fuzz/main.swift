//
// main.swift (Tests/fuzz)
// AES67 Manager
//
// The IS-04 parsing, fed what nobody wrote a test for.
//
// NmosResources reads the JSON a node answers with, and a node is any machine
// on the segment that answers an HTTP GET -- not necessarily this Mac's
// driver. What it hands back drives the routing matrix and, from there, the
// PATCH bodies this app sends; a parser that trusts its input here is one
// trusting a stranger.
//
// Deterministic: fixed seed, printed, so a failure is one command away from
// being reproduced. This toolchain's swiftc has no libFuzzer entry point, so
// there is one driver rather than two.
//
import Foundation

// A small xorshift rather than SystemRandomNumberGenerator: the point is that
// two runs, on two machines, feed the parser the same bytes.
struct SeededGenerator: RandomNumberGenerator {
    private var state: UInt64
    init(seed: UInt64) { state = seed == 0 ? 0x9E3779B97F4A7C15 : seed }
    mutating func next() -> UInt64 {
        state ^= state << 13
        state ^= state >> 7
        state ^= state << 17
        return state
    }
}

let seeds: [String] = [
    #"[{"id":"a1","label":"Sender 1","flow_id":"f1","device_id":"d1","transport":"urn:x-nmos:transport:rtp.mcast"}]"#,
    #"[{"id":"r1","label":"Receiver 1","device_id":"d1","subscription":{"active":true,"sender_id":"a1"}}]"#,
    #"[{"id":"f1","source_id":"s1","format":"urn:x-nmos:format:audio"}]"#,
    #"[{"id":"s1","channels":[{"label":"L"},{"label":"R"}]}]"#,
    #"[{"id":"d1","controls":[{"type":"urn:x-nmos:control:sr-ctrl/v1.1","href":"http://192.168.0.10:8080/"}]}]"#,
    #"{"id":"node1","label":"A node"}"#,
    #"{"master_enable":true,"sender_id":"a1"}"#,
    #"{"error":400,"debug":"bad"}"#,
    "[]",
    "{}",
    "null",
]

func fuzzOne(_ data: Data, _ nodeId: String) {
    // Every entry point NmosResources offers, on the same bytes: what one
    // refuses another may take, and a pair that disagrees is the bug.
    _ = try? NmosDecoding.nodeSelf(data)
    _ = try? NmosDecoding.connectionRoot(devices: data)
    _ = try? NmosDecoding.senders(data, flows: data, sources: data, nodeId: nodeId)
    _ = try? NmosDecoding.receivers(data, nodeId: nodeId)
    _ = try? NmosDecoding.active(data)
    _ = NmosDecoding.errorText(data)
}

let iterations = CommandLine.arguments.count > 1 ? Int(CommandLine.arguments[1]) ?? 200_000 : 200_000
let seed = CommandLine.arguments.count > 2 ? UInt64(CommandLine.arguments[2]) ?? 20_260_910 : 20_260_910
print("fuzz: \(iterations) iterations, seed \(seed)")

var rng = SeededGenerator(seed: seed)
for i in 0..<iterations {
    var bytes: [UInt8]
    if i % 2 == 0 {
        // A real document, edited: this is what reaches the code that runs
        // after the JSON has parsed.
        bytes = Array(seeds[Int(rng.next() % UInt64(seeds.count))].utf8)
        let edits = 1 + Int(rng.next() % 8)
        for _ in 0..<edits where !bytes.isEmpty {
            bytes[Int(rng.next() % UInt64(bytes.count))] = UInt8(rng.next() % 256)
        }
        if rng.next() % 4 == 0 {
            bytes = Array(bytes.prefix(Int(rng.next() % UInt64(bytes.count + 1))))
        }
    } else {
        // Bytes from nowhere: this is what reaches the JSON parser itself.
        bytes = (0..<Int(rng.next() % 256)).map { _ in UInt8(rng.next() % 256) }
    }
    fuzzOne(Data(bytes), "node-\(i % 3)")
}

print("fuzz: no crash, no trap")
