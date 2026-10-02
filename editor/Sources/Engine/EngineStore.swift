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
    var components: [String]
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

/// The Swift-side owner of the C++ engine. Main-actor isolated, matching the engine's
/// single-threaded model; agents and UI both call tools through `call(_:_:actor:)`.
@MainActor
@Observable
final class EngineStore {
    private(set) var handle: OpaquePointer?
    private(set) var entities: [EntitySummary] = []
    private(set) var sceneName = "Untitled"
    private(set) var playState = "editing"
    private(set) var activity: [ActivityItem] = []
    private(set) var logs: [String] = []
    private(set) var revision: UInt64 = 0
    private(set) var agentServerRunning = false
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
        let json = raw.flatMap { JSON.parse(String(cString: $0)) } ?? .null
        var texts: [String] = []
        var images: [String] = []
        for block in json["content"].array {
            if block["type"].string == "text", let t = block["text"].string { texts.append(t) }
            if block["type"].string == "image", let d = block["data"].string { images.append(d) }
        }
        refresh(force: false)
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
        sky_update(handle, now.timeIntervalSince(lastTick))
        lastTick = now
        pollEvents()
        refresh(force: false)
    }

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
        entities = overview.structured["entities"].array.map {
            EntitySummary(id: UInt64($0["id"].number ?? 0), name: $0["name"].string ?? "",
                          parent: UInt64($0["parent"].number ?? 0),
                          components: $0["components"].array.compactMap(\.string))
        }
        syncSelectionFromEngine()
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
            case "tool" where actor != "editor" && actor != "user":
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
            case "selection":
                syncSelectionFromEngine()
            case "asset_request":
                append(ActivityItem(actor: e["request"]["requestedBy"].string ?? actor, kind: "asset",
                                    text: "requested \(e["request"]["kind"].string ?? "asset"): \(e["request"]["prompt"].string ?? "")", ok: true))
            default:
                break
            }
        }
    }

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
    func click(entity: UInt64) { sky_input_click(handle, entity) }

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
