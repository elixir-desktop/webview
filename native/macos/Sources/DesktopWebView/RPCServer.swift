import Foundation
import Network

final class RPCServer {
    private let queue = DispatchQueue(label: "edw.rpc")
    private var listener: NWListener?
    private var connection: NWConnection?
    private var buffer = Data()
    private var nextOutboundId: Int = 1
    private var pending: [String: (JSONValue?) -> Void] = [:]

    var onRequest: ((JSONRPC.Request, @escaping (JSONRPC.Response) -> Void) -> Void)?
    var onNotification: ((JSONRPC.Notification) -> Void)?
    var onDisconnect: (() -> Void)?

    private(set) var port: UInt16 = 0

    func start(host: String, port: UInt16) throws {
        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        let endpointPort: NWEndpoint.Port = port == 0 ? .any : NWEndpoint.Port(rawValue: port)!
        listener = try NWListener(using: params, on: endpointPort)
        listener?.newConnectionHandler = { [weak self] conn in
            self?.accept(conn)
        }
        listener?.stateUpdateHandler = { [weak self] state in
            if case .ready = state, let p = self?.listener?.port?.rawValue {
                self?.port = p
                fputs("listening \(p)\n", stdout)
                fflush(stdout)
            }
        }
        listener?.start(queue: queue)
    }

    private func accept(_ conn: NWConnection) {
        // Single client: replace previous
        connection?.cancel()
        connection = conn
        buffer.removeAll()
        conn.start(queue: queue)
        receive(on: conn)
        conn.stateUpdateHandler = { [weak self] state in
            switch state {
            case .failed, .cancelled:
                if self?.connection === conn {
                    self?.connection = nil
                    self?.onDisconnect?()
                }
            default:
                break
            }
        }
    }

    private func receive(on conn: NWConnection) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 1024 * 1024) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            if let data, !data.isEmpty {
                self.buffer.append(data)
                self.drain()
            }
            if isComplete || error != nil {
                if self.connection === conn {
                    self.connection = nil
                    self.onDisconnect?()
                }
                return
            }
            self.receive(on: conn)
        }
    }

    private func drain() {
        while buffer.count >= 4 {
            let len = buffer.prefix(4).withUnsafeBytes { $0.load(as: UInt32.self).bigEndian }
            if buffer.count < 4 + Int(len) { return }
            let payload = buffer.subdata(in: 4..<(4 + Int(len)))
            buffer.removeSubrange(0..<(4 + Int(len)))
            handlePayload(payload)
        }
    }

    private func handlePayload(_ data: Data) {
        guard let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return }
        let hasMethod = obj["method"] != nil
        let hasId = obj["id"] != nil
        let hasResultOrError = obj["result"] != nil || obj["error"] != nil

        if hasMethod && hasId {
            if let req = try? JSONDecoder.rpc.decode(JSONRPC.Request.self, from: data) {
                onRequest?(req) { [weak self] resp in
                    self?.send(resp)
                }
            }
        } else if hasMethod && !hasId {
            if let n = try? JSONDecoder.rpc.decode(JSONRPC.Notification.self, from: data) {
                onNotification?(n)
            }
        } else if hasResultOrError && hasId {
            if let resp = try? JSONDecoder.rpc.decode(JSONRPC.Response.self, from: data),
               let id = resp.id {
                let key = idKey(id)
                if let cb = pending.removeValue(forKey: key) {
                    cb(resp.result)
                }
            }
        }
    }

    func send(_ response: JSONRPC.Response) {
        guard let data = try? JSONEncoder.rpc.encode(response) else { return }
        sendRaw(data)
    }

    func notify(method: String, params: JSONValue?) {
        let n = JSONRPC.Notification(jsonrpc: "2.0", method: method, params: params)
        guard let data = try? JSONEncoder.rpc.encode(n) else { return }
        sendRaw(data)
    }

    func request(method: String, params: JSONValue?, completion: @escaping (JSONValue?) -> Void) {
        let idNum = nextOutboundId
        nextOutboundId += 1
        let id = JSONValue.number(Double(idNum))
        pending[idKey(id)] = completion
        let req = JSONRPC.Request(jsonrpc: "2.0", id: id, method: method, params: params)
        guard let data = try? JSONEncoder.rpc.encode(req) else { return }
        sendRaw(data)
    }

    private func sendRaw(_ data: Data) {
        guard let conn = connection else { return }
        var len = UInt32(data.count).bigEndian
        var packet = Data(bytes: &len, count: 4)
        packet.append(data)
        conn.send(content: packet, completion: .contentProcessed { _ in })
    }

    private func idKey(_ id: JSONValue) -> String {
        switch id {
        case .number(let n): return "n:\(Int(n))"
        case .string(let s): return "s:\(s)"
        default: return "x:\(id)"
        }
    }

    func closeConnection() {
        connection?.cancel()
        connection = nil
    }
}
