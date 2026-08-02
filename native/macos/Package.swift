// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "DesktopWebView",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(
            name: "DesktopWebView",
            path: "Sources/DesktopWebView",
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("WebKit"),
                .linkedFramework("UserNotifications")
            ]
        )
    ]
)
