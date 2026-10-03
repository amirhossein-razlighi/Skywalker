import AppKit
import QuartzCore
import SwiftUI

/// SwiftUI wrapper around the Metal viewport.
struct ViewportView: NSViewRepresentable {
    let engine: EngineStore

    func makeNSView(context: Context) -> ViewportNSView { ViewportNSView(engine: engine) }
    func updateNSView(_ nsView: ViewportNSView, context: Context) {}
}

/// An NSView backed by a CAMetalLayer. The engine renders the frame offscreen (exactly
/// what agents capture) and presents it into this layer every display refresh.
///
/// Editing controls (familiar from DCC tools):
///   Q/W/E/R select · move · rotate · scale tool   F frame selection   ⌫ delete   ⌘D duplicate
///   LMB: select / drag gizmo handle / drag object on its ground plane (move tool)
///   RMB or ⌥LMB drag: orbit   MMB or ⇧+two-finger scroll: pan   wheel / pinch: zoom
///   two-finger scroll: orbit   hold ⌃ while dragging a handle: snap
/// While playing, keys and clicks are delivered to the game (Wander `on key` / `on click`).
final class ViewportNSView: NSView {
    private let engine: EngineStore
    private var displayLink: CADisplayLink?
    private var metalLayer: CAMetalLayer { layer as! CAMetalLayer }  // swiftlint:disable:this force_cast
    private enum DragMode { case none, orbit, pan, moveEntity, gizmo }
    private var dragMode: DragMode = .none
    private var lastPoint = CGPoint.zero
    private var tracking: NSTrackingArea?

    init(engine: EngineStore) {
        self.engine = engine
        super.init(frame: .zero)
        wantsLayer = true
        layerContentsRedrawPolicy = .never
        registerForDraggedTypes([.string])
    }

    // MARK: Drag & drop from the asset browser

