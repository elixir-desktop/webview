import AppKit
import Foundation
import UserNotifications
import WebKit

final class HostController: NSObject {
    let config: HostConfig
    let server = RPCServer()
    private var initialized = false
    private var idCounter = 0
    private var windows: [String: WebWindowController] = [:]
    private var webviews: [String: String] = [:] // webviewId -> windowId
    private var menus: [String: NSMenu] = [:]
    private var menuOnclicks: [String: [Int: String]] = [:] // menuId -> tag -> onclick
    private var trays: [String: NSStatusItem] = [:]
    private var icons: [String: NSImage] = [:]
    private var permissionPolicy: [String: [String: String]] = [:] // origin -> type -> allow|deny|ask
    private var beamProcess: Process?
    private var appleMenuSet = false

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
        // UNUserNotificationCenter requires a real app bundle; defer until notification.show.
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
        if config.lifetime == .coupled {
            beamProcess?.terminate()
            NSApp.terminate(nil)
        }
        // reconnect: keep windows; client will re-initialize
        initialized = false
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
        do {
            try proc.run()
            beamProcess = proc
        } catch {
            fputs("edw: failed to spawn beam: \(error)\n", stderr)
        }
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
            if let menuId = params?["menu_id"]?.stringValue, let menu = menus[menuId] {
                // Convert popup-style menu into menubar if needed
                let bar = NSMenu()
                for item in menu.items {
                    bar.addItem(item.copy() as! NSMenuItem)
                }
                w.window.menu = bar
                NSApp.mainMenu = bar
            }
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
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About \(name)", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        appMenu.addItem(NSMenuItem.separator())
        appMenu.addItem(withTitle: "Quit \(name)", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        let bar = NSApp.mainMenu ?? NSMenu()
        if bar.items.first?.submenu == nil || !appleMenuSet {
            let appItem = NSMenuItem()
            appItem.submenu = appMenu
            if bar.items.isEmpty {
                bar.addItem(appItem)
            } else {
                bar.insertItem(appItem, at: 0)
            }
            NSApp.mainMenu = bar
            appleMenuSet = true
        }
        return .bool(true)
    }

    private func trayCreate(_ params: JSONValue?) -> JSONValue {
        let id = nextId("tray")
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let iconId = params?["icon_id"]?.stringValue, let img = icons[iconId] {
            item.button?.image = img
        } else {
            item.button?.title = "EDW"
        }
        if let menuId = params?["menu_id"]?.stringValue, let menu = menus[menuId] {
            item.menu = menu
        }
        trays[id] = item
        return .object(["tray_id": .string(id)])
    }

    private func traySetIcon(_ params: JSONValue?) -> JSONValue {
        guard let id = params?["tray_id"]?.stringValue, let item = trays[id] else {
            return .bool(false)
        }
        if let iconId = params?["icon_id"]?.stringValue, let img = icons[iconId] {
            item.button?.image = img
        }
        return .bool(true)
    }

    private func traySetMenu(_ params: JSONValue?) -> JSONValue {
        guard let id = params?["tray_id"]?.stringValue, let item = trays[id] else {
            return .bool(false)
        }
        if let menuId = params?["menu_id"]?.stringValue, let menu = menus[menuId] {
            item.menu = menu
        }
        return .bool(true)
    }

    private func notificationShow(_ params: JSONValue?) -> JSONValue {
        let id = params?["id"]?.stringValue ?? nextId("n")
        let title = params?["title"]?.stringValue ?? ""
        let message = params?["message"]?.stringValue ?? ""
        let bundled = Bundle.main.bundleURL.pathExtension == "app"
        if bundled {
            let content = UNMutableNotificationContent()
            content.title = title
            content.body = message
            let req = UNNotificationRequest(identifier: id, content: content, trigger: nil)
            let center = UNUserNotificationCenter.current()
            center.requestAuthorization(options: [.alert, .sound]) { _, _ in
                center.add(req, withCompletionHandler: nil)
            }
        } else {
            fputs("notification: \(title): \(message)\n", stderr)
        }
        return .object(["notification_id": .string(id)])
    }

    private func iconCreate(_ params: JSONValue?) throws -> JSONValue {
        let id = nextId("icon")
        if let path = params?["path"]?.stringValue, let img = NSImage(contentsOfFile: path) {
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
