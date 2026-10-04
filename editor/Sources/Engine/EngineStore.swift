import CSkywalker
import Foundation
import Observation

/// Result of a tool call, decoded from the engine's MCP CallToolResult.
struct ToolCallResult: Sendable {
    var text: String
    var imagesBase64: [String]
    var structured: JSON
    var isError: Bool
    var raw: JSON
}

/// One row of the hierarchy, decoded from `scene_overview`.
struct EntitySummary: Identifiable, Hashable, Sendable {
    let id: UInt64
    var name: String
    var parent: UInt64
    var enabled: Bool
    var components: [String]
    var prefabSource = ""      // a linked prefab instance root: its prefab file
    var prefabMember = false   // part of a linked prefab instance (not the root)
}

enum GizmoMode: Int, CaseIterable, Identifiable, Sendable {
    case select = 0, move = 1, rotate = 2, scale = 3
    var id: Int { rawValue }
    var symbol: String {
        switch self {
        case .select: "cursorarrow"
        case .move: "arrow.up.and.down.and.arrow.left.and.right"
        case .rotate: "arrow.trianglehead.2.clockwise.rotate.90"
        case .scale: "arrow.up.left.and.arrow.down.right"
        }
    }
    var title: String {
        switch self {
        case .select: "Select (Q)"
        case .move: "Move (W)"
        case .rotate: "Rotate (E)"
        case .scale: "Scale (R)"
        }
    }
}

struct FrameStats: Sendable {
    var cpuMs = 0.0
    var draws = 0
    var entities = 0
    var renderer = ""
    var fps = 0.0
    // Profiler (perf_stats {passes: true} in the engine): rolling 60-frame averages.
    var gpuMs = 0.0
    var profilerSupported = false
    var passes: [PassTiming] = []
    var cpuScopes: [PassTiming] = []
    var debugView = "final"
}

/// One row of the profiler list: a GPU pass (group = shadows, main, post, ...) or a CPU scope.
struct PassTiming: Sendable, Identifiable {
    var id: String { name }
    var name: String
    var group: String
    var ms: Double
    var maxMs: Double
}

/// An entry of the activity feed: who did what.
struct ActivityItem: Identifiable, Sendable {
    let id = UUID()
    let date = Date()
    var actor: String
    var kind: String
    var text: String
    var ok: Bool
}

/// The running (or last) movie render, from the engine's movie_progress / movie_finished events.
struct MovieProgress: Sendable {
    var state = "rendering"  // rendering | done | cancelled | failed
    var name = ""
    var frame = 0
    var frames = 0
    var msPerFrame = 0.0
    var eta = 0.0
    var outputs: [String] = []
    var error: String?
    var finished: Bool { state != "rendering" }
    var fraction: Double { frames > 0 ? Double(frame) / Double(frames) : 0 }

    init(_ j: JSON) {
        state = j["state"].string ?? "rendering"
        name = j["name"].string ?? ""
        frame = j["frame"].int ?? 0
        frames = j["frames"].int ?? 0
        msPerFrame = j["ms_per_frame"].number ?? 0
        eta = j["eta_s"].number ?? 0
        outputs = j["outputs"].array.compactMap { $0["path"].string }
        error = j["error"].string
    }
}

/// What the Render Movie sheet opens with (a sequence picked from the Sequencer, or nothing).
struct MovieRequest: Identifiable, Sendable {
    let id = UUID()
    var sequence: UInt64?
}

/// A deferred tool call (`SkyPendingCall`) handed to a background task. The engine documents
/// `sky_pending_run` as safe to call from any thread while the main thread stays free.
private struct PendingCallBox: @unchecked Sendable {
    let pointer: OpaquePointer
}

