import Darwin
import Foundation

enum BeamCli {
    struct NodeSpec {
        var name: String
        var short: Bool
    }

    /// Runs `--edw-rpc` / `--edw-recover` when set. Returns an exit code, or nil to start the UI.
    static func exclusiveExitCode(_ config: HostConfig) -> Int32? {
        if config.rpcExpr != nil && config.recover {
            fputs("edw: --edw-rpc and --edw-recover are mutually exclusive\n", stderr)
            return 1
        }
        if let expr = config.rpcExpr {
            if expr.isEmpty {
                fputs("edw: --edw-rpc requires an Elixir expression\n", stderr)
                return 1
            }
            return runRpc(config: config, expr: expr)
        }
        if config.recover {
            return runRecover(config: config)
        }
        return nil
    }

    static func resolvedBeamDir(_ config: HostConfig) -> String {
        let root = config.resourcesRoot()
        guard let path = config.beamPath, !path.isEmpty else {
            return (root as NSString).appendingPathComponent("beam")
        }
        if (path as NSString).isAbsolutePath { return path }
        return (root as NSString).appendingPathComponent(path)
    }

    static func resolvedWorkingDir(_ config: HostConfig) -> String {
        let beamDir = resolvedBeamDir(config)
        guard let wd = config.beamWorkingDir, !wd.isEmpty else { return beamDir }
        if (wd as NSString).isAbsolutePath { return wd }
        return (config.resourcesRoot() as NSString).appendingPathComponent(wd)
    }

    static func resolveAppName(_ config: HostConfig) -> String? {
        if let name = config.beamApp, !name.isEmpty { return name }
        let bin = (resolvedBeamDir(config) as NSString).appendingPathComponent("bin")
        guard let files = try? FileManager.default.contentsOfDirectory(atPath: bin) else { return nil }
        return files.sorted().first { !$0.hasPrefix(".") && !$0.hasSuffix(".bat") && !$0.hasSuffix(".cmd") }
    }

    static func resolveRecoveryScript(_ config: HostConfig) -> String? {
        guard let raw = config.recoveryScript, !raw.isEmpty else { return nil }
        let path: String
        if (raw as NSString).isAbsolutePath {
            path = raw
        } else {
            path = (config.resourcesRoot() as NSString).appendingPathComponent(raw)
        }
        var isDir: ObjCBool = false
        guard FileManager.default.fileExists(atPath: path, isDirectory: &isDir), !isDir.boolValue else {
            return nil
        }
        return path
    }

    static func evalFileExpr(scriptPath: String) -> String {
        let posix = scriptPath.replacingOccurrences(of: "\\", with: "/")
        let escaped = posix
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        return "Code.eval_file(\"\(escaped)\")"
    }

