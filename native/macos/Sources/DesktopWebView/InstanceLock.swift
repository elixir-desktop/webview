import Darwin
import Foundation

final class InstanceLock {
    typealias ActivateFn = ([String]) -> Void
    typealias EvalFn = (String, @escaping (Bool, String) -> Void) -> Void

    private var listenFD: Int32 = -1
    private var path: String = ""
    private var running = false
    private var thread: Thread?
    private var activate: ActivateFn?
    private var eval: EvalFn?

    deinit {
        stop()
    }

    static func sanitizeId(_ raw: String) -> String {
        var out = ""
        for ch in raw.unicodeScalars {
            if CharacterSet.alphanumerics.contains(ch) || ch == "." || ch == "_" || ch == "-" {
                out.append(Character(ch))
            } else {
                out.append("_")
            }
        }
        if out.count > 64 { out = String(out.prefix(64)) }
        return out.isEmpty ? "DesktopWebView" : out
    }

    static func socketPath(_ instanceId: String) -> String {
        let tmp = ProcessInfo.processInfo.environment["TMPDIR"] ?? "/tmp"
        let root = tmp.hasSuffix("/") ? String(tmp.dropLast()) : tmp
        return "\(root)/edw-\(getuid())-\(sanitizeId(instanceId)).sock"
    }