/// The Swift-side owner of the C++ engine. Main-actor isolated, matching the engine's
/// single-threaded model; agents and UI both call tools through `call(_:_:actor:)`.
@MainActor
@Observable
final class EngineStore {
    private(set) var handle: OpaquePointer?
    private let gamepads = GamepadBridge()
    private(set) var entities: [EntitySummary] = []
    private(set) var sceneName = "Untitled"
    private(set) var playState = "editing"
    private(set) var activity: [ActivityItem] = []
    private(set) var logs: [String] = []
    private(set) var revision: UInt64 = 0
    private(set) var agentServerRunning = false
    private(set) var stats = FrameStats()
    private(set) var environment: JSON = .null
    private(set) var cameraYaw: Float = 0
    private(set) var cameraPitch: Float = 0
    /// Movie renderer: the running/last render, and the open Render Movie sheet.
    private(set) var movie: MovieProgress?
    var movieRequest: MovieRequest?
    var gizmoMode: GizmoMode = .move { didSet { applyGizmo() } }
    var gizmoLocal = false { didSet { applyGizmo() } }
    var snapping = false
    var snapStep: Float = 0.5 { didSet { applyGizmo() } }
    var viewSceneCamera = false { didSet { sky_set_view_scene_camera(handle, viewSceneCamera ? 1 : 0) } }
    /// Editing-time viewport quality (fast / balanced / full); play mode always renders full.
    var viewportQuality = "fast" { didSet { call("viewport_quality", ["quality": .string(viewportQuality)]) } }
    /// Live viewport debug view (wireframe, overdraw, lod, ...; "final" = off). Agents can change it too.
    func setDebugView(_ view: String) {
        call("viewport_debug_view", ["view": .string(view)])
        stats.debugView = view
    }
    @ObservationIgnored private var frameCount = 0
    @ObservationIgnored private var fpsWindowStart = Date()
    var selection: Set<UInt64> = [] {
        didSet {
            guard selection != oldValue, !syncingSelection else { return }
            let ids = Array(selection)
            ids.withUnsafeBufferPointer { sky_set_selection(handle, $0.baseAddress, Int32($0.count), "user") }
        }
    }
    @ObservationIgnored private var syncingSelection = false
    @ObservationIgnored private var lastRevision: UInt64 = .max
    @ObservationIgnored private var lastTick = Date()
    @ObservationIgnored private var lastOverview = Date.distantPast
    @ObservationIgnored let projectDirectory: URL
    /// Called when the engine reports studio activity (tasks, feedback, decisions, loops...).
    @ObservationIgnored var onStudioEvent: (() -> Void)?

    init(projectDirectory: URL) {
        self.projectDirectory = projectDirectory
        handle = sky_engine_create(projectDirectory.path)
        let scene = projectDirectory.appending(path: "scenes/main.sky.json")
        if FileManager.default.fileExists(atPath: scene.path) {
            _ = call("scene_load", ["path": "scenes/main.sky.json"])
        } else {
            _ = call("scene_new", ["name": "My First Sky"])
        }
        _ = call("camera_set", ["frame": "all"])
        refresh(force: true)
        applyGizmo()
        // External agents (Claude Code, Codex, ...) can attach immediately. The socket is
        // user-only (0600); the toggle lives in Settings → External Agents.
        if UserDefaults.standard.object(forKey: Self.autoStartKey) as? Bool ?? true {
            setAgentServer(enabled: true)
        }
    }

    static let autoStartKey = "agentServerAutoStart"

    /// Must be called before the store goes away (SwiftUI never deinitializes app-lifetime
    /// stores, but tests and multi-window setups do).
    func shutdown() {
        guard let h = handle else { return }
        sky_agent_server_stop(h)
        sky_engine_destroy(h)
        handle = nil
    }

    // MARK: Tools

    @discardableResult
    func call(_ tool: String, _ args: JSON = [:], actor: String = "user") -> ToolCallResult {
        guard let handle else { return ToolCallResult(text: "engine not running", imagesBase64: [], structured: .null, isError: true, raw: .null) }
        let raw = sky_call_tool(handle, tool, args.serialized(), actor)
        defer { sky_string_free(raw) }
        let result = Self.decode(raw)
        refresh(force: false)
        return result
    }

