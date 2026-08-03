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
                .linkedFramework("UserNotifications"),
                .linkedFramework("AVFoundation"),
                // Embed Info.plist so TCC can show mic/camera prompts for bare binaries.
                .unsafeFlags([
                    "-Xlinker", "-sectcreate",
                    "-Xlinker", "__TEXT",
                    "-Xlinker", "__info_plist",
                    "-Xlinker", "Info.plist"
                ])
            ]
        )
    ]
)
