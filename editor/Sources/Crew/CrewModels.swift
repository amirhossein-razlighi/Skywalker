import SwiftUI

/// Where a Cloudling's "brain" lives. Two wire protocols cover nearly every model:
/// the Anthropic Messages API, and the OpenAI-compatible Chat Completions API (OpenAI,
/// DeepSeek, Mistral, Groq, OpenRouter, and local/self-hosted servers such as Ollama,
/// LM Studio, vLLM and llama.cpp).
enum ProviderKind: String, Codable, CaseIterable, Identifiable, Sendable {
    case anthropic
    case openAICompatible

    var id: String { rawValue }
    var label: String {
        switch self {
        case .anthropic: "Anthropic (Claude)"
        case .openAICompatible: "OpenAI-compatible"
        }
    }
}

struct ProviderConfig: Codable, Identifiable, Hashable, Sendable {
    var id = UUID()
    var name: String
    var kind: ProviderKind
    var baseURL: String
    var defaultModel: String
    /// Whether the model accepts images (screenshots of the viewport).
    var supportsVision = true

    /// API keys are never stored here — they live in the Keychain under this account name.
    var keychainAccount: String { "provider-\(id.uuidString)" }

    static let presets: [ProviderConfig] = [
        ProviderConfig(name: "Claude", kind: .anthropic, baseURL: "https://api.anthropic.com",
                       defaultModel: "claude-opus-5-5"),
        ProviderConfig(name: "OpenAI", kind: .openAICompatible, baseURL: "https://api.openai.com/v1",
                       defaultModel: "gpt-5"),
        ProviderConfig(name: "DeepSeek", kind: .openAICompatible, baseURL: "https://api.deepseek.com/v1",
                       defaultModel: "deepseek-chat", supportsVision: false),
        ProviderConfig(name: "Ollama (local)", kind: .openAICompatible, baseURL: "http://localhost:11434/v1",
                       defaultModel: "qwen3", supportsVision: false),
        ProviderConfig(name: "LM Studio (local)", kind: .openAICompatible, baseURL: "http://localhost:1234/v1",
                       defaultModel: "local-model", supportsVision: false),
    ]
}

/// A studio role (from the engine's catalogue: `studio_overview {include_catalog: true}`).
struct CrewRole: Hashable, Identifiable, Sendable {
    var id: String
    var title: String
    var discipline: String
    var symbol: String
    var mission: String

    /// Filled from the engine at launch; the engine is the single source of truth.
    @MainActor static var all: [CrewRole] = []

    @MainActor static func named(_ id: String) -> CrewRole {
        all.first { $0.id == id }
            ?? CrewRole(id: id, title: id.replacingOccurrences(of: "_", with: " ").capitalized, discipline: "",
                        symbol: "person.fill", mission: "")
    }
}

/// How much freedom a Cloudling has.
enum Autonomy: String, Codable, CaseIterable, Identifiable, Sendable {
    case observe     // read-only tools (plus studio coordination)
    case ask         // mutating tools need your approval
    case auto = "autonomous"  // acts freely (everything is undoable)

    var id: String { rawValue }
    var label: String {
        switch self {
        case .observe: "Observe only"
        case .ask: "Ask before changes"
        case .auto: "Autonomous"
        }
    }
}

/// Per-category override of what a Cloudling may do with a group of engine tools.
enum ToolAccess: String, Codable, CaseIterable, Identifiable, Sendable {
    case inherit  // follow the Cloudling's autonomy
    case allow
    case ask
    case off

    var id: String { rawValue }
    var label: String {
        switch self {
        case .inherit: "Default"
        case .allow: "Allow"
        case .ask: "Ask"
        case .off: "Off"
        }
    }
}

/// Engine tool categories (the `skywalker/category` of each tool) shown in the Agent Designer.
struct ToolCategory: Identifiable, Hashable, Sendable {
    let id: String
    let title: String
    let symbol: String
    let detail: String

