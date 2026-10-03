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

/// A crew role: what the Cloudling is good at and how it should work.
struct CrewRole: Codable, Hashable, Identifiable, Sendable {
    var id: String
    var title: String
    var symbol: String
    var mission: String

    static let all: [CrewRole] = [
        CrewRole(id: "director", title: "Creative Director", symbol: "star.fill",
                 mission: "You own the vision. Break goals into tasks, delegate them to the crew with crew_delegate, review results with viewport_capture, and keep the game coherent and fun."),
        CrewRole(id: "level", title: "Level Designer", symbol: "map.fill",
                 mission: "You build spaces: layout, composition, scale, paths and landmarks. Use batch to place many entities at once and check your work visually."),
        CrewRole(id: "gameplay", title: "Gameplay Programmer", symbol: "chevron.left.forwardslash.chevron.right",
                 mission: "You turn intents into Wander behaviors. Read wander_reference first, validate with wander_check, attach with behavior_set, and verify with sim_control step + sim_input."),
        CrewRole(id: "lighting", title: "Lighting Artist", symbol: "sun.max.fill",
                 mission: "You set mood with environment_update (sun, sky, fog, exposure) and light entities. Always compare before/after captures."),
        CrewRole(id: "writer", title: "Writer", symbol: "pencil",
                 mission: "You write names, lore, dialogue and quest text. Store text in entity vars (e.g. vars.dialogue) and keep tone consistent."),
        CrewRole(id: "artist", title: "Asset Artist", symbol: "paintpalette.fill",
                 mission: "You give things their look: colors, materials, and requests for generated meshes, textures and sprites via asset_request."),
        CrewRole(id: "qa", title: "QA Tester", symbol: "checkmark.seal.fill",
                 mission: "You break things. Play the game with sim_control/sim_input, read logs, and report precise, reproducible issues. Do not fix things unless asked."),
    ]

    static func named(_ id: String) -> CrewRole { all.first { $0.id == id } ?? all[1] }
}

/// How much freedom a Cloudling has.
enum Autonomy: String, Codable, CaseIterable, Identifiable, Sendable {
    case observe   // read-only tools only
    case ask       // mutating tools need your approval
    case auto      // acts freely (everything is undoable)

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
        ToolCategory(id: "network", title: "Network", symbol: "arrow.down.circle", detail: "download licensed assets (asks by default)"),
        ToolCategory(id: "dcc", title: "Design apps", symbol: "wand.and.stars", detail: "run Blender / Maya / Houdini scripts, convert and edit models (asks by default)"),
    ]
}

/// Tokens a Cloudling has used (reported by the provider).
struct TokenUsage: Codable, Hashable, Sendable {
    var input = 0
    var output = 0
    var cacheRead = 0
    var requests = 0

    static func + (a: TokenUsage, b: TokenUsage) -> TokenUsage {
        TokenUsage(input: a.input + b.input, output: a.output + b.output, cacheRead: a.cacheRead + b.cacheRead,
                   requests: a.requests + b.requests)
    }
}

/// A crew member.
struct Cloudling: Codable, Identifiable, Hashable, Sendable {
    var id = UUID()
    var name: String
    var roleID: String
    var colorHex: String
    var face: CloudFace
    var providerID: UUID?
    var model: String = ""
    var autonomy: Autonomy = .auto
    var personality: String = ""
    /// Extra standing instructions, appended to the role's mission.
    var instructions: String = ""
    /// Replaces the role's built-in mission when not empty.
    var missionOverride: String = ""
    var maxRounds: Int = 40
    var toolAccess: [String: ToolAccess] = [:]
    /// Long-term notes the Cloudling keeps across conversations (memory_note tool).
    var memory: [String] = []

    var mission: String { missionOverride.isEmpty ? role.mission : missionOverride }

    /// Effective access for a tool category; read-only tools are never "ask".
    func access(category: String, readOnly: Bool) -> ToolAccess {
        let explicit = toolAccess[category] ?? .inherit
        if explicit == .off { return .off }
        if readOnly { return .allow }
        if explicit != .inherit { return explicit }
        // Reaching outside the project (downloads, running design apps and their scripts) needs the
        // human's OK unless explicitly allowed.
        if category == "network" || category == "dcc" { return autonomy == .observe ? .off : .ask }
        switch autonomy {
        case .observe: return .off
        case .ask: return .ask
        case .auto: return .allow
        }
    }

    var role: CrewRole { CrewRole.named(roleID) }
    var color: Color { Color(hex: colorHex) }
    var actorName: String { "agent:\(name)" }

