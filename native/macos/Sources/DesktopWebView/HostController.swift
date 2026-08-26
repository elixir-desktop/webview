import AppKit
import AVFoundation
import Foundation
import UserNotifications
import WebKit

final class HostController: NSObject, UNUserNotificationCenterDelegate {
    let config: HostConfig
    let server = RPCServer()
    private var initialized = false
    private var idCounter = 0
    private var windows: [String: WebWindowController] = [:]
    private var webviews: [String: String] = [:] // webviewId -> windowId
    private var menus: [String: NSMenu] = [:]
    private var menuOnclicks: [String: [Int: String]] = [:] // menuId -> tag -> onclick
    private var trays: [String: NSStatusItem] = [:]
    /// menu_id → tray_id bindings so menu.update reattaches the status item menu.
    private var trayMenus: [String: String] = [:]
    private var icons: [String: NSImage] = [:]
    private var permissionPolicy: [String: [String: String]] = [:] // origin -> type -> allow|deny|ask
    private var beamProcess: Process?
    private var appleMenuSet = false
    /// Display name for the macOS application menu (e.g. "Diode Collab").
    private var appDisplayName = "DesktopWebView"
    private var appleMenuItem: NSMenuItem?
    /// True after the user/OS asked to quit; BEAM is expected to shut down next.
    private(set) var quitRequested = false
    /// When true, `applicationShouldTerminate` may finish tearing down the host.
    private(set) var readyToTerminate = false
    /// Set by `system.prepare_quit` so a BEAM exit during this window is
    /// treated as a clean shutdown (no host-driven respawn).
    private var expectedBeamExitUntil: Date? = nil
    /// Number of times the host has respawned BEAM in this process's lifetime.
    private var beamRestartAttempts: Int = 0
    /// Pending restart timer; cancelled if the host quits before it fires.
    private var restartTimer: DispatchSourceTimer? = nil
    /// When true, `applicationShouldTerminate` cancels so last-window teardown
    /// during session reset cannot quit the host (CI / `--edw-no-beam`).
    var suppressTerminate = false

    init(config: HostConfig) {
        self.config = config
        super.init()
    }

    func start() throws {
        server.onRequest = { [weak self] req, reply in
            DispatchQueue.main.async {
                self?.handle(req, reply: reply)
            }
        }
        server.onDisconnect = { [weak self] in
            DispatchQueue.main.async {
                self?.clientDisconnected()
            }
        }
        server.onSessionEnd = { [weak self] in
            DispatchQueue.main.async {
                self?.resetSession()
            }
        }
        try server.start(host: config.host, port: config.port)

        // Wait briefly for port assignment
        for _ in 0..<50 {
            if server.port != 0 { break }
            Thread.sleep(forTimeInterval: 0.02)
        }

        if config.beamEnabled && !config.noBeam {
            spawnBeam()
        }

        NSApp.setActivationPolicy(.regular)
        // Default Edit menu is required for keyboard accelerators (Cmd+C/V/X/A) to
        // reach the WKWebView responder chain. installMainMenuPreservingApple keeps
        // any pre-existing Apple menu item across this call.
        installMainMenuPreservingApple(extraItems: [buildEditMenu()])
        // Probe OS mic TCC early so the usage string from embedded Info.plist can
        // surface a System Settings prompt before CallLive getUserMedia.
        AVCaptureDevice.requestAccess(for: .audio) { _ in }
        // UNUserNotificationCenter only works from a real .app; set the delegate so
        // banners still show while the app is focused (Test Notification, etc.).
        if Bundle.main.bundleURL.pathExtension == "app" {
            let center = UNUserNotificationCenter.current()
            center.delegate = self
        }
    }

    @objc private func appReopen() {
        // Dock reopen is handled via applicationShouldHandleReopen on app delegate
    }

    func notifyReopen() {
        server.notify(method: "event.system.reopen", params: .object([:]))
    }

    func notifyOpenURL(_ url: String) {
        server.notify(method: "event.system.open_url", params: .object(["url": .string(url)]))
    }

    func notifyOpenFile(_ path: String) {
        server.notify(method: "event.system.open_file", params: .object(["path": .string(path)]))
    }

    private func clientDisconnected() {
        resetSession()
        // BEAM-first/dev (`--edw-no-beam`): the Elixir node owns the host. When it
        // disconnects the UI must go away — reconnect only makes sense when the
        // host owns BEAM and can accept a new client.
        if quitRequested || config.lifetime == .coupled || config.noBeam {
            finishQuit()
            return
        }
    }

