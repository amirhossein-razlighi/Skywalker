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
/// Controls (editing):  click = select · drag object = move on its ground plane ·
/// drag empty space / right-drag / two-finger scroll = orbit · ⇧ + scroll or middle-drag = pan ·
/// pinch or mouse wheel = zoom · F = frame selection · ⌫ = delete selection.
/// While playing, keys and clicks go to the game (Wander `on key` / `on click`).
final class ViewportNSView: NSView {
    private let engine: EngineStore
    private var displayLink: CADisplayLink?
    private var metalLayer: CAMetalLayer { layer as! CAMetalLayer }  // swiftlint:disable:this force_cast
    private enum DragMode { case none, orbit, pan, moveEntity }
    private var dragMode: DragMode = .none
    private var lastPoint = CGPoint.zero

    init(engine: EngineStore) {
        self.engine = engine
        super.init(frame: .zero)
        wantsLayer = true
        layerContentsRedrawPolicy = .never
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
        displayLink?.invalidate()
        displayLink = nil
        guard window != nil else { return }
        updateDrawableSize()
        let link = displayLink(target: self, selector: #selector(step(_:)))
        link.add(to: .main, forMode: .common)
        displayLink = link
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

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        let p = pixel(event)
        lastPoint = p
        let (w, h) = pixelSize
        let hit = engine.pick(x: Float(p.x), y: Float(p.y), width: w, height: h)
        if engine.playState != "editing" {
            if hit != 0 { engine.click(entity: hit) }
            dragMode = .orbit
            return
        }
        if event.modifierFlags.contains(.option) || hit == 0 {
            if hit == 0 && !event.modifierFlags.contains(.shift) { engine.selection = [] }
            dragMode = .orbit
            return
        }
        if event.modifierFlags.contains(.shift) || event.modifierFlags.contains(.command) {
            if engine.selection.contains(hit) { engine.selection.remove(hit) } else { engine.selection.insert(hit) }
            dragMode = .none
        } else {
            engine.selection = [hit]
            engine.dragBegin(entity: hit, x: Float(p.x), y: Float(p.y), width: w, height: h)
            dragMode = .moveEntity
        }
    }

    override func mouseDragged(with event: NSEvent) {
        let p = pixel(event)
        defer { lastPoint = p }
        let (w, h) = pixelSize
        switch dragMode {
        case .orbit: engine.orbit(dx: Float(-(p.x - lastPoint.x) * 0.25), dy: Float((p.y - lastPoint.y) * 0.25))
        case .pan: engine.pan(dx: Float((p.x - lastPoint.x) / CGFloat(h)), dy: Float((p.y - lastPoint.y) / CGFloat(h)))
        case .moveEntity: engine.dragUpdate(x: Float(p.x), y: Float(p.y), width: w, height: h)
        case .none: break
        }
    }

    override func mouseUp(with event: NSEvent) {
        if dragMode == .moveEntity { engine.dragEnd() }
        dragMode = .none
    }

    override func rightMouseDown(with event: NSEvent) {
        lastPoint = pixel(event)
        dragMode = .orbit
    }
    override func rightMouseDragged(with event: NSEvent) { mouseDragged(with: event) }
    override func rightMouseUp(with event: NSEvent) { dragMode = .none }

    override func otherMouseDown(with event: NSEvent) {
        lastPoint = pixel(event)
        dragMode = .pan
    }
    override func otherMouseDragged(with event: NSEvent) { mouseDragged(with: event) }
    override func otherMouseUp(with event: NSEvent) { dragMode = .none }

    override func scrollWheel(with event: NSEvent) {
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
        if engine.playState != "editing" {
            if !event.isARepeat { engine.key(name, down: true) }
            return
        }
        switch name {
        case "f":
            if let first = engine.selection.first { engine.call("camera_set", ["frame": .number(Double(first))]) }
            else { engine.call("camera_set", ["frame": "all"]) }
        case "backspace":
            if !engine.selection.isEmpty {
                let ops: [JSON] = engine.selection.map { ["tool": "entity_delete", "args": ["entity": .number(Double($0))]] }
                engine.call("batch", ["operations": .array(ops), "label": "Delete selection"])
            }
        default: super.keyDown(with: event)
        }
    }

    override func keyUp(with event: NSEvent) {
        guard let name = keyName(event), engine.playState != "editing" else { return }
        engine.key(name, down: false)
    }
}