    static func starterCrew(provider: UUID?) -> [Cloudling] {
        [
            Cloudling(name: "Nimbus", roleID: "director", colorHex: "#8b73fa", face: .determined, providerID: provider,
                      personality: "Warm, decisive, big-picture."),
            Cloudling(name: "Cirro", roleID: "level", colorHex: "#5c8fed", face: .happy, providerID: provider,
                      personality: "Loves vistas and secret paths."),
            Cloudling(name: "Stratus", roleID: "gameplay", colorHex: "#54ccad", face: .focused, providerID: provider,
                      personality: "Precise and test-driven."),
            Cloudling(name: "Aurora", roleID: "lighting", colorHex: "#ffb873", face: .dreamy, providerID: provider,
                      personality: "Thinks in golden hours."),
            Cloudling(name: "Haze", roleID: "writer", colorHex: "#ed6b7a", face: .wink, providerID: provider,
                      personality: "Whimsical, never wordy."),
        ]
    }
}

extension Cloudling {
    // Tolerant decoding: crews saved by older versions lack the newer fields.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decodeIfPresent(UUID.self, forKey: .id) ?? UUID()
        name = try c.decode(String.self, forKey: .name)
        roleID = try c.decodeIfPresent(String.self, forKey: .roleID) ?? "level"
        colorHex = try c.decodeIfPresent(String.self, forKey: .colorHex) ?? "#7fb3ff"
        face = try c.decodeIfPresent(CloudFace.self, forKey: .face) ?? .happy
        providerID = try c.decodeIfPresent(UUID.self, forKey: .providerID)
        model = try c.decodeIfPresent(String.self, forKey: .model) ?? ""
        autonomy = try c.decodeIfPresent(Autonomy.self, forKey: .autonomy) ?? .auto
        personality = try c.decodeIfPresent(String.self, forKey: .personality) ?? ""
        instructions = try c.decodeIfPresent(String.self, forKey: .instructions) ?? ""
        missionOverride = try c.decodeIfPresent(String.self, forKey: .missionOverride) ?? ""
        maxRounds = try c.decodeIfPresent(Int.self, forKey: .maxRounds) ?? 40
        toolAccess = try c.decodeIfPresent([String: ToolAccess].self, forKey: .toolAccess) ?? [:]
        memory = try c.decodeIfPresent([String].self, forKey: .memory) ?? []
    }
}

/// A Cloudling as a shareable project asset (`agents/<name>.agent.json`): everything except
/// machine-specific bits (provider ids, API keys).
struct AgentDefinition: Codable, Sendable {
    var format = "skywalker.agent"
    var version = 1
    var name: String
    var role: String
    var color: String
    var face: CloudFace
    var provider: String?
    var model: String
    var autonomy: Autonomy
    var personality: String
    var mission: String
    var instructions: String
    var maxRounds: Int
    var toolAccess: [String: ToolAccess]
    var memory: [String]

    init(_ c: Cloudling, providerName: String?) {
        name = c.name
        role = c.roleID
        color = c.colorHex
        face = c.face
        provider = providerName
        model = c.model
        autonomy = c.autonomy
        personality = c.personality
        mission = c.missionOverride
        instructions = c.instructions
        maxRounds = c.maxRounds
        toolAccess = c.toolAccess
        memory = c.memory
    }

    func cloudling(providers: [ProviderConfig]) -> Cloudling {
        var c = Cloudling(name: name, roleID: role, colorHex: color, face: face,
                          providerID: providers.first { $0.name == provider }?.id ?? providers.first?.id)
        c.model = model
        c.autonomy = autonomy
        c.personality = personality
        c.missionOverride = mission
        c.instructions = instructions
        c.maxRounds = maxRounds
        c.toolAccess = toolAccess
        c.memory = memory
        return c
    }
}

/// A step of a pipeline ("flight plan"): one Cloudling, one instruction.
struct FlightStep: Codable, Identifiable, Hashable, Sendable {
    var id = UUID()
    var cloudlingID: UUID
    var instruction: String
    /// Runs at the same time as the previous step (both see the same earlier notes).
    var parallelWithPrevious = false

    init(cloudlingID: UUID, instruction: String, parallelWithPrevious: Bool = false) {
        self.cloudlingID = cloudlingID
        self.instruction = instruction
        self.parallelWithPrevious = parallelWithPrevious
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decodeIfPresent(UUID.self, forKey: .id) ?? UUID()
        cloudlingID = try c.decode(UUID.self, forKey: .cloudlingID)
        instruction = try c.decodeIfPresent(String.self, forKey: .instruction) ?? ""
        parallelWithPrevious = try c.decodeIfPresent(Bool.self, forKey: .parallelWithPrevious) ?? false
    }
}

/// A pipeline. Steps run in order; consecutive steps marked parallel run together. Each
/// stage receives the goal plus the reports of all earlier stages.
struct FlightPlan: Codable, Identifiable, Hashable, Sendable {
    var id = UUID()
    var name: String
    var steps: [FlightStep]
}
