import Foundation

/// A tool the model may call (mirrors the engine's MCP tool definitions).
struct AgentTool: Sendable {
    var name: String
    var description: String
    var inputSchema: JSON
    var readOnly: Bool
}

struct ToolCall: Sendable, Identifiable {
    var id: String
    var name: String
    var arguments: JSON
}

struct ToolOutcome: Sendable {
    var callID: String
    var name: String
    var text: String
    var imagesBase64: [String]
    var isError: Bool
}

enum StopKind: Sendable {
    case done, toolUse, maxTokens, refusal(String), other(String)
}

struct AssistantTurn: Sendable {
    var text: String
    var toolCalls: [ToolCall]
    var stop: StopKind
}

enum ProviderError: LocalizedError {
    case missingKey(String)
    case http(Int, String)
    case badResponse(String)

    var errorDescription: String? {
        switch self {
        case let .missingKey(p): "No API key set for \(p). Add one in Settings → Providers."
        case let .http(code, body): "HTTP \(code): \(body.prefix(400))"
        case let .badResponse(why): "Unexpected response: \(why)"
        }
    }
}

/// A stateful conversation with one model. Sessions own their provider-native history,
/// which is strictly append-only (required for preserved thinking on current Claude
/// models, and good for prompt caching everywhere).
@MainActor
protocol LLMSession: AnyObject {
    func send(userText: String?, toolOutcomes: [ToolOutcome]) async throws -> AssistantTurn
}

enum Providers {
    @MainActor
    static func makeSession(config: ProviderConfig, model: String, system: String, tools: [AgentTool]) throws -> LLMSession {
        let key = Keychain.get(account: config.keychainAccount) ?? ""
        switch config.kind {
        case .anthropic:
            if key.isEmpty { throw ProviderError.missingKey(config.name) }
            return AnthropicSession(config: config, apiKey: key, model: model, system: system, tools: tools)
        case .openAICompatible:
            return OpenAICompatibleSession(config: config, apiKey: key, model: model, system: system, tools: tools)
        }
    }

    static func post(_ url: URL, headers: [String: String], body: JSON) async throws -> JSON {
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.timeoutInterval = 600  // long agentic turns are normal
        req.setValue("application/json", forHTTPHeaderField: "content-type")
        for (k, v) in headers { req.setValue(v, forHTTPHeaderField: k) }
        req.httpBody = Data(body.serialized().utf8)
        let (data, response) = try await URLSession.shared.data(for: req)
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        guard (200..<300).contains(status) else {
            throw ProviderError.http(status, String(decoding: data, as: UTF8.self))
        }
        guard let json = JSON.parse(data) else { throw ProviderError.badResponse("invalid JSON") }
        return json
    }
}

// MARK: - Anthropic Messages API (raw HTTP; Swift has no official SDK)

@MainActor
final class AnthropicSession: LLMSession {
    private let config: ProviderConfig
    private let apiKey: String
    private let model: String
    private let system: String
    private let tools: [AgentTool]
    private var messages: [JSON] = []

    init(config: ProviderConfig, apiKey: String, model: String, system: String, tools: [AgentTool]) {
        self.config = config
        self.apiKey = apiKey
        self.model = model
        self.system = system
        self.tools = tools
    }

    /// Server-side refusal fallback is available on these model families.
    private var supportsFallback: Bool {
        ["claude-opus-5-5", "claude-opus-5", "claude-fable-5-1", "claude-sonnet-5-5"].contains(model)
    }

