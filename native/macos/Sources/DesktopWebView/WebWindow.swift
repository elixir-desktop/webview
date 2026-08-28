import AppKit
import WebKit
import Foundation

final class WebWindowController: NSObject, NSWindowDelegate, WKUIDelegate, WKNavigationDelegate,
    WKScriptMessageHandler
{
    let windowId: String
    private(set) var webviewId: String
    let window: NSWindow
    private(set) var webView: WKWebView
    weak var host: HostController?
    var lastURL: String?

    init(windowId: String, webviewId: String, title: String, width: CGFloat, height: CGFloat, host: HostController) {
        self.windowId = windowId
        self.webviewId = webviewId
        self.host = host
        let rect = NSRect(x: 0, y: 0, width: width, height: height)
        window = NSWindow(
            contentRect: rect,
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        // Programmatic NSWindows default to isReleasedWhenClosed = true, which
        // races AppKit's _NSWindowTransformAnimation on activate/raise and can
        // SIGSEGV when a notification click brings the app forward.
        window.isReleasedWhenClosed = false
        window.title = title
        window.center()

        let config = WKWebViewConfiguration()
        config.preferences.isElementFullscreenEnabled = true
        // Secure localhost LiveView is a valid getUserMedia context; ensure capture
        // APIs are enabled for CallLive in secondary windows.
        config.mediaTypesRequiringUserActionForPlayback = []
        if #available(macOS 14.0, *) {
            // media capture handled via UI delegate
        }
        let wv = WKWebView(frame: rect, configuration: config)
        wv.allowsBackForwardNavigationGestures = true
        self.webView = wv
        super.init()
        window.delegate = self
        wv.uiDelegate = self
        wv.navigationDelegate = self
        window.contentView = wv
    }

    func rebuildWebView() -> String {
        let newId = host?.nextId("v") ?? webviewId
        let config = WKWebViewConfiguration()
        config.mediaTypesRequiringUserActionForPlayback = []
        let frame = webView.frame
        let wv = WKWebView(frame: frame, configuration: config)
        wv.uiDelegate = self
        wv.navigationDelegate = self
        window.contentView = wv
        webView = wv
        webviewId = newId
        if let lastURL, let url = URL(string: lastURL) {
            wv.load(URLRequest(url: url))
        }
        return newId
    }

    func loadURL(_ url: String) {
        lastURL = url
        if let u = URL(string: url) {
            webView.load(URLRequest(url: u))
        }
    }

    func setContextMenuEnabled(_ enabled: Bool) {
        // WKWebView does not expose a simple API; use menu override via willOpenMenu if needed.
        _ = enabled
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        host?.notifyCloseRequested(windowId: windowId)
        return false // veto; Elixir decides
    }

    func windowDidBecomeKey(_ notification: Notification) {
        host?.server.notify(method: "event.window.focus", params: .object(["window_id": .string(windowId)]))
    }

    func windowDidResignKey(_ notification: Notification) {
        host?.server.notify(method: "event.window.blur", params: .object(["window_id": .string(windowId)]))
    }

    func webView(
        _ webView: WKWebView,
        createWebViewWith configuration: WKWebViewConfiguration,
        for navigationAction: WKNavigationAction,
        windowFeatures: WKWindowFeatures
    ) -> WKWebView? {
        if let url = navigationAction.request.url?.absoluteString {
            host?.server.notify(
                method: "event.webview.new_window",
                params: .object(["webview_id": .string(webviewId), "url": .string(url)])
            )
            host?.openExternal(url)
        }
        return nil
    }

    func webView(
        _ webView: WKWebView,
        requestMediaCapturePermissionFor origin: WKSecurityOrigin,
        initiatedByFrame frame: WKFrameInfo,
        type: WKMediaCaptureType,
        decisionHandler: @escaping (WKPermissionDecision) -> Void
    ) {
        let permType: String
        switch type {
        case .camera: permType = "camera"
        case .microphone: permType = "microphone"
        case .cameraAndMicrophone: permType = "camera"
        @unknown default: permType = "microphone"
        }
        let originStr = "\(origin.protocol)://\(origin.host):\(origin.port)"
        host?.handlePermissionRequest(
            origin: originStr,
            type: permType,
            webviewId: webviewId,
            decisionHandler: decisionHandler
        )
    }

    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
        host?.server.notify(
            method: "event.webview.error",
            params: .object([
                "webview_id": .string(webviewId),
                "message": .string(error.localizedDescription)
            ])
        )
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        host?.server.notify(
            method: "event.webview.finished",
            params: .object([
                "webview_id": .string(webviewId),
                "url": .string(webView.url?.absoluteString ?? "")
            ])
        )
    }

    func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {}
}
