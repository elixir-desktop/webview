import Foundation

struct HostConfig {
    var noBeam: Bool = false
    var port: UInt16 = 0
    var host: String = "127.0.0.1"
    var configPath: String? = nil
    var lifetime: Lifetime = .reconnect
    var testRPC: Bool = false
    var beamPath: String? = nil
    var beamApp: String? = nil
    var beamArgs: [String] = ["start"]
    var beamWorkingDir: String? = nil
    var beamEnabled: Bool = true
    var extraEnv: [String: String] = [:]
    var forwardedArgv: [String] = []
    /// When true, the host re-spawns BEAM after it exits unexpectedly.
    /// Driven by the host-first macOS bundle: replaces the `heart` watchdog.
    var restartBeam: Bool = true
    /// Maximum number of consecutive BEAM respawns before the host gives up.
    /// `0` means retry indefinitely.
    var restartMaxAttempts: Int = 0
    /// Initial backoff between respawn attempts (ms). Doubles per attempt,
    /// capped at 5000 ms.
    var restartBackoffMs: UInt32 = 500

    enum Lifetime: String {
        case reconnect
        case coupled
    }

    static func parse(argv: [String]) -> HostConfig {
        var cfg = HostConfig()
        var forwarded: [String] = []
        var i = 1 // skip executable
        var passthrough = false

        while i < argv.count {
            let a = argv[i]
            if passthrough || a == "--" {
                if a == "--" { passthrough = true; i += 1; continue }
                forwarded.append(a)
                i += 1
                continue
            }
            if a.hasPrefix("--edw-") {
                let body = String(a.dropFirst(6)) // after --edw-
                if body == "no-beam" {
                    cfg.noBeam = true
                } else if body == "test-rpc" {
                    cfg.testRPC = true
                } else if body.hasPrefix("port=") {
                    cfg.port = UInt16(body.dropFirst(5)) ?? 0
                } else if body.hasPrefix("host=") {
                    cfg.host = String(body.dropFirst(5))
                } else if body.hasPrefix("config=") {
                    cfg.configPath = String(body.dropFirst(7))
                } else if body.hasPrefix("lifetime=") {
                    let v = String(body.dropFirst(9))
                    cfg.lifetime = Lifetime(rawValue: v) ?? .reconnect
                } else if body.hasPrefix("beam-path=") {
                    cfg.beamPath = String(body.dropFirst(10))
                } else if body.hasPrefix("beam-app=") {
                    cfg.beamApp = String(body.dropFirst(9))
                } else if body.hasPrefix("restart-beam=") {
                    cfg.restartBeam = (body.dropFirst(13) != "false" && body.dropFirst(13) != "0")
                } else if body.hasPrefix("max-restart-attempts=") {
                    cfg.restartMaxAttempts = Int(body.dropFirst(21)) ?? 0
                } else if body.hasPrefix("restart-backoff-ms=") {
                    cfg.restartBackoffMs = UInt32(body.dropFirst(19)) ?? 500
                } else {
                    fputs("unknown --edw flag: \(a)\n", stderr)
                }
                i += 1
                continue
            }
            forwarded.append(a)
            i += 1
        }
        cfg.forwardedArgv = forwarded
        cfg.applyIni()
        if cfg.noBeam { cfg.beamEnabled = false }
        return cfg
    }

    mutating func applyIni() {
        let path = resolveIniPath()
        guard let path, let text = try? String(contentsOfFile: path, encoding: .utf8) else { return }
        let ini = Ini.parse(text)
        if let v = ini["network", "host"] { host = v }
        if let v = ini["network", "port"], let p = UInt16(v) { port = p }
        if let v = ini["lifetime", "mode"], let l = Lifetime(rawValue: v) { lifetime = l }
        if let v = ini["lifetime", "restart_beam"] {
            restartBeam = !(v == "false" || v == "0")
        }
        if let v = ini["lifetime", "restart_max_attempts"], let n = Int(v) {
            restartMaxAttempts = n
        }
        if let v = ini["lifetime", "restart_backoff_ms"], let n = UInt32(v) {
            restartBackoffMs = n
        }
        if let v = ini["beam", "enabled"] {
            beamEnabled = !(v == "false" || v == "0")
        }
        if let v = ini["beam", "path"] { beamPath = v }
        if let v = ini["beam", "app_name"] { beamApp = v }
        if let v = ini["beam", "args"] {
            beamArgs = v.split(separator: " ").map(String.init)
        }
        if let v = ini["beam", "working_dir"] { beamWorkingDir = v }
        for (k, v) in ini.section("env") {
            extraEnv[k] = v
        }
    }

    func resolveIniPath() -> String? {
        if let configPath { return configPath }
        let exe = Bundle.main.executableURL?.deletingLastPathComponent().path
            ?? URL(fileURLWithPath: CommandLine.arguments[0]).deletingLastPathComponent().path
        let beside = (exe as NSString).appendingPathComponent("DesktopWebView.ini")
        if FileManager.default.fileExists(atPath: beside) { return beside }
        // App bundle Resources
        if let res = Bundle.main.resourcePath {
            let p = (res as NSString).appendingPathComponent("DesktopWebView.ini")
            if FileManager.default.fileExists(atPath: p) { return p }
        }
        return nil
    }

    func resourcesRoot() -> String {
        if let res = Bundle.main.resourcePath { return res }
        return URL(fileURLWithPath: CommandLine.arguments[0]).deletingLastPathComponent().path
    }
}

struct Ini {
    private var sections: [String: [String: String]] = [:]

    static func parse(_ text: String) -> Ini {
        var ini = Ini()
        var current = "default"
        for raw in text.components(separatedBy: .newlines) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") || line.hasPrefix(";") { continue }
            if line.hasPrefix("["), line.hasSuffix("]") {
                current = String(line.dropFirst().dropLast()).lowercased()
                continue
            }
            let parts = line.split(separator: "=", maxSplits: 1).map {
                $0.trimmingCharacters(in: .whitespaces)
            }
            guard parts.count == 2 else { continue }
            ini.sections[current, default: [:]][parts[0]] = parts[1]
        }
        return ini
    }

    subscript(section: String, key: String) -> String? {
        sections[section.lowercased()]?[key]
    }

    func section(_ name: String) -> [String: String] {
        sections[name.lowercased()] ?? [:]
    }
}