    func send(userText: String?, toolOutcomes: [ToolOutcome]) async throws -> AssistantTurn {
        var content: [JSON] = toolOutcomes.map { outcome in
            var blocks: [JSON] = [["type": "text", "text": .string(outcome.text.isEmpty ? "(no output)" : outcome.text)]]
            for img in outcome.imagesBase64 {
                blocks.append(["type": "image",
                               "source": ["type": "base64", "media_type": "image/png", "data": .string(img)]])
            }
            return ["type": "tool_result", "tool_use_id": .string(outcome.callID), "content": .array(blocks),
                    "is_error": .bool(outcome.isError)]
        }
        if let userText, !userText.isEmpty { content.append(["type": "text", "text": .string(userText)]) }
        messages.append(["role": "user", "content": .array(content)])

        var body: JSON = [
            "model": .string(model),
            "max_tokens": 16000,
            "system": .string(system),
            "tools": .array(tools.map { ["name": .string($0.name), "description": .string($0.description),
                                         "input_schema": $0.inputSchema] }),
            "messages": .array(messages),
            "output_config": ["effort": "high"],
            "cache_control": ["type": "ephemeral"],  // cache tools + system + history prefix
        ]
        var headers = ["x-api-key": apiKey, "anthropic-version": "2023-06-01"]
        if supportsFallback {
            body.set("fallbacks", "default")
            headers["anthropic-beta"] = "server-side-fallback-2026-07-01"
        }
        let base = config.baseURL.hasSuffix("/") ? String(config.baseURL.dropLast()) : config.baseURL
        let response = try await Providers.post(URL(string: base + "/v1/messages")!, headers: headers, body: body)

        // Append the assistant content exactly as returned (thinking blocks included).
        let assistantContent = response["content"]
        messages.append(["role": "assistant", "content": assistantContent])

        var text = ""
        var calls: [ToolCall] = []
        for block in assistantContent.array {
            switch block["type"].string {
            case "text": text += block["text"].string ?? ""
            case "tool_use":
                calls.append(ToolCall(id: block["id"].string ?? UUID().uuidString, name: block["name"].string ?? "",
                                      arguments: block["input"]))
            default: break
            }
        }
        let stop: StopKind
        switch response["stop_reason"].string {
        case "tool_use": stop = .toolUse
        case "end_turn", "stop_sequence": stop = .done
        case "max_tokens": stop = .maxTokens
        case "refusal": stop = .refusal(response["stop_details"]["explanation"].string ?? "declined")
        case let other: stop = .other(other ?? "unknown")
        }
        return AssistantTurn(text: text, toolCalls: calls, stop: calls.isEmpty ? stop : .toolUse)
    }
}

// MARK: - OpenAI-compatible Chat Completions (OpenAI, DeepSeek, Ollama, LM Studio, vLLM, ...)

@MainActor
final class OpenAICompatibleSession: LLMSession {
    private let config: ProviderConfig
    private let apiKey: String
    private let model: String
    private let tools: [AgentTool]
    private var messages: [JSON]

    init(config: ProviderConfig, apiKey: String, model: String, system: String, tools: [AgentTool]) {
        self.config = config
        self.apiKey = apiKey
        self.model = model
        self.tools = tools
        messages = [["role": "system", "content": .string(system)]]
    }

    func send(userText: String?, toolOutcomes: [ToolOutcome]) async throws -> AssistantTurn {
        var images: [(String, String)] = []
        for outcome in toolOutcomes {
            messages.append(["role": "tool", "tool_call_id": .string(outcome.callID),
                             "content": .string(outcome.isError ? "ERROR: " + outcome.text : outcome.text)])
            for img in outcome.imagesBase64 { images.append((outcome.name, img)) }
        }
        // Tool messages cannot carry images in this protocol, so screenshots follow as a user message.
        if !images.isEmpty, config.supportsVision {
            var parts: [JSON] = [["type": "text", "text": "Images returned by the tools above:"]]
            for (_, img) in images {
                parts.append(["type": "image_url", "image_url": ["url": .string("data:image/png;base64," + img)]])
            }
            messages.append(["role": "user", "content": .array(parts)])
        }
        if let userText, !userText.isEmpty { messages.append(["role": "user", "content": .string(userText)]) }

        let body: JSON = [
            "model": .string(model),
            "messages": .array(messages),
            "tools": .array(tools.map {
                ["type": "function", "function": ["name": .string($0.name), "description": .string($0.description),
                                                  "parameters": $0.inputSchema]]
            }),
        ]
        var headers: [String: String] = [:]
        if !apiKey.isEmpty { headers["authorization"] = "Bearer \(apiKey)" }
        let base = config.baseURL.hasSuffix("/") ? String(config.baseURL.dropLast()) : config.baseURL
        let response = try await Providers.post(URL(string: base + "/chat/completions")!, headers: headers, body: body)

        let choice = response["choices"][0]
        let message = choice["message"]
        guard !message.isNull else { throw ProviderError.badResponse("no choices") }
        messages.append(message)
        let calls = message["tool_calls"].array.map { call in
            ToolCall(id: call["id"].string ?? UUID().uuidString, name: call["function"]["name"].string ?? "",
                     arguments: JSON.parse(call["function"]["arguments"].string ?? "{}") ?? [:])
        }
        let stop: StopKind
        switch choice["finish_reason"].string {
        case "tool_calls": stop = .toolUse
        case "length": stop = .maxTokens
        case "content_filter": stop = .refusal("content filter")
        default: stop = .done
        }
        return AssistantTurn(text: message["content"].string ?? "", toolCalls: calls, stop: calls.isEmpty ? stop : .toolUse)
    }
}