    /// Like `call`, but a tool that runs something slow (a design app such as Blender) does it off
    /// the main thread while the editor keeps drawing and handling input. Use this from async
    /// contexts (the crew, buttons); `call` stays for the quick synchronous paths.
    @discardableResult
    func callAsync(_ tool: String, _ args: JSON = [:], actor: String = "user") async -> ToolCallResult {
        guard let handle else { return ToolCallResult(text: "engine not running", imagesBase64: [], structured: .null, isError: true, raw: .null) }
        var pending: OpaquePointer?
        let first = sky_call_tool_begin(handle, tool, args.serialized(), actor, &pending)
        if let first {
            defer { sky_string_free(first) }
            let result = Self.decode(first)
            refresh(force: false)
            return result
        }
        guard let pending else {
            return ToolCallResult(text: "the tool did not start", imagesBase64: [], structured: .null, isError: true, raw: .null)
        }
        let box = PendingCallBox(pointer: pending)
        await Task.detached(priority: .userInitiated) { sky_pending_run(box.pointer) }.value
        let raw = sky_pending_finish(handle, pending)
        defer { sky_string_free(raw) }
        let result = Self.decode(raw)
        refresh(force: false)
        return result
    }

    private static func decode(_ raw: UnsafeMutablePointer<CChar>?) -> ToolCallResult {
        let json = raw.flatMap { JSON.parse(String(cString: $0)) } ?? .null
        var texts: [String] = []
        var images: [String] = []
        for block in json["content"].array {
            if block["type"].string == "text", let t = block["text"].string { texts.append(t) }
            if block["type"].string == "image", let d = block["data"].string { images.append(d) }
        }
        return ToolCallResult(text: texts.joined(separator: "\n"), imagesBase64: images,
                              structured: json["structuredContent"], isError: json["isError"].bool ?? false, raw: json)
    }

    func toolList() -> JSON {
        guard let handle, let raw = sky_tools_list(handle) else { return .null }
        defer { sky_string_free(raw) }
        return JSON.parse(String(cString: raw)) ?? .null
    }

    func entity(_ id: UInt64) -> JSON {
        let r = call("entity_get", ["entity": .number(Double(id))])
        return r.isError ? .null : r.structured
    }

    // MARK: Frame loop

    /// Called every display frame by the viewport.
    func tick() {
        guard let handle else { return }
        let now = Date()
        if playState == "playing" { gamepads.poll(into: handle) }  // controllers feed the game only while it runs
        sky_update(handle, now.timeIntervalSince(lastTick))
        lastTick = now
        pollEvents()
        refresh(force: false)
        updateStats(now: now)
    }

    private func updateStats(now: Date) {
        guard let handle else { return }
        frameCount += 1
        var yaw: Float = 0, pitch: Float = 0
        sky_camera_angles(handle, &yaw, &pitch)
        if abs(yaw - cameraYaw) > 0.01 || abs(pitch - cameraPitch) > 0.01 {
            cameraYaw = yaw
            cameraPitch = pitch
        }
        let elapsed = now.timeIntervalSince(fpsWindowStart)
        guard elapsed >= 0.5, let raw = sky_frame_stats(handle) else { return }  // update UI twice a second
        defer { sky_string_free(raw) }
        let j = JSON.parse(String(cString: raw)) ?? .null
        let profile = j["profile"]
        stats = FrameStats(cpuMs: j["cpuMs"].number ?? 0, draws: j["draws"].int ?? 0, entities: j["entities"].int ?? 0,
                           renderer: j["renderer"].string ?? "", fps: Double(frameCount) / elapsed,
                           gpuMs: j["gpuMs"].number ?? 0, profilerSupported: profile["supported"].bool ?? false,
                           passes: profile["passes"].array.map {
                               PassTiming(name: $0["pass"].string ?? "?", group: $0["group"].string ?? "",
                                          ms: $0["avgMs"].number ?? 0, maxMs: $0["maxMs"].number ?? 0)
                           },
                           cpuScopes: profile["cpu"].array.map {
                               PassTiming(name: $0["scope"].string ?? "?", group: "cpu", ms: $0["avgMs"].number ?? 0,
                                          maxMs: $0["maxMs"].number ?? 0)
                           },
                           debugView: j["debugView"].string ?? "final")
        frameCount = 0
        fpsWindowStart = now
    }

    private func applyGizmo() {
        sky_gizmo_set_mode(handle, Int32(gizmoMode.rawValue), gizmoLocal ? 1 : 0)
        sky_gizmo_set_snap(handle, snapStep, 15)
    }

