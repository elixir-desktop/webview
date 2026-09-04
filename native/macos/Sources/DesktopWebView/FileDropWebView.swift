import AppKit
import ObjectiveC.runtime
import WebKit

private final class FileDropBridge: NSObject {
    @objc(edw_draggingEntered:)
    dynamic func edw_draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        let view = self as AnyObject as! NSView
        guard let webView = view.enclosingFileDropWebView else {
            return view.edw_originalDraggingEntered(sender)
        }

        let acceptsFileDrop = webView.acceptsFileDrop(sender)
        let operation = view.edw_originalDraggingEntered(sender)
        return webView.fileDropOperation(operation, accepted: acceptsFileDrop)
    }

    @objc(edw_draggingUpdated:)
    dynamic func edw_draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        let view = self as AnyObject as! NSView
        guard let webView = view.enclosingFileDropWebView else {
            return view.edw_originalDraggingUpdated(sender)
        }

        let acceptsFileDrop = webView.acceptsFileDrop(sender)
        let operation = view.edw_originalDraggingUpdated(sender)
        return webView.fileDropOperation(operation, accepted: acceptsFileDrop)
    }

    @objc(edw_prepareForDragOperation:)
    dynamic func edw_prepareForDragOperation(_ sender: NSDraggingInfo) -> Bool {
        let view = self as AnyObject as! NSView
        guard let webView = view.enclosingFileDropWebView else {
            return view.edw_originalPrepareForDragOperation(sender)
        }

        let acceptsFileDrop = webView.acceptsFileDrop(sender)
        return view.edw_originalPrepareForDragOperation(sender) || acceptsFileDrop
    }

    @objc(edw_performDragOperation:)
    dynamic func edw_performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        let view = self as AnyObject as! NSView
        return view.edw_originalPerformDragOperation(sender)
    }

    @objc(edw_draggingExited:)
    dynamic func edw_draggingExited(_ sender: NSDraggingInfo?) {
        let view = self as AnyObject as! NSView
        view.edw_originalDraggingExited(sender)
    }

    static func install(on view: NSView) {
        let targetClass = type(of: view)
        let classId = ObjectIdentifier(targetClass)

        guard !bridgedClasses.contains(classId) else {
            return
        }

        var installed = false
        installed =
            install(
                on: targetClass,
                selector: #selector(NSView.draggingEntered(_:)),
                originalSelector: #selector(NSView.edw_originalDraggingEntered(_:)),
                replacementSelector: #selector(FileDropBridge.edw_draggingEntered(_:))
            ) || installed
        installed =
            install(
                on: targetClass,
                selector: #selector(NSView.draggingUpdated(_:)),
                originalSelector: #selector(NSView.edw_originalDraggingUpdated(_:)),
                replacementSelector: #selector(FileDropBridge.edw_draggingUpdated(_:))
            ) || installed
        installed =
            install(
                on: targetClass,
                selector: #selector(NSView.prepareForDragOperation(_:)),
                originalSelector: #selector(NSView.edw_originalPrepareForDragOperation(_:)),
                replacementSelector: #selector(FileDropBridge.edw_prepareForDragOperation(_:))
            ) || installed
        installed =
            install(
                on: targetClass,
                selector: #selector(NSView.performDragOperation(_:)),
                originalSelector: #selector(NSView.edw_originalPerformDragOperation(_:)),
                replacementSelector: #selector(FileDropBridge.edw_performDragOperation(_:))
            ) || installed
        installed =
            install(
                on: targetClass,
                selector: #selector(NSView.draggingExited(_:)),
                originalSelector: #selector(NSView.edw_originalDraggingExited(_:)),
                replacementSelector: #selector(FileDropBridge.edw_draggingExited(_:))
            ) || installed

        if installed {
            bridgedClasses.insert(classId)
        }
    }

    private static var bridgedClasses = Set<ObjectIdentifier>()

    private static func install(
        on targetClass: AnyClass,
        selector: Selector,
        originalSelector: Selector,
        replacementSelector: Selector
    ) -> Bool {
        guard
            let method = class_getInstanceMethod(targetClass, selector),
            let replacement = class_getInstanceMethod(FileDropBridge.self, replacementSelector)
        else {
            return false
        }

        let originalImplementation = method_getImplementation(method)
        let typeEncoding = method_getTypeEncoding(method)
        class_addMethod(targetClass, selector, originalImplementation, typeEncoding)

        guard let targetMethod = class_getInstanceMethod(targetClass, selector) else {
            return false
        }

        class_addMethod(targetClass, originalSelector, originalImplementation, typeEncoding)
        method_setImplementation(targetMethod, method_getImplementation(replacement))
        return true
    }
}

private extension NSView {
    var enclosingFileDropWebView: FileDropWebView? {
        var view: NSView? = self

        while let current = view {
            if let webView = current as? FileDropWebView {
                return webView
            }

            view = current.superview
        }

        return nil
    }

    @objc(edw_originalDraggingEntered:)
    dynamic func edw_originalDraggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        []
    }

    @objc(edw_originalDraggingUpdated:)
    dynamic func edw_originalDraggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        []
    }

    @objc(edw_originalPrepareForDragOperation:)
    dynamic func edw_originalPrepareForDragOperation(_ sender: NSDraggingInfo) -> Bool {
        false
    }

    @objc(edw_originalPerformDragOperation:)
    dynamic func edw_originalPerformDragOperation(_ sender: NSDraggingInfo) -> Bool {
        false
    }

    @objc(edw_originalDraggingExited:)
    dynamic func edw_originalDraggingExited(_ sender: NSDraggingInfo?) {}
}

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
        installDragBridge()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        registerForDraggedTypes(Self.fileDragTypes)
        installDragBridge()
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        installDragBridge()
        DispatchQueue.main.async { [weak self] in
            self?.installDragBridge()
        }
    }

    fileprivate func acceptsFileDrop(_ sender: NSDraggingInfo) -> Bool {
        let types = sender.draggingPasteboard.types ?? []
        return types.contains { Self.fileDragTypes.contains($0) }
    }

    fileprivate func fileDropOperation(
        _ operation: NSDragOperation,
        accepted: Bool
    ) -> NSDragOperation {
        accepted && operation.isEmpty ? .copy : operation
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        let acceptedFileDrop = acceptsFileDrop(sender)
        return fileDropOperation(super.draggingEntered(sender), accepted: acceptedFileDrop)
    }

    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        let acceptedFileDrop = acceptsFileDrop(sender)
        return fileDropOperation(super.draggingUpdated(sender), accepted: acceptedFileDrop)
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

    private func installDragBridge() {
        for view in privateDragViews(in: self) {
            view.registerForDraggedTypes(Self.fileDragTypes)
            FileDropBridge.install(on: view)
        }
    }

    private func privateDragViews(in view: NSView) -> [NSView] {
        view.subviews.flatMap { subview in
            let nested = privateDragViews(in: subview)
            let name = NSStringFromClass(type(of: subview))

            if name.contains("WKContentView") || name.contains("WKView") {
                return [subview] + nested
            }

            return nested
        }
    }
}