    /// Drop all client-owned native UI. Idempotent. Does not quit the host or
    /// change BEAM respawn bookkeeping.
    func resetSession() {
        suppressTerminate = true
        for item in trays.values {
            NSStatusBar.system.removeStatusItem(item)
        }
        trays.removeAll()
        trayMenus.removeAll()

        // Do not call NSWindow.close(): last-window close can still invoke
        // applicationShouldTerminate on some runners and drop the RPC socket.
        for w in Array(windows.values) {
            w.window.delegate = nil
            w.window.orderOut(nil)
            w.window.contentView = nil
        }
        windows.removeAll()
        webviews.removeAll()

        menus.removeAll()
        menuOnclicks.removeAll()
        icons.removeAll()
        permissionPolicy.removeAll()

        if Bundle.main.bundleURL.pathExtension == "app" {
            UNUserNotificationCenter.current().removeAllDeliveredNotifications()
        }

        initialized = false
        installMainMenuPreservingApple(extraItems: [buildEditMenu()])
        DispatchQueue.main.async { [weak self] in
            self?.suppressTerminate = false
        }
    }

    /// Ask Elixir to shut down (`event.system.quit`). Used by Quit menu / Cmd+Q.
    func requestQuit() {
        guard !quitRequested else { return }
        quitRequested = true
        server.notify(method: "event.system.quit", params: .object([:]))
        // If BEAM never disconnects (already dead / hung), still exit the host.
        DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) { [weak self] in
            self?.finishQuit()
        }
    }

    func finishQuit() {
        readyToTerminate = true
        restartTimer?.cancel()
        restartTimer = nil
        // Force the host-kills-BEAM path into the "do not respawn" branch.
        expectedBeamExitUntil = Date().addingTimeInterval(3.0)
        beamProcess?.terminate()
        NSApp.reply(toApplicationShouldTerminate: true)
        NSApp.terminate(nil)
    }

    func nextId(_ prefix: String) -> String {
        idCounter += 1
        return "\(prefix)\(idCounter)"
    }

    private func spawnBeam() {
        let root = config.resourcesRoot()
        let beamDir = config.beamPath.map { ($0 as NSString).isAbsolutePath ? $0 : (root as NSString).appendingPathComponent($0) }
            ?? (root as NSString).appendingPathComponent("beam")
        let appName = config.beamApp ?? firstBin(in: (beamDir as NSString).appendingPathComponent("bin"))
        guard let appName else {
            fputs("edw: no beam app_name and no bin script found in \(beamDir)\n", stderr)
            return
        }
        let script = (beamDir as NSString).appendingPathComponent("bin/\(appName)")
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: script)
        proc.arguments = config.beamArgs + config.forwardedArgv
        var env = ProcessInfo.processInfo.environment
        env["EDW_PORT"] = "\(server.port)"
        env["EDW_HOST"] = config.host
        for (k, v) in config.extraEnv { env[k] = v }
        proc.environment = env
        let wd = config.beamWorkingDir.map {
            ($0 as NSString).isAbsolutePath ? $0 : (root as NSString).appendingPathComponent($0)
        } ?? beamDir
        proc.currentDirectoryURL = URL(fileURLWithPath: wd)
        proc.terminationHandler = { [weak self] _ in
            DispatchQueue.main.async { self?.beamDidExit() }
        }
        do {
            try proc.run()
            beamProcess = proc
        } catch {
            fputs("edw: failed to spawn beam: \(error)\n", stderr)
        }
    }

    /// Handle BEAM exit while the host is still alive. Decide whether to respawn
    /// based on whether the exit looked intentional (`system.prepare_quit`)
    /// and whether we have a maximum-attempts budget left.
    private func beamDidExit() {
        resetSession()
        beamProcess = nil
        restartTimer?.cancel()
        restartTimer = nil
        if quitRequested {
            // Host initiated the quit; do not respawn.
            return
        }
        if let until = expectedBeamExitUntil, until > Date() {
            // Elixir sent `system.prepare_quit` and exited within the window.
            // Treat as a clean shutdown.
            return
        }
        if !config.restartBeam {
            return
        }
        if config.restartMaxAttempts > 0, beamRestartAttempts >= config.restartMaxAttempts {
            fputs("edw: beam exited; restart limit reached, terminating host\n", stderr)
            NSApp.terminate(nil)
            return
        }
        beamRestartAttempts += 1
        let shift = min(beamRestartAttempts - 1, 4)
        let multiplier = UInt32(1 << shift)
        let backoff = min(config.restartBackoffMs * multiplier, 5_000)
        let timer = DispatchSource.makeTimerSource(queue: .main)
        timer.schedule(deadline: .now() + .milliseconds(Int(backoff)))
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            self.restartTimer = nil
            if self.config.beamEnabled && !self.config.noBeam {
                self.spawnBeam()
            }
        }
        restartTimer = timer
        timer.resume()
    }

    private func firstBin(in dir: String) -> String? {
        guard let files = try? FileManager.default.contentsOfDirectory(atPath: dir) else { return nil }
        return files.sorted().first { !$0.hasPrefix(".") && !$0.hasSuffix(".bat") }
    }

    func notifyCloseRequested(windowId: String) {
        server.notify(method: "event.window.close_requested", params: .object(["window_id": .string(windowId)]))
    }

    func openExternal(_ url: String) {
        if let u = URL(string: url) {
            NSWorkspace.shared.open(u)
        }
    }

    func handlePermissionRequest(
        origin: String,
        type: String,
        webviewId: String,
        decisionHandler: @escaping (WKPermissionDecision) -> Void
    ) {
        let policy = permissionPolicy[origin]?[type] ?? "ask"
        switch policy {
        case "allow":
            decisionHandler(.grant)
            return
        case "deny":
            decisionHandler(.deny)
            return
        default:
            break
        }
        server.request(
            method: "permission.request",
            params: .object([
                "origin": .string(origin),
                "type": .string(type),
                "webview_id": .string(webviewId)
            ])
        ) { result in
            let decision = result?["decision"]?.stringValue ?? "ask"
            DispatchQueue.main.async {
                switch decision {
                case "allow": decisionHandler(.grant)
                case "deny": decisionHandler(.deny)
                default: decisionHandler(.prompt)
                }
            }
        }
    }

    // MARK: - RPC dispatch

    private func handle(_ req: JSONRPC.Request, reply: @escaping (JSONRPC.Response) -> Void) {
        let method = req.method
        let params = req.params

        if method.hasPrefix("test.") {
            guard config.testRPC else {
                reply(.fail(id: req.id, code: -32003, message: "Test RPC disabled"))
                return
            }
            if let response = handleTest(method, params: params, id: req.id) {
                reply(response)
            }
            return
        }

        if method != "initialize" && !initialized {
            reply(.fail(id: req.id, code: -32001, message: "Not initialized"))
            return
        }

        do {
            let result = try dispatch(method, params: params)
            reply(.ok(id: req.id, result: result))
        } catch let e as HostError {
            reply(.fail(id: req.id, code: e.code, message: e.message))
        } catch {
            reply(.fail(id: req.id, code: -32603, message: error.localizedDescription))
        }
    }

    private func dispatch(_ method: String, params: JSONValue?) throws -> JSONValue {
        switch method {
        case "initialize":
            resetSession()
            initialized = true
            return .object([
                "protocol_version": .number(1),
                "platform": .string("macos"),
                "capabilities": .object([
                    "window": .bool(true),
                    "webview": .bool(true),
                    "menu": .bool(true),
                    "tray": .bool(true),
                    "notification": .bool(true),
                    "permission": .bool(true),
                    "media": .bool(true),
                    "test_rpc": .bool(config.testRPC)
                ])
            ])

        case "window.open":
            return try windowOpen(params)
        case "window.close", "window.destroy":
            return try windowClose(params)
        case "window.show":
            return try windowShow(params, show: params?["show"]?.boolValue ?? true)
        case "window.hide":
            return try windowShow(params, show: false)
        case "window.set_title":
            let w = try win(params)
            w.window.title = params?["title"]?.stringValue ?? ""
            return .bool(true)
        case "window.set_min_size":
            let w = try win(params)
            let width = CGFloat(params?["width"]?.intValue ?? 0)
            let height = CGFloat(params?["height"]?.intValue ?? 0)
            w.window.contentMinSize = NSSize(width: width, height: height)
            return .bool(true)
        case "window.set_icon":
            let w = try win(params)
            if let iconId = params?["icon_id"]?.stringValue, let img = icons[iconId] {
                w.window.standardWindowButton(.documentIconButton)?.image = img
                NSApp.applicationIconImage = img
            }
            return .bool(true)
        case "window.set_menubar":
            let w = try win(params)
            // No menu_id means "don't touch the main menu"; the Edit menu
            // installed at start() (or by set_menubar earlier) remains active.
            guard let menuId = params?["menu_id"]?.stringValue, let menu = menus[menuId] else {
                return .bool(true)
            }
            // AppKit always titles the *first* main-menu item with the process
            // name. Keep the application (Apple) menu first, then the default
            // Edit menu (so Cmd+C/V/X/A accelerators remain active), then the
            // user-supplied menubar items. Installing the Edit menu from a copy
            // is safe — each window gets its own mainMenu instance.
            let bar = NSMenu()
            if let apple = ensureAppleMenuItem() {
                apple.menu?.removeItem(apple)
                bar.addItem(apple)
            }
            bar.addItem(buildEditMenu())
            for item in menu.items {
                bar.addItem(item.copy() as! NSMenuItem)
            }
            w.window.menu = bar
            NSApp.mainMenu = bar
            return .bool(true)
        case "window.iconize":
            let w = try win(params)
            if params?["iconize"]?.boolValue == true {
                w.window.miniaturize(nil)
            } else {
                w.window.deminiaturize(nil)
            }
            return .bool(true)
        case "window.shown":
            let w = try win(params)
            return .bool(w.window.isVisible)
        case "window.active":
            let w = try win(params)
            return .bool(w.window.isKeyWindow)
        case "window.raise":
            let w = try win(params)
            NSApp.activate(ignoringOtherApps: true)
            w.window.makeKeyAndOrderFront(nil)
            return .bool(true)
        case "window.close_veto":
            _ = try win(params)
            return .bool(true)

        case "webview.load_url":
            let wv = try web(params)
            guard let url = params?["url"]?.stringValue else { throw HostError(-32602, "url required") }
            wv.loadURL(url)
            return .bool(true)
        case "webview.reload":
            try web(params).webView.reload()
            return .bool(true)
        case "webview.current_url":
            let w = try web(params)
            return .string(w.webView.url?.absoluteString ?? w.lastURL ?? "")
        case "webview.rebuild":
            let w = try win(params)
            let newId = w.rebuildWebView()
            webviews = webviews.filter { $0.value != w.windowId }
            webviews[newId] = w.windowId
            return .object(["webview_id": .string(newId)])
        case "webview.set_context_menu":
            try web(params).setContextMenuEnabled(params?["enabled"]?.boolValue ?? false)
            return .bool(true)

        case "menu.create":
            return try menuCreate(params)
        case "menu.update":
            return try menuUpdate(params)
        case "menu.destroy":
            if let id = params?["menu_id"]?.stringValue {
                menus.removeValue(forKey: id)
                menuOnclicks.removeValue(forKey: id)
            }
            return .bool(true)
        case "menu.set_apple":
            return setAppleMenu(params)

        case "tray.create":
            return trayCreate(params)
        case "tray.set_icon":
            return traySetIcon(params)
        case "tray.set_menu":
            return traySetMenu(params)
        case "tray.destroy":
            if let id = params?["tray_id"]?.stringValue, let item = trays.removeValue(forKey: id) {
                trayMenus = trayMenus.filter { $0.value != id }
                NSStatusBar.system.removeStatusItem(item)
            }
            return .bool(true)

        case "notification.show":
            return notificationShow(params)
        case "notification.close":
            if let id = params?["notification_id"]?.stringValue {
                if Bundle.main.bundleIdentifier != nil {
                    UNUserNotificationCenter.current().removeDeliveredNotifications(withIdentifiers: [id])
                }
            }
            return .bool(true)

        case "icon.create":
            return try iconCreate(params)
        case "icon.destroy":
            if let id = params?["icon_id"]?.stringValue { icons.removeValue(forKey: id) }
            return .bool(true)

        case "system.open_url":
            if let url = params?["url"]?.stringValue { openExternal(url) }
            return .bool(true)
        case "system.locale":
            return .string(Locale.current.identifier.lowercased())
        case "system.os_description":
            let v = ProcessInfo.processInfo.operatingSystemVersionString
            return .string("macOS \(v)")
        case "system.prepare_quit":
            // Elixir is about to halt; mark so the upcoming BEAM exit is treated
            // as a clean shutdown (no host-driven respawn).
            quitRequested = true
            expectedBeamExitUntil = Date().addingTimeInterval(3.0)
            return .bool(true)
        case "system.set_permission_policy":
            guard let origin = params?["origin"]?.stringValue else { throw HostError(-32602, "origin") }
            var map = permissionPolicy[origin] ?? [:]
            if let c = params?["camera"]?.stringValue { map["camera"] = c }
            if let m = params?["microphone"]?.stringValue { map["microphone"] = m }
            permissionPolicy[origin] = map
            return .bool(true)

        case "dialog.choose_file":
            return dialogChoose(params, directories: false)
        case "dialog.choose_directory":
            return dialogChoose(params, directories: true)
        case "dialog.prompt":
            return dialogPrompt(params)

        default:
            throw HostError(-32601, "Method not found: \(method)")
        }
    }

    private func dialogChoose(_ params: JSONValue?, directories: Bool) -> JSONValue {
        let panel = NSOpenPanel()
        panel.canChooseFiles = !directories
        panel.canChooseDirectories = directories
        panel.allowsMultipleSelection = false
        panel.canCreateDirectories = directories
        if let title = params?["title"]?.stringValue {
            panel.message = title
            panel.title = title
        }
        if let path = params?["default_path"]?.stringValue, !path.isEmpty {
            panel.directoryURL = URL(fileURLWithPath: path, isDirectory: true)
        }
        let result = panel.runModal()
        if result == .OK, let url = panel.url {
            return .object(["path": .string(url.path)])
        }
        return .null
    }

    private func dialogPrompt(_ params: JSONValue?) -> JSONValue {
        let alert = NSAlert()
        alert.messageText = params?["title"]?.stringValue ?? ""
        alert.informativeText = params?["message"]?.stringValue ?? ""
        alert.addButton(withTitle: "OK")
        alert.addButton(withTitle: "Cancel")
        let field = NSTextField(frame: NSRect(x: 0, y: 0, width: 280, height: 24))
        field.stringValue = params?["default_value"]?.stringValue ?? ""
        alert.accessoryView = field
        let response = alert.runModal()
        if response == .alertFirstButtonReturn {
            return .object(["value": .string(field.stringValue)])
        }
        return .null
    }

    private func handleTest(_ method: String, params: JSONValue?, id: JSONValue?) -> JSONRPC.Response? {
        switch method {
        case "test.ping":
            return .ok(id: id, result: .string("pong"))
        case "test.echo":
            return .ok(id: id, result: params ?? .null)
        case "test.capabilities":
            return .ok(id: id, result: .object([
                "window": .bool(true),
                "webview": .bool(true),
                "menu": .bool(true),
                "tray": .bool(true),
                "notification": .bool(true),
                "permission": .bool(true),
                "media": .bool(true),
                "test_rpc": .bool(true)
            ]))
        case "test.window.list":
            let list: [JSONValue] = windows.values.map { w in
                .object([
                    "window_id": .string(w.windowId),
                    "webview_id": .string(w.webviewId),
                    "title": .string(w.window.title),
                    "url": .string(w.lastURL ?? w.webView.url?.absoluteString ?? "")
                ])
            }
            return .ok(id: id, result: .array(list))
        case "test.tray.list":
            let list: [JSONValue] = trays.keys.sorted().map { id in
                .object(["tray_id": .string(id)])
            }
            return .ok(id: id, result: .array(list))
        case "test.session.reset":
            resetSession()
            return .ok(id: id, result: .bool(true))
        case "test.menu.list":
            // Snapshot the current main menu (NSApp.mainMenu) so the E2E suite
            // can assert that the host has installed a default Edit menu with
            // the expected keyboard accelerators. Items without an action (the
            // submenu root) are reported as `action = ""`.
            let items: [JSONValue] = (NSApp.mainMenu?.items ?? []).map { item in
                let sub: [JSONValue] = (item.submenu?.items ?? []).map { subItem in
                    let action = subItem.action.map(NSStringFromSelector) ?? ""
                    return .object([
                        "label": .string(subItem.title),
                        "key": .string(subItem.keyEquivalent),
                        "modifiers": .number(Double(subItem.keyEquivalentModifierMask.rawValue)),
                        "action": .string(action)
                    ])
                }
                return .object([
                    "title": .string(item.title),
                    "items": .array(sub)
                ])
            }
            return .ok(id: id, result: .array(items))
        case "test.webview.eval":
            guard let wvId = params?["webview_id"]?.stringValue,
                  let script = params?["script"]?.stringValue,
                  let winId = webviews[wvId],
                  let w = windows[winId]
            else {
                return .fail(id: id, code: -32002, message: "unknown webview")
            }
            w.webView.evaluateJavaScript(script) { [weak self] result, error in
                let out: JSONValue
                if let error {
                    out = .object(["error": .string(error.localizedDescription)])
                } else if let s = result as? String {
                    out = .string(s)
                } else if let b = result as? Bool {
                    out = .bool(b)
                } else if let n = result as? NSNumber {
                    out = .number(n.doubleValue)
                } else if result == nil {
                    out = .null
                } else {
                    out = .string(String(describing: result!))
                }
                self?.server.send(.ok(id: id, result: out))
            }
            return nil
        case "test.permission.simulate":
            let origin = params?["origin"]?.stringValue ?? "http://127.0.0.1"
            let type = params?["type"]?.stringValue ?? "microphone"
            let wv = params?["webview_id"]?.stringValue ?? windows.values.first?.webviewId ?? ""
            server.request(
                method: "permission.request",
                params: .object([
                    "origin": .string(origin),
                    "type": .string(type),
                    "webview_id": .string(wv)
                ])
            ) { _ in }
            return .ok(id: id, result: .bool(true))
        case "test.disconnect":
            server.closeConnection()
            return .ok(id: id, result: .bool(true))
        case "test.crash":
            exit(2)
        default:
            return .fail(id: id, code: -32601, message: "Unknown test method")
        }
    }

    // MARK: - helpers

    private func win(_ params: JSONValue?) throws -> WebWindowController {
        guard let id = params?["window_id"]?.stringValue, let w = windows[id] else {
            throw HostError(-32002, "unknown window")
        }
        return w
    }

    private func web(_ params: JSONValue?) throws -> WebWindowController {
        guard let id = params?["webview_id"]?.stringValue,
              let winId = webviews[id],
              let w = windows[winId]
        else {
            // also allow window_id for rebuild path
            if params?["window_id"] != nil { return try win(params) }
            throw HostError(-32002, "unknown webview")
        }
        return w
    }

    private func windowOpen(_ params: JSONValue?) throws -> JSONValue {
        let title = params?["title"]?.stringValue ?? "Desktop"
        let width = CGFloat(params?["width"]?.intValue ?? 800)
        let height = CGFloat(params?["height"]?.intValue ?? 600)
        let windowId = nextId("w")
        let webviewId = nextId("v")
        let ctrl = WebWindowController(
            windowId: windowId,
            webviewId: webviewId,
            title: title,
            width: width,
            height: height,
            host: self
        )
        if let mw = params?["min_width"]?.intValue, let mh = params?["min_height"]?.intValue {
            ctrl.window.contentMinSize = NSSize(width: mw, height: mh)
        }
        if let iconId = params?["icon_id"]?.stringValue, let img = icons[iconId] {
            NSApp.applicationIconImage = img
        }
        windows[windowId] = ctrl
        webviews[webviewId] = windowId
        ctrl.window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        return .object(["window_id": .string(windowId), "webview_id": .string(webviewId)])
    }

    private func windowClose(_ params: JSONValue?) throws -> JSONValue {
        let w = try win(params)
        w.window.delegate = nil
        w.window.close()
        webviews = webviews.filter { $0.value != w.windowId }
        windows.removeValue(forKey: w.windowId)
        return .bool(true)
    }

    private func windowShow(_ params: JSONValue?, show: Bool) throws -> JSONValue {
        let w = try win(params)
        if show {
            w.window.makeKeyAndOrderFront(nil)
        } else {
            w.window.orderOut(nil)
        }
        return .bool(true)
    }

    private func menuCreate(_ params: JSONValue?) throws -> JSONValue {
        let id = nextId("m")
        let dom = params?["dom"]
        var map: [Int: String] = [:]
        let menu = buildMenu(dom: dom, onclicks: &map, menuId: id)
        menus[id] = menu
        menuOnclicks[id] = map
        return .object(["menu_id": .string(id)])
    }

    private func menuUpdate(_ params: JSONValue?) throws -> JSONValue {
        guard let id = params?["menu_id"]?.stringValue else { throw HostError(-32602, "menu_id") }
        var map: [Int: String] = [:]
        let menu = buildMenu(dom: params?["dom"], onclicks: &map, menuId: id)
        menus[id] = menu
        menuOnclicks[id] = map
        // Desktop.Menu mounts with an empty DOM then updates — rebind trays that
        // still hold the previous NSMenu instance.
        if let trayId = trayMenus[id], let item = trays[trayId] {
            item.menu = menu
        }
        return .bool(true)
    }

    private func buildMenu(dom: JSONValue?, onclicks: inout [Int: String], menuId: String) -> NSMenu {
        let menu = NSMenu()
        guard let root = dom else { return menu }
        let children: [JSONValue]
        if let tag = root["tag"]?.stringValue, tag == "menubar" || tag == "menu" {
            children = {
                if case .array(let a) = root["children"] { return a }
                return []
            }()
        } else if case .array(let a) = root {
            children = a
        } else {
            children = []
        }
        for child in children {
            if let item = buildMenuItem(child, onclicks: &onclicks, menuId: menuId) {
                menu.addItem(item)
            }
        }
        return menu
    }

    private func buildMenuItem(_ node: JSONValue, onclicks: inout [Int: String], menuId: String) -> NSMenuItem? {
        guard let tag = node["tag"]?.stringValue else { return nil }
        if tag == "hr" {
            return NSMenuItem.separator()
        }
        let attrs = node["attrs"]?.objectValue ?? [:]
        let label: String = {
            if case .array(let kids) = node["children"], let first = kids.first, case .string(let s) = first {
                return s
            }
            return attrs["label"]?.stringValue ?? ""
        }()
        if tag == "menu" {
            let item = NSMenuItem(title: attrs["label"]?.stringValue ?? label, action: nil, keyEquivalent: "")
            var subMap = onclicks
            let submenu = buildMenu(dom: node, onclicks: &subMap, menuId: menuId)
            onclicks.merge(subMap) { _, b in b }
            item.submenu = submenu
            return item
        }
        if tag == "item" {
            let item = NSMenuItem(title: label, action: #selector(menuClicked(_:)), keyEquivalent: "")
            item.target = self
            let tagId = nextId("t").hashValue & 0x7fffffff
            item.tag = tagId
            item.representedObject = menuId
            if let onclick = attrs["onclick"]?.stringValue {
                onclicks[tagId] = onclick
            }
            return item
        }
        return nil
    }

    @objc private func menuClicked(_ sender: NSMenuItem) {
        let menuId = (sender.representedObject as? String) ?? ""
        let onclick = menuOnclicks[menuId]?[sender.tag] ?? ""
        server.notify(
            method: "event.menu.click",
            params: .object(["menu_id": .string(menuId), "onclick": .string(onclick)])
        )
    }

    private func setAppleMenu(_ params: JSONValue?) -> JSONValue {
        let name = params?["app_name"]?.stringValue ?? "App"
        // Secondary windows (e.g. CallWindow) also call menu.set_apple; keep the
        // first product name so the menubar / process name are not overwritten.
        if appleMenuSet {
            return .bool(true)
        }
        appDisplayName = name
        // Without an .app bundle, AppKit uses the executable name ("DesktopWebView")
        // for the application menu title. Align the process name with the product.
        ProcessInfo.processInfo.processName = name

        _ = ensureAppleMenuItem()
        installMainMenuPreservingApple(extraItems: Array(NSApp.mainMenu?.items.dropFirst() ?? []))
        appleMenuSet = true
        return .bool(true)
    }

    /// Default Edit menu with the standard macOS keyboard accelerators. The
    /// actions are wired to the responder chain (`target = nil`), so WKWebView's
    /// internal text input views implement them and receive `copy:` / `cut:` /
    /// `paste:` / `selectAll:` etc. when the user presses Cmd+C/V/X/A. Without
    /// this menu installed, `performKeyEquivalent` never fires the selectors.
    private func buildEditMenu() -> NSMenuItem {
        let menu = NSMenu(title: "Edit")
        let cmd: NSEvent.ModifierFlags = .command

        let undo = NSMenuItem(title: "Undo", action: #selector(UndoManager.undo), keyEquivalent: "z")
        undo.keyEquivalentModifierMask = cmd
        undo.target = nil
        menu.addItem(undo)

        let redo = NSMenuItem(title: "Redo", action: #selector(UndoManager.redo), keyEquivalent: "z")
        redo.keyEquivalentModifierMask = cmd.union(.shift)
        redo.target = nil
        menu.addItem(redo)

        menu.addItem(NSMenuItem.separator())

        let cut = NSMenuItem(title: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        cut.keyEquivalentModifierMask = cmd
        cut.target = nil
        menu.addItem(cut)

        let copy = NSMenuItem(title: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        copy.keyEquivalentModifierMask = cmd
        copy.target = nil
        menu.addItem(copy)

        let paste = NSMenuItem(title: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        paste.keyEquivalentModifierMask = cmd
        paste.target = nil
        menu.addItem(paste)

        let delete = NSMenuItem(title: "Delete", action: #selector(NSText.delete(_:)), keyEquivalent: String(Character(UnicodeScalar(NSBackspaceCharacter)!)))
        delete.target = nil
        menu.addItem(delete)

        let selectAll = NSMenuItem(title: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        selectAll.keyEquivalentModifierMask = cmd
        selectAll.target = nil
        menu.addItem(selectAll)

        let editItem = NSMenuItem(title: "Edit", action: nil, keyEquivalent: "")
        editItem.submenu = menu
        return editItem
    }

    /// Application menu (About / Quit). Always the first main-menu item on macOS.
    private func ensureAppleMenuItem() -> NSMenuItem? {
        let name = appDisplayName
        let appMenu = NSMenu(title: name)
        appMenu.addItem(withTitle: "About \(name)", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        appMenu.addItem(NSMenuItem.separator())
        appMenu.addItem(withTitle: "Quit \(name)", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")

        let appItem = appleMenuItem ?? NSMenuItem(title: name, action: nil, keyEquivalent: "")
        appItem.title = name
        appItem.submenu = appMenu
        appleMenuItem = appItem
        return appItem
    }

    private func installMainMenuPreservingApple(extraItems: [NSMenuItem]) {
        let bar = NSMenu()
        if let apple = ensureAppleMenuItem() {
            // Detach from previous menu before re-adding.
            apple.menu?.removeItem(apple)
            bar.addItem(apple)
        }
        for item in extraItems {
            item.menu?.removeItem(item)
            bar.addItem(item)
        }
        NSApp.mainMenu = bar
    }

    private func trayCreate(_ params: JSONValue?) -> JSONValue {
        let id = nextId("tray")
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let iconId = params?["icon_id"]?.stringValue, let img = icons[iconId] {
            item.button?.image = prepareTrayImage(img)
        } else {
            item.button?.title = "EDW"
        }
        if let menuId = params?["menu_id"]?.stringValue, let menu = menus[menuId] {
            item.menu = menu
            trayMenus[menuId] = id
        }
        trays[id] = item
        return .object(["tray_id": .string(id)])
    }

    private func traySetIcon(_ params: JSONValue?) -> JSONValue {
        guard let id = params?["tray_id"]?.stringValue, let item = trays[id] else {
            return .bool(false)
        }
        if let iconId = params?["icon_id"]?.stringValue, let img = icons[iconId] {
            // Keep full-color app icons (Diode paints status colors into PNGs).
            // Do not set isTemplate — that forces a gray menu-bar silhouette.
            item.button?.image = prepareTrayImage(img)
            item.button?.title = ""
        }
        return .bool(true)
    }

    /// PNG pixel size becomes NSImage point size by default (e.g. 32pt), which is
    /// oversized next to other menu-bar icons. Force a status-item scale (~80% of
    /// bar thickness ≈ 18pt on a 22pt bar).
    private func prepareTrayImage(_ img: NSImage) -> NSImage {
        let copy = img.copy() as! NSImage
        let side = max(14.0, NSStatusBar.system.thickness * 0.8)
        copy.size = NSSize(width: side, height: side)
        return copy
    }

    private func traySetMenu(_ params: JSONValue?) -> JSONValue {
        guard let id = params?["tray_id"]?.stringValue, let item = trays[id] else {
            return .bool(false)
        }
        if let menuId = params?["menu_id"]?.stringValue, let menu = menus[menuId] {
            item.menu = menu
            trayMenus[menuId] = id
        }
        return .bool(true)
    }

    private func notificationShow(_ params: JSONValue?) -> JSONValue {
        let id = params?["id"]?.stringValue ?? nextId("n")
        let title = params?["title"]?.stringValue ?? ""
        let message = params?["message"]?.stringValue ?? ""
        // UNUserNotificationCenter only delivers from a real .app bundle. Local
        // `./run` hosts are bare binaries, so fall back to AppleScript.
        if Bundle.main.bundleURL.pathExtension == "app" {
            let content = UNMutableNotificationContent()
            content.title = title
            content.body = message
            let req = UNNotificationRequest(identifier: id, content: content, trigger: nil)
            let center = UNUserNotificationCenter.current()
            center.delegate = self
            center.getNotificationSettings { settings in
                switch settings.authorizationStatus {
                case .authorized, .provisional, .ephemeral:
                    center.add(req, withCompletionHandler: nil)
                case .notDetermined:
                    center.requestAuthorization(options: [.alert, .sound]) { granted, _ in
                        if granted {
                            center.add(req, withCompletionHandler: nil)
                        } else {
                            fputs("notification: authorization denied for \(title)\n", stderr)
                        }
                    }
                case .denied:
                    fputs("notification: authorization denied for \(title)\n", stderr)
                @unknown default:
                    center.requestAuthorization(options: [.alert, .sound]) { granted, _ in
                        if granted {
                            center.add(req, withCompletionHandler: nil)
                        }
                    }
                }
            }
        } else {
            showCliNotification(title: title, message: message)
        }
        return .object(["notification_id": .string(id)])
    }

    // MARK: - UNUserNotificationCenterDelegate

    func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        willPresent notification: UNNotification,
        withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void
    ) {
        // Without this, macOS suppresses banners while the app is in the foreground.
        completionHandler([.banner, .list, .sound])
    }

    func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        didReceive response: UNNotificationResponse,
        withCompletionHandler completionHandler: @escaping () -> Void
    ) {
        let id = response.notification.request.identifier
        server.notify(
            method: "event.notification.click",
            params: .object(["notification_id": .string(id)])
        )
        completionHandler()
    }

    private func showCliNotification(title: String, message: String) {
        func escape(_ s: String) -> String {
            s.replacingOccurrences(of: "\\", with: "\\\\")
                .replacingOccurrences(of: "\"", with: "\\\"")
        }
        let script =
            "display notification \"\(escape(message))\" with title \"\(escape(title))\""
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        task.arguments = ["-e", script]
        try? task.run()
        fputs("notification: \(title): \(message)\n", stderr)
    }

    private func iconCreate(_ params: JSONValue?) throws -> JSONValue {
        let id = nextId("icon")
        if let path = params?["path"]?.stringValue {
            guard let img = NSImage(contentsOfFile: path) else {
                throw HostError(-32002, "failed to load icon: \(path)")
            }
            icons[id] = img
            return .object(["icon_id": .string(id)])
        }
        if let b64 = params?["png_base64"]?.stringValue,
           let data = Data(base64Encoded: b64),
           let img = NSImage(data: data)
        {
            icons[id] = img
            return .object(["icon_id": .string(id)])
        }
        icons[id] = NSImage(size: NSSize(width: 32, height: 32))
        return .object(["icon_id": .string(id)])
    }
}

struct HostError: Error {
    let code: Int
    let message: String
    init(_ code: Int, _ message: String) {
        self.code = code
        self.message = message
    }
}
