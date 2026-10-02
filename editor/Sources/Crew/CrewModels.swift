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

/// A step of a pipeline ("flight plan"): one Cloudling, one instruction.
struct FlightStep: Codable, Identifiable, Hashable, Sendable {
    var id = UUID()
    var cloudlingID: UUID
    var instruction: String
}

/// An ordered pipeline. Each step receives the goal plus the previous steps' reports.
struct FlightPlan: Codable, Identifiable, Hashable, Sendable {
    var id = UUID()
    var name: String
    var steps: [FlightStep]
}

extension Color {
    init(hex: String) {
        var s = hex.trimmingCharacters(in: .whitespaces)
        if s.hasPrefix("#") { s.removeFirst() }
        let v = UInt64(s, radix: 16) ?? 0x5c8fed
        self.init(red: Double((v >> 16) & 0xff) / 255, green: Double((v >> 8) & 0xff) / 255, blue: Double(v & 0xff) / 255)
    }
}
