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

    private func acceptsFileDrop(_ sender: NSDraggingInfo) -> Bool {
        let types = sender.draggingPasteboard.types ?? []
        return types.contains(.fileURL) ||
            types.contains { Self.fileDragTypes.contains($0) }
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        let operation = super.draggingEntered(sender)
        return acceptsFileDrop(sender) && operation.isEmpty ? .copy : operation
    }

    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        let operation = super.draggingUpdated(sender)
        return acceptsFileDrop(sender) && operation.isEmpty ? .copy : operation
    }

    override func prepareForDragOperation(_ sender: NSDraggingInfo) -> Bool {
        super.prepareForDragOperation(sender) || acceptsFileDrop(sender)
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        super.performDragOperation(sender)
    }

    override func draggingExited(_ sender: NSDraggingInfo?) {
        super.draggingExited(sender)
    }
}
