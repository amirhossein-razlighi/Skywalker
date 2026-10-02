import Foundation

/// A Sendable, Codable JSON value. Used for tool arguments/results and for talking to
/// LLM providers, so data can cross actor boundaries safely under Swift 6 concurrency.
enum JSON: Sendable, Equatable, Hashable {
    case null
    case bool(Bool)
    case number(Double)
    case string(String)
    case array([JSON])
    case object([(String, JSON)])

    // Ordered object equality/hash by content.
    static func == (lhs: JSON, rhs: JSON) -> Bool {
        switch (lhs, rhs) {
        case (.null, .null): return true
        case let (.bool(a), .bool(b)): return a == b
        case let (.number(a), .number(b)): return a == b
        case let (.string(a), .string(b)): return a == b
        case let (.array(a), .array(b)): return a == b
        case let (.object(a), .object(b)):
            guard a.count == b.count else { return false }
            return zip(a, b).allSatisfy { $0.0 == $1.0 && $0.1 == $1.1 }
        default: return false
        }
    }

    func hash(into hasher: inout Hasher) {
        hasher.combine(serialized())
    }

    // MARK: Accessors

    subscript(key: String) -> JSON {
        if case let .object(members) = self {
            return members.first(where: { $0.0 == key })?.1 ?? .null
        }
        return .null
    }

    subscript(index: Int) -> JSON {
        if case let .array(items) = self, items.indices.contains(index) { return items[index] }
        return .null
    }

    var string: String? { if case let .string(s) = self { return s }; return nil }
    var number: Double? { if case let .number(n) = self { return n }; return nil }
    var int: Int? { number.map { Int($0) } }
    var bool: Bool? { if case let .bool(b) = self { return b }; return nil }
    var array: [JSON] { if case let .array(a) = self { return a }; return [] }
    var members: [(String, JSON)] { if case let .object(m) = self { return m }; return [] }
    var isNull: Bool { if case .null = self { return true }; return false }

    mutating func set(_ key: String, _ value: JSON) {
        var m = members
        if let i = m.firstIndex(where: { $0.0 == key }) { m[i].1 = value } else { m.append((key, value)) }
        self = .object(m)
    }

    // MARK: Serialization (preserves key order)

    func serialized(pretty: Bool = false) -> String {
        var out = ""
        write(to: &out, pretty: pretty, depth: 0)
        return out
    }

    private func write(to out: inout String, pretty: Bool, depth: Int) {
        let nl = pretty ? "\n" + String(repeating: "  ", count: depth + 1) : ""
        let close = pretty ? "\n" + String(repeating: "  ", count: depth) : ""
        switch self {
        case .null: out += "null"
        case let .bool(b): out += b ? "true" : "false"
        case let .number(n):
            if n.rounded() == n, abs(n) < 1e15 { out += String(Int64(n)) } else { out += String(n) }
        case let .string(s): JSON.escape(s, into: &out)
        case let .array(items):
            out += "["
            for (i, item) in items.enumerated() {
                if i > 0 { out += "," }
                out += nl
                item.write(to: &out, pretty: pretty, depth: depth + 1)
            }
            if !items.isEmpty { out += close }
            out += "]"
        case let .object(members):
            out += "{"
            for (i, (k, v)) in members.enumerated() {
                if i > 0 { out += "," }
                out += nl
                JSON.escape(k, into: &out)
                out += pretty ? ": " : ":"
                v.write(to: &out, pretty: pretty, depth: depth + 1)
            }
            if !members.isEmpty { out += close }
            out += "}"
        }
    }

    private static func escape(_ s: String, into out: inout String) {
        out += "\""
        for scalar in s.unicodeScalars {
            switch scalar {
            case "\"": out += "\\\""
            case "\\": out += "\\\\"
            case "\n": out += "\\n"
            case "\r": out += "\\r"
            case "\t": out += "\\t"
            default:
                if scalar.value < 0x20 {
                    out += String(format: "\\u%04x", scalar.value)
                } else {
                    out.unicodeScalars.append(scalar)
                }
            }
        }
        out += "\""
    }

    // MARK: Parsing (via JSONSerialization, then converted; key order follows the source
    // where Foundation preserves it — fine for display and round-tripping to the engine).

    static func parse(_ text: String) -> JSON? {
        guard let data = text.data(using: .utf8) else { return nil }
        return parse(data)
    }

    static func parse(_ data: Data) -> JSON? {
        guard let obj = try? JSONSerialization.jsonObject(with: data, options: [.fragmentsAllowed]) else { return nil }
        return JSON(any: obj)
    }

    init(any value: Any) {
        switch value {
        case is NSNull: self = .null
        case let n as NSNumber:
            if CFGetTypeID(n) == CFBooleanGetTypeID() { self = .bool(n.boolValue) } else { self = .number(n.doubleValue) }
        case let s as String: self = .string(s)
        case let a as [Any]: self = .array(a.map(JSON.init(any:)))
        case let d as [String: Any]: self = .object(d.keys.sorted().map { ($0, JSON(any: d[$0]!)) })
        default: self = .null
        }
    }
}

extension JSON: ExpressibleByStringLiteral, ExpressibleByIntegerLiteral, ExpressibleByFloatLiteral,
    ExpressibleByBooleanLiteral, ExpressibleByArrayLiteral, ExpressibleByDictionaryLiteral, ExpressibleByNilLiteral
{
    init(stringLiteral value: String) { self = .string(value) }
    init(integerLiteral value: Int) { self = .number(Double(value)) }
    init(floatLiteral value: Double) { self = .number(value) }
    init(booleanLiteral value: Bool) { self = .bool(value) }
    init(arrayLiteral elements: JSON...) { self = .array(elements) }
    init(dictionaryLiteral elements: (String, JSON)...) { self = .object(elements) }
    init(nilLiteral: ()) { self = .null }
}

extension JSON {
    static func vec3(_ x: Double, _ y: Double, _ z: Double) -> JSON { .array([.number(x), .number(y), .number(z)]) }
}