    static let all: [ToolCategory] = [
        ToolCategory(id: "scene", title: "Scene", symbol: "square.stack.3d.up", detail: "overview, query, environment"),
        ToolCategory(id: "entity", title: "Entities", symbol: "cube", detail: "create, edit, transform, delete, batch"),
        ToolCategory(id: "world", title: "World", symbol: "globe", detail: "raycast, scatter, place on surface"),
        ToolCategory(id: "asset", title: "Assets", symbol: "shippingbox", detail: "browse, import, materials, prefabs, generators"),
        ToolCategory(id: "wander", title: "Behaviors", symbol: "chevron.left.forwardslash.chevron.right", detail: "Wander code"),
        ToolCategory(id: "sim", title: "Simulation", symbol: "play", detail: "play, step, input, trace"),
        ToolCategory(id: "view", title: "Viewport", symbol: "eye", detail: "captures, camera, selection"),
        ToolCategory(id: "history", title: "Files & History", symbol: "clock.arrow.circlepath", detail: "undo, save, load"),
        ToolCategory(id: "render", title: "Rendering", symbol: "paintbrush", detail: "shaders, perf stats"),
        ToolCategory(id: "physics", title: "Physics & Navigation", symbol: "atom", detail: "bodies, queries, settle, navmesh, paths"),
        ToolCategory(id: "animation", title: "Animation", symbol: "figure.walk", detail: "animators, clips, bone attachments, cinematic sequences"),
        ToolCategory(id: "network", title: "Network", symbol: "arrow.down.circle", detail: "download licensed assets (asks by default)"),
        ToolCategory(id: "dcc", title: "Design apps", symbol: "wand.and.stars", detail: "run Blender / Maya / Houdini scripts, convert and edit models (asks by default)"),
        ToolCategory(id: "files", title: "Ship game", symbol: "shippingbox.and.arrow.backward", detail: "game.json settings, build the macOS app, try it in the player (build and run ask by default)"),
        ToolCategory(id: "studio", title: "Studio", symbol: "person.3", detail: "board, feedback, decisions, messages, playtests"),
    ]
}

/// Tokens a Cloudling has used (tracked by the engine's studio: studio/usage.json).
struct TokenUsage: Hashable, Sendable {
    var input = 0
    var output = 0
    var cacheRead = 0
    var requests = 0
    var toolCalls = 0
    var costUSD = 0.0

    init(input: Int = 0, output: Int = 0, cacheRead: Int = 0, requests: Int = 0) {
        self.input = input
        self.output = output
        self.cacheRead = cacheRead
        self.requests = requests
    }

    init(json j: JSON) {
        input = (j["input_tokens"].int ?? 0) + (j["cache_write_tokens"].int ?? 0)
        output = j["output_tokens"].int ?? 0
        cacheRead = j["cache_read_tokens"].int ?? 0
        requests = j["requests"].int ?? 0
        toolCalls = j["tool_calls"].int ?? 0
        costUSD = j["cost_usd"].number ?? 0
    }
}

/// A studio agent ("Cloudling") as the editor sees it. The source of truth is the engine's
/// roster (`agents/<id>.agent.json`, shared with the CLI runner and external agents); edits
/// go back through `studio_agent_define`.
struct Cloudling: Identifiable, Hashable, Sendable {
    var id: String
    var name: String
    var roleID: String
    var discipline: String = ""
    var focus: String = ""
    var focusTags: [String] = []
    var colorHex: String = "#7fb3ff"
    var face: CloudFace = .happy
    /// Provider name from the agent file ("Claude", "Ollama (local)", "anthropic", ...).
    var provider: String = "anthropic"
    var model: String = ""
    var autonomy: Autonomy = .auto
    var personality: String = ""
    /// Extra standing instructions, appended to the role's mission.
    var instructions: String = ""
    /// Replaces the role's built-in mission when not empty.
    var missionOverride: String = ""
    var maxRounds: Int = 40
    var toolAccess: [String: ToolAccess] = [:]
    var reportsTo: String = ""
    /// Long-term notes (kept with `studio_memory`).
    var memory: [String] = []
    /// Playtester persona knobs.
    var playtest: JSON = .null

    init(id: String, name: String, roleID: String) {
        self.id = id
        self.name = name
        self.roleID = roleID
    }

