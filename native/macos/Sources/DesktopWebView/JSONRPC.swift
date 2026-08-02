import Foundation

enum JSONRPC {
    struct Request: Codable {
        let jsonrpc: String
        let id: JSONValue?
        let method: String
        let params: JSONValue?
    }

    struct Response: Codable {
        let jsonrpc: String
        let id: JSONValue?
        let result: JSONValue?
        let error: RPCError?

        static func ok(id: JSONValue?, result: JSONValue?) -> Response {
            Response(jsonrpc: "2.0", id: id, result: result, error: nil)
        }

        static func fail(id: JSONValue?, code: Int, message: String) -> Response {
            Response(jsonrpc: "2.0", id: id, result: nil, error: RPCError(code: code, message: message, data: nil))
        }
    }

    struct Notification: Codable {
        let jsonrpc: String
        let method: String
        let params: JSONValue?
    }

    struct RPCError: Codable {
        let code: Int
        let message: String
        let data: JSONValue?
    }
}

enum JSONValue: Codable, Equatable {
    case null
    case bool(Bool)
    case number(Double)
    case string(String)
    case array([JSONValue])
    case object([String: JSONValue])

    init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if c.decodeNil() { self = .null; return }
        if let b = try? c.decode(Bool.self) { self = .bool(b); return }
        if let i = try? c.decode(Int.self) { self = .number(Double(i)); return }
        if let d = try? c.decode(Double.self) { self = .number(d); return }
        if let s = try? c.decode(String.self) { self = .string(s); return }
        if let a = try? c.decode([JSONValue].self) { self = .array(a); return }
        if let o = try? c.decode([String: JSONValue].self) { self = .object(o); return }
        throw DecodingError.dataCorruptedError(in: c, debugDescription: "Unsupported JSON")
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        switch self {
        case .null: try c.encodeNil()
        case .bool(let b): try c.encode(b)
        case .number(let n):
            if n.rounded() == n, n >= Double(Int.min), n <= Double(Int.max) {
                try c.encode(Int(n))
            } else {
                try c.encode(n)
            }
        case .string(let s): try c.encode(s)
        case .array(let a): try c.encode(a)
        case .object(let o): try c.encode(o)
        }
    }

    var stringValue: String? {
        if case .string(let s) = self { return s }
        return nil
    }

    var boolValue: Bool? {
        if case .bool(let b) = self { return b }
        return nil
    }

    var intValue: Int? {
        if case .number(let n) = self { return Int(n) }
        return nil
    }

    var objectValue: [String: JSONValue]? {
        if case .object(let o) = self { return o }
        return nil
    }

    subscript(key: String) -> JSONValue? {
        objectValue?[key]
    }
}

extension JSONEncoder {
    static let rpc: JSONEncoder = {
        let e = JSONEncoder()
        e.outputFormatting = []
        return e
    }()
}

extension JSONDecoder {
    static let rpc = JSONDecoder()
}