    // Gizmo interaction (pixel coordinates)
    func gizmoHover(x: Float, y: Float, width: Int, height: Int) { _ = sky_gizmo_hover(handle, x, y, Int32(width), Int32(height)) }
    func gizmoBegin(x: Float, y: Float, width: Int, height: Int) -> Bool {
        sky_gizmo_begin(handle, x, y, Int32(width), Int32(height)) != 0
    }
    func gizmoDrag(x: Float, y: Float, width: Int, height: Int, snap: Bool) {
        sky_gizmo_drag(handle, x, y, Int32(width), Int32(height), snap || snapping ? 1 : 0)
    }
    func gizmoEnd() { sky_gizmo_end(handle) }

    func render(layer: UnsafeMutableRawPointer, width: Int, height: Int) {
        guard let handle, width > 0, height > 0 else { return }
        _ = sky_render_to_layer(handle, layer, Int32(width), Int32(height))
    }

    private func refresh(force: Bool) {
        guard let handle else { return }
        let rev = sky_scene_revision(handle)
        let state = String(cString: sky_play_state(handle))
        if state != playState { playState = state }  // avoid invalidating views every frame
        if !force && rev == lastRevision { return }
        // While playing, transforms change every tick but the outline rarely does:
        // refresh it at most a few times per second.
        if !force && playState != "editing" && Date().timeIntervalSince(lastOverview) < 0.3 { return }
        lastOverview = Date()
        lastRevision = rev
        if revision != rev { revision = rev }
        let overview = call_noRefresh("scene_overview", ["max_entities": 5000])
        sceneName = overview.structured["name"].string ?? sceneName
        environment = call_noRefresh("environment_get", [:]).structured
        entities = overview.structured["entities"].array.map {
            EntitySummary(id: UInt64($0["id"].number ?? 0), name: $0["name"].string ?? "",
                          parent: UInt64($0["parent"].number ?? 0),
                          enabled: $0["enabled"].bool ?? true,
                          components: $0["components"].array.compactMap(\.string),
                          prefabSource: $0["prefab"].string ?? "",
                          prefabMember: $0["prefab"].number != nil)
        }
        syncSelectionFromEngine()
    }

    /// The project-relative scene file the engine has open ("" until the scene is first saved). The engine
    /// tracks it across scene_load / scene_save from every client, so it is read fresh, not cached.
    func currentScenePath() -> String {
        call_noRefresh("scene_overview", ["max_entities": 0]).structured["path"].string ?? ""
    }

    /// Saves the open scene to `path` (project-relative), or back to its own file when `path` is nil.
    @discardableResult
    func saveScene(to path: String? = nil) -> ToolCallResult {
        let args: JSON = path.map { JSON.object([("path", .string($0))]) } ?? [:]
        return call("scene_save", args)
    }

    private func call_noRefresh(_ tool: String, _ args: JSON) -> ToolCallResult {
        guard let handle else { return ToolCallResult(text: "", imagesBase64: [], structured: .null, isError: true, raw: .null) }
        let raw = sky_call_tool(handle, tool, args.serialized(), "editor")
        defer { sky_string_free(raw) }
        let json = raw.flatMap { JSON.parse(String(cString: $0)) } ?? .null
        return ToolCallResult(text: json["content"][0]["text"].string ?? "", imagesBase64: [],
                              structured: json["structuredContent"], isError: json["isError"].bool ?? false, raw: json)
    }

    private func syncSelectionFromEngine() {
        guard let handle else { return }
        var buffer = [UInt64](repeating: 0, count: 256)
        let n = Int(sky_get_selection(handle, &buffer, Int32(buffer.count)))
        let engineSel = Set(buffer.prefix(min(n, buffer.count)))
        if engineSel != selection {
            syncingSelection = true
            selection = engineSel
            syncingSelection = false
        }
    }

