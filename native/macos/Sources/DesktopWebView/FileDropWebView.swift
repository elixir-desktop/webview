import AppKit
import WebKit

final class FileDropWebView: WKWebView {
    private static let fileDragTypes =
        [
            NSPasteboard.PasteboardType.fileURL,
            NSPasteboard.PasteboardType("NSFilenamesPboardType")
        ] +
        NSFilePromiseReceiver.readableDraggedTypes.map {
            NSPasteboard.PasteboardType($0)
        }

    override init(frame: NSRect, configuration: WKWebViewConfiguration) {
        super.init(frame: frame, configuration: configuration)
        registerForDraggedTypes(Self.fileDragTypes)
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        registerForDraggedTypes(Self.fileDragTypes)
    }
}