    @discardableResult
    static func runRecover(config: HostConfig) -> Int32 {
        guard let script = resolveRecoveryScript(config) else {
            fputs("edw: recovery_script is missing or not a file\n", stderr)
            return 1
        }
        guard let app = resolveAppName(config) else {
            fputs("edw: no beam app_name and no bin script found in \(resolvedBeamDir(config))\n", stderr)
            return 1
        }
        let beamDir = resolvedBeamDir(config)
        var bin = (beamDir as NSString).appendingPathComponent("bin/\(app)")
        if !FileManager.default.isExecutableFile(atPath: bin) && FileManager.default.fileExists(atPath: bin + ".bat") {
            bin += ".bat"
        }
        guard FileManager.default.fileExists(atPath: bin) else {
            fputs("edw: beam script not found: \(bin)\n", stderr)
            return 1
        }
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: bin)
        proc.arguments = ["eval", evalFileExpr(scriptPath: script)]
        proc.currentDirectoryURL = URL(fileURLWithPath: resolvedWorkingDir(config))
        var env = ProcessInfo.processInfo.environment
        for (k, v) in config.extraEnv { env[k] = v }
        proc.environment = env
        do {
            try proc.run()
            proc.waitUntilExit()
            return proc.terminationStatus
        } catch {
            fputs("edw: failed to run recovery eval: \(error)\n", stderr)
            return 1
        }
    }

    static func runRpc(config: HostConfig, expr: String) -> Int32 {
        let beamDir = resolvedBeamDir(config)
        guard let erlCall = findErlCall(beamDir: beamDir) else {
            fputs("edw: erl_call not found under \(beamDir) or PATH\n", stderr)
            return 1
        }
        guard let cookie = findCookie(config: config, beamDir: beamDir) else {
            fputs("edw: cookie not found (ini cookie/cookie_file, releases/COOKIE, or vm.args)\n", stderr)
            return 1
        }
        guard let node = findNode(config: config, beamDir: beamDir) else {
            fputs("edw: node not found (ini [beam] node or vm.args -name/-sname)\n", stderr)
            return 1
        }
        let b64 = Data(expr.utf8).base64EncodedString()
        let outFile = FileManager.default.temporaryDirectory
            .appendingPathComponent("edw-rpc-out-\(UUID().uuidString)")
        let outPath = outFile.path.replacingOccurrences(of: "\\", with: "/")
        let erlang = """
        Bin = base64:decode(<<"\(b64)">>),
        {Val, _} = 'Elixir.Code':eval_string(Bin),
        Inspect = 'Elixir.Kernel':inspect(Val),
        ok = file:write_file(<<"\(outPath)">>, Inspect).
        """
        var args = ["-c", cookie, "-r", "-no_result_term"]
        if node.short {
            args += ["-sname", node.name]
        } else {
            args += ["-name", node.name]
        }
        args.append("-e")

        let tmp = FileManager.default.temporaryDirectory
            .appendingPathComponent("edw-rpc-\(UUID().uuidString).erl")
        var payload = erlang.trimmingCharacters(in: .whitespacesAndNewlines)
        if !payload.hasSuffix(".") { payload += "." }
        payload += "\n"
        do {
            try payload.write(to: tmp, atomically: true, encoding: .utf8)
        } catch {
            fputs("edw: failed to write erl_call input: \(error)\n", stderr)
            return 1
        }
        defer {
            try? FileManager.default.removeItem(at: tmp)
            try? FileManager.default.removeItem(at: outFile)
        }

        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: erlCall)
        proc.arguments = args
        proc.environment = ProcessInfo.processInfo.environment
        do {
            let readHandle = try FileHandle(forReadingFrom: tmp)
            proc.standardInput = readHandle
            proc.standardOutput = FileHandle(fileDescriptor: STDOUT_FILENO, closeOnDealloc: false)
            proc.standardError = FileHandle(fileDescriptor: STDERR_FILENO, closeOnDealloc: false)
            try proc.run()
            proc.waitUntilExit()
            try readHandle.close()
            if proc.terminationStatus == 0,
               let data = try? Data(contentsOf: outFile),
               let text = String(data: data, encoding: .utf8) {
                let line = text.hasSuffix("\n") ? text : text + "\n"
                fputs(line, stdout)
                fflush(stdout)
                fputs(line, stderr)
                fflush(stderr)
                return 0
            }
            if proc.terminationStatus == 0 {
                fputs("edw: erl_call succeeded but wrote no result file\n", stderr)
                return 1
            }
            return proc.terminationStatus
        } catch {
            fputs("edw: failed to run erl_call: \(error)\n", stderr)
            return 1
        }
    }

    static func findErlCall(beamDir: String) -> String? {
        let fm = FileManager.default
        if let p = firstMatch(in: beamDir, directoryPrefix: "erts-", file: "bin/erl_call"),
           fm.isExecutableFile(atPath: p) {
            return p
        }
        let lib = (beamDir as NSString).appendingPathComponent("lib")
        if let p = firstMatch(in: lib, directoryPrefix: "erl_interface-", file: "bin/erl_call"),
           fm.isExecutableFile(atPath: p) {
            return p
        }
        return which("erl_call")
    }

    static func findCookie(config: HostConfig, beamDir: String) -> String? {
        if let c = config.beamCookie, !c.isEmpty { return c }
        if let file = config.beamCookieFile, !file.isEmpty {
            let path = (file as NSString).isAbsolutePath
                ? file
                : (config.resourcesRoot() as NSString).appendingPathComponent(file)
            if let text = readTrimmed(path) { return text }
        }
        if let text = readTrimmed((beamDir as NSString).appendingPathComponent("releases/COOKIE")) {
            return text
        }
        if let vm = readVmArgs(beamDir: beamDir), let cookie = vm.cookie {
            return cookie
        }
        return nil
    }

    static func findNode(config: HostConfig, beamDir: String) -> NodeSpec? {
        if let n = config.beamNode, !n.isEmpty {
            let short = !n.contains("@")
            return NodeSpec(name: n, short: short)
        }
        if let vm = readVmArgs(beamDir: beamDir), let node = vm.node {
            return node
        }
        return nil
    }

    private struct VmArgs {
        var node: NodeSpec?
        var cookie: String?
    }

    private static func readVmArgs(beamDir: String) -> VmArgs? {
        let releases = (beamDir as NSString).appendingPathComponent("releases")
        var vmPath: String?
        if let startErl = readTrimmed((releases as NSString).appendingPathComponent("start_erl.data")) {
            let parts = startErl.split(whereSeparator: { $0 == " " || $0 == "\t" }).map(String.init)
            if parts.count >= 2 {
                vmPath = (releases as NSString).appendingPathComponent("\(parts[1])/vm.args")
            }
        }
        if vmPath == nil {
            if let vers = try? FileManager.default.contentsOfDirectory(atPath: releases) {
                for v in vers.sorted() where !v.hasPrefix(".") {
                    let candidate = (releases as NSString).appendingPathComponent("\(v)/vm.args")
                    if FileManager.default.fileExists(atPath: candidate) {
                        vmPath = candidate
                        break
                    }
                }
            }
        }
        guard let vmPath, let text = try? String(contentsOfFile: vmPath, encoding: .utf8) else { return nil }
        var out = VmArgs()
        for raw in text.components(separatedBy: .newlines) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") { continue }
            let toks = line.split(whereSeparator: { $0.isWhitespace }).map(String.init)
            var i = 0
            while i < toks.count {
                let t = toks[i]
                if t == "-sname", i + 1 < toks.count {
                    out.node = NodeSpec(name: toks[i + 1], short: true)
                    i += 2
                    continue
                }
                if t == "-name", i + 1 < toks.count {
                    out.node = NodeSpec(name: toks[i + 1], short: false)
                    i += 2
                    continue
                }
                if t == "-setcookie", i + 1 < toks.count {
                    out.cookie = toks[i + 1]
                    i += 2
                    continue
                }
                i += 1
            }
        }
        return out
    }

    private static func firstMatch(in dir: String, directoryPrefix: String, file: String) -> String? {
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: dir) else { return nil }
        for name in names.sorted() where name.hasPrefix(directoryPrefix) {
            let p = (dir as NSString).appendingPathComponent("\(name)/\(file)")
            if FileManager.default.fileExists(atPath: p) { return p }
        }
        return nil
    }

    private static func which(_ name: String) -> String? {
        guard let path = ProcessInfo.processInfo.environment["PATH"] else { return nil }
        for dir in path.split(separator: ":") {
            let p = "\(dir)/\(name)"
            if FileManager.default.isExecutableFile(atPath: p) { return p }
        }
        return nil
    }

    private static func readTrimmed(_ path: String) -> String? {
        guard let text = try? String(contentsOfFile: path, encoding: .utf8) else { return nil }
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }
}