    init?(json j: JSON) {
        guard let id = j["id"].string, !id.isEmpty else { return nil }
        self.id = id
        name = j["name"].string ?? id
        roleID = j["role"].string ?? ""
        discipline = j["discipline"].string ?? ""
        focus = j["focus"].string ?? ""
        focusTags = j["focus_tags"].array.compactMap(\.string)
        colorHex = j["color"].string ?? "#7fb3ff"
        face = CloudFace(rawValue: j["face"].string ?? "") ?? .happy
        provider = j["provider"].string ?? "anthropic"
        model = j["model"].string ?? ""
        autonomy = Autonomy(rawValue: j["autonomy"].string ?? "") ?? .auto
        personality = j["persona"].string ?? ""
        instructions = j["instructions"].string ?? ""
        missionOverride = j["mission"].string ?? ""
        maxRounds = j["max_rounds"].int ?? 40
        for (k, v) in j["permissions"].members {
            if let a = ToolAccess(rawValue: v.string ?? ""), a != .inherit { toolAccess[k] = a }
        }
        reportsTo = j["reports_to"].string ?? ""
        memory = j["memory"].array.compactMap(\.string)
        playtest = j["playtest"]
    }

    /// The spec for `studio_agent_define` (every editable field).
    var spec: JSON {
        var perms: [(String, JSON)] = []
        for cat in ToolCategory.all { perms.append((cat.id, .string((toolAccess[cat.id] ?? .inherit).rawValue))) }
        var j: JSON = [
            "id": .string(id), "name": .string(name), "role": .string(roleID), "focus": .string(focus),
            "focus_tags": .array(focusTags.map { .string($0) }), "persona": .string(personality),
            "mission": .string(missionOverride), "instructions": .string(instructions), "provider": .string(provider),
            "model": .string(model), "autonomy": .string(autonomy.rawValue), "permissions": .object(perms),
            "reports_to": .string(reportsTo), "color": .string(colorHex), "face": .string(face.rawValue),
            "max_rounds": .number(Double(maxRounds)), "memory": .array(memory.map { .string($0) }),
        ]
        if !playtest.isNull { j.set("playtest", playtest) }
        return j
    }

    /// Effective access for a tool category — mirrors `AgentProfile::access` in the engine.
    func access(category: String, readOnly: Bool, openWorld: Bool = false) -> ToolAccess {
        let explicit = toolAccess[category] ?? .inherit
        if explicit == .off { return .off }
        // Studio coordination never changes the game: allowed at every autonomy level.
        if category == "studio" { return explicit == .ask ? .ask : .allow }
        if readOnly && !openWorld { return .allow }
        if explicit != .inherit { return explicit }
        // Reaching outside the project (downloads, running design apps and their scripts) needs the
        // human's OK unless explicitly allowed.
        if openWorld || category == "network" || category == "dcc" { return autonomy == .observe ? .off : .ask }
        switch autonomy {
        case .observe: return .off
        case .ask: return .ask
        case .auto: return .allow
        }
    }

    @MainActor var role: CrewRole { CrewRole.named(roleID) }
    var color: Color { Color(hex: colorHex) }
    /// The engine actor for this agent: studio tools resolve it back to the roster member.
    var actorName: String { "agent:\(id)" }
}

/// Crew members of the editor's first versions (Application Support/crew.json) are migrated
/// into the project roster once.
struct LegacyCloudling: Decodable {
    var name: String
    var roleID: String?
    var colorHex: String?
    var face: CloudFace?
    var model: String?
    var autonomy: String?
    var personality: String?
    var instructions: String?
    var missionOverride: String?
    var maxRounds: Int?
    var toolAccess: [String: String]?
    var memory: [String]?

    static let roleMap = ["director": "creative_director", "level": "level_designer", "gameplay": "gameplay_programmer",
                          "lighting": "lighting_artist", "writer": "writer", "artist": "environment_artist", "qa": "qa_lead"]

    var spec: JSON {
        var perms: [(String, JSON)] = []
        for (k, v) in toolAccess ?? [:] where v != "inherit" { perms.append((k, .string(v))) }
        let auto = autonomy == "auto" ? "autonomous" : (autonomy ?? "autonomous")
        return [
            "name": .string(name), "role": .string(Self.roleMap[roleID ?? ""] ?? "level_designer"),
            "color": .string(colorHex ?? "#7fb3ff"), "face": .string((face ?? .happy).rawValue), "model": .string(model ?? ""),
            "autonomy": .string(auto), "persona": .string(personality ?? ""), "instructions": .string(instructions ?? ""),
            "mission": .string(missionOverride ?? ""), "max_rounds": .number(Double(maxRounds ?? 40)),
            "permissions": .object(perms), "memory": .array((memory ?? []).map { .string($0) }),
        ]
    }
}