    private func draggedAsset(_ info: NSDraggingInfo) -> String? {
        guard let s = info.draggingPasteboard.string(forType: .string), s.hasPrefix(EngineStore.assetDragPrefix) else { return nil }
        return String(s.dropFirst(EngineStore.assetDragPrefix.count))
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        draggedAsset(sender) != nil && editing ? .copy : []
    }

    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        draggingEntered(sender)
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        guard let path = draggedAsset(sender), editing else { return false }
        let p = convert(sender.draggingLocation, from: nil)
        let (w, h) = pixelSize
        let type = engine.call("asset_info", ["asset": .string(path)], actor: "editor").structured["type"].string ?? ""
        engine.placeAsset(path, type: type, at: (Float(p.x * scale), Float(p.y * scale), w, h))
        window?.makeFirstResponder(self)
        return true
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("not supported") }

    override func makeBackingLayer() -> CALayer {
        let l = CAMetalLayer()
        l.pixelFormat = .bgra8Unorm_srgb
        l.framebufferOnly = true
        l.maximumDrawableCount = 3
        return l
    }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        displayLink?.invalidate()  // also breaks the link's retain on self
        displayLink = nil
        guard window != nil else { return }
        updateDrawableSize()
        let link = displayLink(target: self, selector: #selector(step(_:)))
        link.add(to: .main, forMode: .common)
        displayLink = link
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let tracking { removeTrackingArea(tracking) }
        let area = NSTrackingArea(rect: bounds, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect], owner: self)
        addTrackingArea(area)
        tracking = area
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        updateDrawableSize()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        updateDrawableSize()
    }

    private var scale: CGFloat { window?.backingScaleFactor ?? 2 }
    private var pixelSize: (Int, Int) { (Int(bounds.width * scale), Int(bounds.height * scale)) }

    private func updateDrawableSize() {
        metalLayer.contentsScale = scale
        metalLayer.drawableSize = CGSize(width: max(1, bounds.width * scale), height: max(1, bounds.height * scale))
    }

    @objc private func step(_ link: CADisplayLink) {
        engine.tick()
        let (w, h) = pixelSize
        engine.render(layer: Unmanaged.passUnretained(metalLayer).toOpaque(), width: w, height: h)
    }

    // MARK: Mouse

    private func pixel(_ event: NSEvent) -> CGPoint {
        let p = convert(event.locationInWindow, from: nil)
        return CGPoint(x: p.x * scale, y: p.y * scale)
    }

    private var editing: Bool { engine.playState == "editing" }

    /// Forwards the cursor to the running game: normalized position and movement in points.
    private func forwardMouse(_ event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        let w = max(bounds.width, 1), h = max(bounds.height, 1)
        engine.mouseMove(x: Float(p.x / w), y: Float(p.y / h), dx: Float(event.deltaX), dy: Float(event.deltaY))
    }

    override func mouseMoved(with event: NSEvent) {
        if !editing {
            forwardMouse(event)
            return
        }
        let p = pixel(event)
        let (w, h) = pixelSize
        engine.gizmoHover(x: Float(p.x), y: Float(p.y), width: w, height: h)
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        let p = pixel(event)
        lastPoint = p
        let (w, h) = pixelSize
        if event.modifierFlags.contains(.option) {
            dragMode = .orbit
            return
        }
        if editing, engine.gizmoBegin(x: Float(p.x), y: Float(p.y), width: w, height: h) {
            dragMode = .gizmo
            return
        }
        let hit = engine.pick(x: Float(p.x), y: Float(p.y), width: w, height: h)
        if !editing {
            if hit != 0 { engine.click(entity: hit) }
            engine.mouseButton(0, down: true)
            dragMode = .none
            return
        }
        if hit == 0 {
            if !event.modifierFlags.contains(.shift) && !event.modifierFlags.contains(.command) { engine.selection = [] }
            dragMode = .orbit
            return
        }
        if event.modifierFlags.contains(.shift) || event.modifierFlags.contains(.command) {
            if engine.selection.contains(hit) { engine.selection.remove(hit) } else { engine.selection.insert(hit) }
            dragMode = .none
        } else {
            engine.selection = [hit]
            if engine.gizmoMode == .move {
                engine.dragBegin(entity: hit, x: Float(p.x), y: Float(p.y), width: w, height: h)
                dragMode = .moveEntity
            } else {
                dragMode = .none
            }
        }
    }

    override func mouseDragged(with event: NSEvent) {
        if !editing {
            forwardMouse(event)
            return
        }
        let p = pixel(event)
        defer { lastPoint = p }
        let (w, h) = pixelSize
        switch dragMode {
        case .orbit: engine.orbit(dx: Float(-(p.x - lastPoint.x) * 0.25), dy: Float((p.y - lastPoint.y) * 0.25))
        case .pan: engine.pan(dx: Float((p.x - lastPoint.x) / CGFloat(max(h, 1))), dy: Float((p.y - lastPoint.y) / CGFloat(max(h, 1))))
        case .moveEntity: engine.dragUpdate(x: Float(p.x), y: Float(p.y), width: w, height: h)
        case .gizmo:
            engine.gizmoDrag(x: Float(p.x), y: Float(p.y), width: w, height: h, snap: event.modifierFlags.contains(.control))
        case .none: break
        }
    }

    override func mouseUp(with event: NSEvent) {
        if !editing { engine.mouseButton(0, down: false) }
        switch dragMode {
        case .moveEntity: engine.dragEnd()
        case .gizmo: engine.gizmoEnd()
        default: break
        }
        dragMode = .none
    }

    override func rightMouseDown(with event: NSEvent) {
        if !editing {
            engine.mouseButton(1, down: true)
            return
        }
        lastPoint = pixel(event)
        dragMode = .orbit
    }
    override func rightMouseDragged(with event: NSEvent) { mouseDragged(with: event) }
    override func rightMouseUp(with event: NSEvent) {
        if !editing { engine.mouseButton(1, down: false) }
        dragMode = .none
    }

    override func otherMouseDown(with event: NSEvent) {
        if !editing {
            engine.mouseButton(2, down: true)
            return
        }
        lastPoint = pixel(event)
        dragMode = .pan
    }
    override func otherMouseDragged(with event: NSEvent) { mouseDragged(with: event) }
    override func otherMouseUp(with event: NSEvent) {
        if !editing { engine.mouseButton(2, down: false) }
        dragMode = .none
    }

    override func scrollWheel(with event: NSEvent) {
        if !editing {
            engine.scroll(dx: Float(event.scrollingDeltaX), dy: Float(event.scrollingDeltaY))
            return
        }
        if event.hasPreciseScrollingDeltas {
            if event.modifierFlags.contains(.shift) {
                let h = max(bounds.height, 1)
                engine.pan(dx: Float(-event.scrollingDeltaX / h), dy: Float(event.scrollingDeltaY / h))
            } else {
                engine.orbit(dx: Float(-event.scrollingDeltaX * 0.3), dy: Float(-event.scrollingDeltaY * 0.3))
            }
        } else {
            engine.zoom(event.scrollingDeltaY > 0 ? 0.9 : 1.1)
        }
    }

    override func magnify(with event: NSEvent) {
        engine.zoom(Float(max(0.2, 1 - event.magnification)))
    }

    // MARK: Keyboard

    private func keyName(_ event: NSEvent) -> String? {
        switch event.keyCode {
        case 49: return "space"
        case 36: return "enter"
        case 53: return "escape"
        case 123: return "left"
        case 124: return "right"
        case 125: return "down"
        case 126: return "up"
        case 48: return "tab"
        case 51: return "backspace"
        default: return event.charactersIgnoringModifiers?.lowercased()
        }
    }

    override func keyDown(with event: NSEvent) {
        guard let name = keyName(event) else { return }
        if !editing {
            if !event.isARepeat { engine.key(name, down: true) }
            return
        }
        if event.modifierFlags.contains(.command) {
            if name == "d" { duplicateSelection() } else { super.keyDown(with: event) }
            return
        }
        switch name {
        case "q": engine.gizmoMode = .select
        case "w": engine.gizmoMode = .move
        case "e": engine.gizmoMode = .rotate
        case "r": engine.gizmoMode = .scale
        case "f":
            if let first = engine.selection.first { engine.call("camera_set", ["frame": .number(Double(first))]) }
            else { engine.call("camera_set", ["frame": "all"]) }
        case "escape": engine.selection = []
        case "backspace":
            guard !engine.selection.isEmpty else { return }
            let ops: [JSON] = engine.selection.map { ["tool": "entity_delete", "args": ["entity": .number(Double($0))]] }
            engine.call("batch", ["operations": .array(ops), "label": "Delete selection"])
        default: super.keyDown(with: event)
        }
    }

    private func duplicateSelection() {
        guard !engine.selection.isEmpty else { return }
        let ops: [JSON] = engine.selection.map {
            ["tool": "entity_duplicate", "args": ["entity": .number(Double($0)), "offset": .vec3(1, 0, 0)]]
        }
        let r = engine.call("batch", ["operations": .array(ops), "label": "Duplicate selection"])
        let created = r.structured["results"].array.flatMap { $0["created"].array.compactMap(\.number) }
        if !created.isEmpty { engine.selection = Set(created.map { UInt64($0) }) }
    }

    private var heldModifiers: NSEvent.ModifierFlags = []

    /// Shift / control / option / command are not key events: derive their presses from flag changes.
    override func flagsChanged(with event: NSEvent) {
        let now = event.modifierFlags.intersection([.shift, .control, .option, .command])
        if !editing {
            for (flag, name) in [(NSEvent.ModifierFlags.shift, "shift"), (.control, "ctrl"), (.option, "alt"), (.command, "cmd")] {
                if now.contains(flag) != heldModifiers.contains(flag) { engine.key(name, down: now.contains(flag)) }
            }
        }
        heldModifiers = now
    }

    override func keyUp(with event: NSEvent) {
        guard let name = keyName(event), !editing else { return }
        engine.key(name, down: false)
    }
}
