import AppKit
import Foundation

final class AppDelegate: NSObject, NSApplicationDelegate {
    var host: HostController!

    func applicationDidFinishLaunching(_ notification: Notification) {}

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        host?.notifyReopen()
        return true
    }

    func application(_ application: NSApplication, open urls: [URL]) {
        for url in urls {
            if url.isFileURL {
                host?.notifyOpenFile(url.path)
            } else {
                host?.notifyOpenURL(url.absoluteString)
            }
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        // Quit menu / Cmd+Q / Dock Quit all go through terminate(_:). Notify BEAM
        // first so the Elixir process exits; only then tear down the host.
        if host.readyToTerminate {
            return .terminateNow
        }
        if host.suppressTerminate {
            return .terminateCancel
        }
        host.requestQuit()
        return .terminateLater
    }
}

let config = HostConfig.parse(argv: CommandLine.arguments)
let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate

let host = HostController(config: config)
delegate.host = host

do {
    try host.start()
} catch {
    fputs("failed to start host: \(error)\n", stderr)
    exit(1)
}

app.run()