    private func pollEvents() {
        guard let handle, let raw = sky_poll_events(handle) else { return }
        defer { sky_string_free(raw) }
        guard let events = JSON.parse(String(cString: raw)) else { return }
        for e in events.array {
            let actor = e["actor"].string ?? "engine"
            switch e["type"].string {
            case "edit":
                append(ActivityItem(actor: actor, kind: "edit", text: e["label"].string ?? "edit", ok: true))
            case "tool" where actor != "editor" && actor != "user" && (e["mutates"].bool ?? false):
                append(ActivityItem(actor: actor, kind: "tool", text: e["tool"].string ?? "",
                                    ok: e["ok"].bool ?? true))
            case "undo", "redo":
                append(ActivityItem(actor: actor, kind: e["type"].string!, text: e["label"].string ?? "", ok: true))
            case "log":
                let line = "[\(e["kind"].string ?? "log")] #\(e["entity"].int ?? 0) \(e["script"].string ?? ""):\(e["line"].int ?? 0) \(e["text"].string ?? "")"
                logs.append(line)
                if logs.count > 500 { logs.removeFirst(logs.count - 500) }
            case "play_state":
                playState = e["state"].string ?? playState
            case "movie_progress", "movie_finished":
                movie = MovieProgress(e)
                if e["type"].string == "movie_finished" {
                    let p = movie!
                    append(ActivityItem(actor: actor, kind: "movie",
                                        text: p.state == "failed" ? "movie failed: \(p.error ?? "")" : "movie \(p.state): \(p.frame) frames → \(p.outputs.first ?? "")",
                                        ok: p.state != "failed"))
                }
            case "selection":
                syncSelectionFromEngine()
            case "studio":
                onStudioEvent?()
                let kind = e["kind"].string ?? ""
                let action = e["action"].string ?? ""
                let notable = kind == "decision" || kind == "playtest" || (kind == "feedback" && action != "seen_again")
                    || (kind == "loop" && (action == "started" || action == "finished"))
                if notable {
                    append(ActivityItem(actor: actor, kind: "studio", text: e["summary"].string ?? kind, ok: true))
                }
            case "custom_tool":
                onStudioEvent?()  // the Studio's Tools tab
                let action = e["action"].string ?? ""
                if action != "call" {
                    let status = e["status"].string.map { " (\($0.replacingOccurrences(of: "_", with: " ")))" } ?? ""
                    append(ActivityItem(actor: actor, kind: "tool", text: "\(action) \(e["tool"].string ?? "")\(status)", ok: true))
                }
            case "asset_request":
                append(ActivityItem(actor: e["request"]["requestedBy"].string ?? actor, kind: "asset",
                                    text: "requested \(e["request"]["kind"].string ?? "asset"): \(e["request"]["prompt"].string ?? "")", ok: true))
            default:
                break
            }
        }
    }

    func clearLogs() { logs.removeAll() }

    // MARK: Movie renderer

    /// Starts a background movie render (the engine advances it frame by frame in `tick`).
    @discardableResult
    func startMovie(_ args: JSON) -> ToolCallResult {
        var a = args
        a.set("background", true)
        let r = call("movie_render", a)
        if !r.isError { movie = MovieProgress(r.structured) }
        return r
    }

    func cancelMovie() { call("movie_render", ["action": "cancel"]) }
    func dismissMovie() { if movie?.finished == true { movie = nil } }

    private func append(_ item: ActivityItem) {
        activity.append(item)
        if activity.count > 300 { activity.removeFirst(activity.count - 300) }
    }

    // MARK: Viewport interaction

    func orbit(dx: Float, dy: Float) { sky_camera_orbit(handle, dx, dy) }
    func pan(dx: Float, dy: Float) { sky_camera_pan(handle, dx, dy) }
    func zoom(_ factor: Float) { sky_camera_zoom(handle, factor) }

    func pick(x: Float, y: Float, width: Int, height: Int) -> UInt64 {
        sky_pick(handle, x, y, Int32(width), Int32(height))
    }

    func dragBegin(entity: UInt64, x: Float, y: Float, width: Int, height: Int) {
        sky_drag_begin(handle, entity, x, y, Int32(width), Int32(height))
    }
    func dragUpdate(x: Float, y: Float, width: Int, height: Int) { sky_drag_update(handle, x, y, Int32(width), Int32(height)) }
    func dragEnd() { sky_drag_end(handle) }