    func tryServe(_ instanceId: String) -> Bool {
        path = InstanceLock.socketPath(instanceId)
        if InstanceLock.canConnect(path) { return false }
        unlink(path)

        listenFD = socket(AF_UNIX, SOCK_STREAM, 0)
        if listenFD < 0 { return false }

        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        path.withCString { cstr in
            withUnsafeMutableBytes(of: &addr.sun_path) { buf in
                guard let base = buf.baseAddress else { return }
                strncpy(base.assumingMemoryBound(to: CChar.self), cstr, buf.count - 1)
            }
        }
        let len = socklen_t(MemoryLayout<sockaddr_un>.size)
        let bindRC = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(listenFD, $0, len) }
        }
        if bindRC != 0 {
            close(listenFD)
            listenFD = -1
            return false
        }
        if listen(listenFD, 8) != 0 {
            close(listenFD)
            listenFD = -1
            unlink(path)
            return false
        }
        return true
    }

    func setHandlers(activate: @escaping ActivateFn, eval: @escaping EvalFn) {
        self.activate = activate
        self.eval = eval
    }

    func start() {
        guard listenFD >= 0, !running else { return }
        running = true
        let thread = Thread { [weak self] in self?.acceptLoop() }
        thread.name = "edw.instance"
        self.thread = thread
        thread.start()
    }

    func stop() {
        running = false
        if listenFD >= 0 {
            shutdown(listenFD, SHUT_RDWR)
            close(listenFD)
            listenFD = -1
        }
        if !path.isEmpty { unlink(path) }
    }

    static func clientActivate(_ instanceId: String, argv: [String]) -> Int32 {
        var err = ""
        let code = rpcCall(instanceId, method: "instance.activate", params: ["argv": argv], result: { _ in }, err: &err)
        if code != 0 {
            fputs("edw: instance.activate failed: \(err)\n", stderr)
        }
        return code
    }

    static func clientEval(_ instanceId: String, expr: String, inspect: inout String) -> Int32 {
        var err = ""
        var out = ""
        let code = rpcCall(instanceId, method: "instance.eval", params: ["expr": expr], result: { obj in
            if let s = obj["inspect"] as? String { out = s }
        }, err: &err)
        if code != 0 {
            fputs("edw: instance.eval failed: \(err)\n", stderr)
            return code
        }
        if out.isEmpty {
            fputs("edw: instance.eval missing inspect\n", stderr)
            return 1
        }
        inspect = out
        return 0
    }

    private func acceptLoop() {
        while running {
            let fd = accept(listenFD, nil, nil)
            if fd < 0 {
                if !running { break }
                if errno == EINTR { continue }
                break
            }
            handleClient(fd)
            close(fd)
        }
    }

    private func handleClient(_ fd: Int32) {
        guard let payload = InstanceLock.readFrame(fd) else { return }
        guard let obj = try? JSONSerialization.jsonObject(with: Data(payload.utf8)) as? [String: Any] else { return }
        let method = obj["method"] as? String ?? ""
        let id = obj["id"] ?? 1
        let params = obj["params"] as? [String: Any] ?? [:]

        if method == "instance.activate" {
            let argv = params["argv"] as? [String] ?? []
            runOnMainSync { self.activate?(argv) }
            _ = InstanceLock.writeFrame(fd, InstanceLock.encodeOK(id: id, result: true))
            return
        }
        if method == "instance.eval" {
            let expr = params["expr"] as? String ?? ""
            if expr.isEmpty {
                _ = InstanceLock.writeFrame(fd, InstanceLock.encodeError(id: id, code: -32602, message: "expr required"))
                return
            }
            guard let eval else {
                _ = InstanceLock.writeFrame(fd, InstanceLock.encodeError(id: id, code: -32000, message: "no initialized Elixir client"))
                return
            }
            let sem = DispatchSemaphore(value: 0)
            var ok = false
            var text = ""
            runOnMainSync {
                eval(expr) { o, t in
                    ok = o
                    text = t
                    sem.signal()
                }
            }
            if sem.wait(timeout: .now() + 15) == .timedOut {
                _ = InstanceLock.writeFrame(fd, InstanceLock.encodeError(id: id, code: -32000, message: "rpc.eval timed out"))
                return
            }
            if !ok {
                _ = InstanceLock.writeFrame(fd, InstanceLock.encodeError(id: id, code: -32000, message: text.isEmpty ? "rpc.eval failed" : text))
                return
            }
            _ = InstanceLock.writeFrame(fd, InstanceLock.encodeOK(id: id, result: ["inspect": text]))
            return
        }
        _ = InstanceLock.writeFrame(fd, InstanceLock.encodeError(id: id, code: -32601, message: "Method not found"))
    }

    private func runOnMainSync(_ body: @escaping () -> Void) {
        if Thread.isMainThread {
            body()
            return
        }
        DispatchQueue.main.sync(execute: body)
    }

    private static func canConnect(_ path: String) -> Bool {
        let fd = connectPath(path)
        if fd >= 0 {
            close(fd)
            return true
        }
        return false
    }

    private static func connectPath(_ path: String) -> Int32 {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        if fd < 0 { return -1 }
        var tv = timeval(tv_sec: 15, tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        path.withCString { cstr in
            withUnsafeMutableBytes(of: &addr.sun_path) { buf in
                guard let base = buf.baseAddress else { return }
                strncpy(base.assumingMemoryBound(to: CChar.self), cstr, buf.count - 1)
            }
        }
        let len = socklen_t(MemoryLayout<sockaddr_un>.size)
        let rc = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, len) }
        }
        if rc != 0 {
            close(fd)
            return -1
        }
        return fd
    }

    private static func rpcCall(_ instanceId: String, method: String, params: [String: Any],
                                result: ([String: Any]) -> Void, err: inout String) -> Int32 {
        let fd = connectPath(socketPath(instanceId))
        if fd < 0 {
            err = "no running single-instance host"
            return 1
        }
        let body: [String: Any] = ["jsonrpc": "2.0", "id": 1, "method": method, "params": params]
        guard let data = try? JSONSerialization.data(withJSONObject: body),
              writeFrame(fd, String(data: data, encoding: .utf8) ?? "") else {
            close(fd)
            err = "control socket write failed"
            return 1
        }
        guard let payload = readFrame(fd) else {
            close(fd)
            err = "control socket read failed"
            return 1
        }
        close(fd)
        guard let obj = try? JSONSerialization.jsonObject(with: Data(payload.utf8)) as? [String: Any] else {
            err = "control socket parse failed"
            return 1
        }
        if let error = obj["error"] as? [String: Any] {
            err = (error["message"] as? String) ?? "instance request failed"
            return 1
        }
        if let res = obj["result"] as? [String: Any] {
            result(res)
        } else if obj["result"] != nil {
            result([:])
        }
        return 0
    }

    private static func writeFrame(_ fd: Int32, _ json: String) -> Bool {
        var len = UInt32(json.utf8.count).bigEndian
        let hdr = withUnsafeBytes(of: &len) { Data($0) }
        guard writeAll(fd, hdr), writeAll(fd, Data(json.utf8)) else { return false }
        return true
    }

    private static func readFrame(_ fd: Int32) -> String? {
        var hdr = [UInt8](repeating: 0, count: 4)
        guard readAll(fd, &hdr) else { return nil }
        let len = hdr.withUnsafeBytes { $0.load(as: UInt32.self).bigEndian }
        if len == 0 || len > 8 * 1024 * 1024 { return nil }
        var buf = [UInt8](repeating: 0, count: Int(len))
        guard readAll(fd, &buf) else { return nil }
        return String(bytes: buf, encoding: .utf8)
    }

    private static func writeAll(_ fd: Int32, _ data: Data) -> Bool {
        data.withUnsafeBytes { raw in
            var p = raw.bindMemory(to: UInt8.self).baseAddress!
            var left = data.count
            while left > 0 {
                let n = write(fd, p, left)
                if n < 0 {
                    if errno == EINTR { continue }
                    return false
                }
                if n == 0 { return false }
                p += n
                left -= n
            }
            return true
        }
    }

    private static func readAll(_ fd: Int32, _ buf: inout [UInt8]) -> Bool {
        var offset = 0
        while offset < buf.count {
            let n = buf.withUnsafeMutableBytes { raw in
                read(fd, raw.baseAddress!.advanced(by: offset), buf.count - offset)
            }
            if n < 0 {
                if errno == EINTR { continue }
                return false
            }
            if n == 0 { return false }
            offset += n
        }
        return true
    }

    private static func encodeOK(id: Any, result: Any) -> String {
        let body: [String: Any] = ["jsonrpc": "2.0", "id": id, "result": result]
        if let data = try? JSONSerialization.data(withJSONObject: body),
           let s = String(data: data, encoding: .utf8) {
            return s
        }
        return #"{"jsonrpc":"2.0","id":1,"result":true}"#
    }

    private static func encodeError(id: Any, code: Int, message: String) -> String {
        let body: [String: Any] = [
            "jsonrpc": "2.0",
            "id": id,
            "error": ["code": code, "message": message]
        ]
        if let data = try? JSONSerialization.data(withJSONObject: body),
           let s = String(data: data, encoding: .utf8) {
            return s
        }
        return #"{"jsonrpc":"2.0","id":1,"error":{"code":-32000,"message":"error"}}"#
    }
}
