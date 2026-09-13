import Darwin
import Foundation

enum BeamCli {
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
        guard config.instances == .single else {
            fputs("edw: --edw-rpc requires a running single-instance host\n", stderr)
            return 1
        }
        var inspect = ""
        let code = InstanceLock.clientEval(config.resolvedInstanceId(), expr: expr, inspect: &inspect)
        if code != 0 { return code }
        let line = inspect.hasSuffix("\n") ? inspect : inspect + "\n"
        fputs(line, stdout)
        fflush(stdout)
        fputs(line, stderr)
        fflush(stderr)
        return 0
    }
}