    func key(_ name: String, down: Bool) { sky_input_key(handle, name, down ? 1 : 0) }
    /// Mouse input while playing: `x`/`y` are 0...1 inside the viewport (top-left origin), `dx`/`dy` in points (+y down).
    func mouseMove(x: Float, y: Float, dx: Float, dy: Float) { sky_input_mouse_move(handle, x, y, dx, dy) }
    /// button: 0 left, 1 right, 2 middle.
    func mouseButton(_ button: Int, down: Bool) { sky_input_mouse_button(handle, Int32(button), down ? 1 : 0) }
    func scroll(dx: Float, dy: Float) { sky_input_scroll(handle, dx, dy) }
    func text(_ characters: String) { sky_input_text(handle, characters) }
    func click(entity: UInt64) { sky_input_click(handle, entity) }

    // MARK: Assets

    static let assetDragPrefix = "skywalker-asset:"
    /// Outliner rows drag "skywalker-entity:<id>" onto entity-link fields in the Details panel.
    static let entityDragPrefix = "skywalker-entity:"

    /// Places a mesh or prefab asset where a viewport pixel points (on the surface under it),
    /// or in front of the camera's target when `pixel` is nil. Applies materials/textures to
    /// the object under the cursor.
    func placeAsset(_ path: String, type: String, at pixel: (x: Float, y: Float, width: Int, height: Int)?) {
        var position: JSON = .vec3(0, 0, 0)
        var target: UInt64 = 0
        if let pixel {
            let hit = call("raycast", ["x": .number(Double(pixel.x)), "y": .number(Double(pixel.y)),
                                       "width": .number(Double(pixel.width)), "height": .number(Double(pixel.height))],
                           actor: "editor").structured
            if hit["hit"].bool == true {
                position = hit["point"]
                target = UInt64(hit["entity"].number ?? 0)
            }
        } else {
            let t = call("camera_set", [:], actor: "editor").structured["target"].array  // current view target
            if t.count == 3 { position = .array(t) }
        }
        let name = ((path as NSString).lastPathComponent as NSString).deletingPathExtension
            .replacingOccurrences(of: ".prefab", with: "")
        switch type {
        case "mesh":
            let r = call("asset_import", ["path": .string(path), "create_entity": .string(name.capitalized), "position": position])
            if let id = r.structured["entity"].number {
                call("place_on_surface", ["entities": [.number(id)]])
                selection = [UInt64(id)]
            }
        case "prefab":
            let r = call("prefab_instantiate", ["prefab": .string(path), "position": position, "on_surface": true])
            if let id = r.structured["entity"].number { selection = [UInt64(id)] }
        case "material", "texture":
            if target != 0 { assignAsset(path, type: type, to: [target]) }
        default:
            break
        }
    }

    func assignAsset(_ path: String, type: String, to entities: [UInt64]) {
        guard !entities.isEmpty else { return }
        let ids = JSON.array(entities.map { .number(Double($0)) })
        switch type {
        case "material":
            call("material_assign", ["entities": ids, "material": .string(path)])
        case "texture", "mesh":
            let field = type == "texture" ? "texture" : "mesh"
            let value = type == "texture" ? path : "asset:" + path
            let ops: [JSON] = entities.map {
                ["tool": "entity_update", "args": ["entity": .number(Double($0)), "components": ["mesh": [field: .string(value)]]]]
            }
            call("batch", ["operations": .array(ops), "label": .string("Assign \(type)")])
        default:
            break
        }
    }

    // MARK: Agent server

    func setAgentServer(enabled: Bool) {
        guard let handle else { return }
        if enabled {
            agentServerRunning = sky_agent_server_start(handle, nil) == 0
            UserDefaults.standard.set(true, forKey: Self.autoStartKey)
        } else {
            UserDefaults.standard.set(false, forKey: Self.autoStartKey)
            sky_agent_server_stop(handle)
            agentServerRunning = false
        }
    }

    static var socketPath: String {
        FileManager.default.homeDirectoryForCurrentUser.appending(path: ".skywalker/editor.sock").path
    }
}
