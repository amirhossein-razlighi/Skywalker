import Foundation
import Observation

/// One line in a Cloudling's chat.
struct ChatEntry: Identifiable, Sendable {
    enum Role: Sendable { case user, agent, tool, system, error }
    let id = UUID()
    var role: Role
    var text: String
    var toolName: String? = nil
    var imageBase64: String? = nil
    var isError = false
}

/// A mutating tool call waiting for the human's OK (autonomy = .ask).
struct PendingApproval: Identifiable {
    let id = UUID()
    let cloudling: Cloudling
    let call: ToolCall
    let resume: CheckedContinuation<Bool, Never>
}

/// Owns the crew, their conversations and pipelines, and runs the agent loops.
@MainActor
@Observable
final class CrewStore {
    var providers: [ProviderConfig] = []
    var cloudlings: [Cloudling] = []
    var plans: [FlightPlan] = []
    private(set) var transcripts: [UUID: [ChatEntry]] = [:]
    private(set) var working: Set<UUID> = []
    var pendingApprovals: [PendingApproval] = []
    var planLog: [String] = []
    private(set) var runningPlan = false

    @ObservationIgnored private let engine: EngineStore
    @ObservationIgnored private var sessions: [UUID: LLMSession] = [:]
    @ObservationIgnored private var cancelled: Set<UUID> = []
    @ObservationIgnored private let storeURL: URL

    static let maxRounds = 40

    init(engine: EngineStore) {
        self.engine = engine
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appending(path: "Skywalker", directoryHint: .isDirectory)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        storeURL = dir.appending(path: "crew.json")
        load()
    }

    // MARK: Persistence

    private struct Saved: Codable {
        var providers: [ProviderConfig]
        var cloudlings: [Cloudling]
        var plans: [FlightPlan]
    }

    private func load() {
        if let data = try? Data(contentsOf: storeURL), let saved = try? JSONDecoder().decode(Saved.self, from: data) {
            providers = saved.providers
            cloudlings = saved.cloudlings
            plans = saved.plans
            return
        }
        providers = ProviderConfig.presets
        cloudlings = Cloudling.starterCrew(provider: providers.first?.id)
        if cloudlings.count >= 4 {
            plans = [FlightPlan(name: "Build a level", steps: [
                FlightStep(cloudlingID: cloudlings[1].id, instruction: "Lay out the level for the goal. Keep it readable from the main camera."),
                FlightStep(cloudlingID: cloudlings[2].id, instruction: "Add the gameplay behaviors the goal needs and test them with sim_control step."),
                FlightStep(cloudlingID: cloudlings[3].id, instruction: "Light the scene to match the mood of the goal."),
            ])]
        }
    }

    func save() {
        let saved = Saved(providers: providers, cloudlings: cloudlings, plans: plans)
        if let data = try? JSONEncoder().encode(saved) { try? data.write(to: storeURL, options: .atomic) }
    }

    // MARK: Queries

    func transcript(for c: Cloudling) -> [ChatEntry] { transcripts[c.id] ?? [] }
    func isWorking(_ c: Cloudling) -> Bool { working.contains(c.id) }
    func cloudling(named name: String) -> Cloudling? {
        cloudlings.first { $0.name.caseInsensitiveCompare(name) == .orderedSame }
    }
    func cloudling(actor: String) -> Cloudling? {
        guard actor.hasPrefix("agent:") else { return nil }
        return cloudling(named: String(actor.dropFirst(6)))
    }

    func resetConversation(_ c: Cloudling) {
        sessions[c.id] = nil
        transcripts[c.id] = []
    }

    func stop(_ c: Cloudling) { cancelled.insert(c.id) }

    private func log(_ c: Cloudling, _ entry: ChatEntry) {
        transcripts[c.id, default: []].append(entry)
    }

    // MARK: Running agents

    /// Sends a message to a Cloudling and runs its tool loop until it is done.
    /// Returns the agent's final text (used for delegation and pipelines).
    @discardableResult
    func send(_ text: String, to c: Cloudling, depth: Int = 0) async -> String {
        log(c, ChatEntry(role: .user, text: text))
        working.insert(c.id)
        cancelled.remove(c.id)
        defer { working.remove(c.id) }

        let session: LLMSession
        do {
            session = try sessionFor(c, allowDelegation: depth == 0)
        } catch {
            log(c, ChatEntry(role: .error, text: error.localizedDescription, isError: true))
            return "error: \(error.localizedDescription)"
        }

        var outcomes: [ToolOutcome] = []
        var userText: String? = text
        var finalText = ""
        for _ in 0..<Self.maxRounds {
            if cancelled.contains(c.id) {
                log(c, ChatEntry(role: .system, text: "Stopped."))
                return finalText.isEmpty ? "stopped" : finalText
            }
            let turn: AssistantTurn
            do {
                turn = try await session.send(userText: userText, toolOutcomes: outcomes)
            } catch {
                log(c, ChatEntry(role: .error, text: error.localizedDescription, isError: true))
                sessions[c.id] = nil  // history may be inconsistent after a failed request
                return "error: \(error.localizedDescription)"
            }
            userText = nil
            if !turn.text.isEmpty {
                log(c, ChatEntry(role: .agent, text: turn.text))
                finalText = turn.text
            }
            switch turn.stop {
            case .refusal(let why):
                log(c, ChatEntry(role: .error, text: "The model declined: \(why)", isError: true))
                return finalText
            case .maxTokens where turn.toolCalls.isEmpty:
                log(c, ChatEntry(role: .system, text: "Reply was cut off (max tokens)."))
                return finalText
            default: break
            }
            if turn.toolCalls.isEmpty { return finalText }
            outcomes = []
            for call in turn.toolCalls {
                outcomes.append(await execute(call, by: c, depth: depth))
            }
        }
        log(c, ChatEntry(role: .system, text: "Reached the step limit (\(Self.maxRounds) rounds)."))
        return finalText
    }

    private func sessionFor(_ c: Cloudling, allowDelegation: Bool) throws -> LLMSession {
        if let s = sessions[c.id] { return s }
        guard let provider = providers.first(where: { $0.id == c.providerID }) ?? providers.first else {
            throw ProviderError.badResponse("no provider configured")
        }
        let model = c.model.isEmpty ? provider.defaultModel : c.model
        let s = try Providers.makeSession(config: provider, model: model, system: systemPrompt(for: c),
                                          tools: tools(for: c, allowDelegation: allowDelegation))
        sessions[c.id] = s
        return s
    }

    private func tools(for c: Cloudling, allowDelegation: Bool) -> [AgentTool] {
        var out: [AgentTool] = engine.toolList()["tools"].array.map {
            AgentTool(name: $0["name"].string ?? "", description: $0["description"].string ?? "",
                      inputSchema: $0["inputSchema"], readOnly: $0["annotations"]["readOnlyHint"].bool ?? false)
        }
        if c.autonomy == .observe { out = out.filter(\.readOnly) }
        if allowDelegation && c.roleID == "director" {
            out.append(AgentTool(
                name: "crew_delegate",
                description: "Give a task to a crew member and wait for their report. Members: " +
                    cloudlings.filter { $0.id != c.id }.map { "\($0.name) (\($0.role.title))" }.joined(separator: ", ") +
                    ". Be specific about what done looks like.",
                inputSchema: ["type": "object",
                              "properties": ["member": ["type": "string", "description": "Crew member name"],
                                             "task": ["type": "string", "description": "The task"]],
                              "required": ["member", "task"], "additionalProperties": false],
                readOnly: false))
        }
        return out
    }

    private func systemPrompt(for c: Cloudling) -> String {
        """
        You are \(c.name), the \(c.role.title) on a small game-development crew working inside the Skywalker \
        game engine. \(c.role.mission)
        Personality: \(c.personality.isEmpty ? "friendly and concise" : c.personality)

        You act on the live game through tools. Every change you make is undoable and shown to the human as \
        done by you. Work loop: understand (scene_overview, selection_get) -> act (use batch for many edits) -> \
        look (viewport_capture; boxes are labelled with #ids) -> verify -> report.
        Conventions: meters, +Y up, entities face -Z, rotations are Euler degrees [pitch, yaw, roll], colors \
        "#rrggbb". Behaviors are written in the Wander language (read wander_reference before writing any).
        When you finish, reply with two or three sentences on what you changed and anything the human should check.
        """
    }

    private func execute(_ call: ToolCall, by c: Cloudling, depth: Int) async -> ToolOutcome {
        log(c, ChatEntry(role: .tool, text: call.arguments.serialized(), toolName: call.name))

        if call.name == "crew_delegate" {
            let memberName = call.arguments["member"].string ?? ""
            guard let member = cloudling(named: memberName), member.id != c.id else {
                return ToolOutcome(callID: call.id, name: call.name, text: "No crew member named \(memberName).",
                                   imagesBase64: [], isError: true)
            }
            let report = await send(call.arguments["task"].string ?? "", to: member, depth: depth + 1)
            return ToolOutcome(callID: call.id, name: call.name, text: "\(member.name) reports: \(report)",
                               imagesBase64: [], isError: false)
        }

        let readOnly = engine.toolList()["tools"].array
            .first { $0["name"].string == call.name }?["annotations"]["readOnlyHint"].bool ?? false
        if c.autonomy == .ask && !readOnly {
            let approved = await withCheckedContinuation { cont in
                pendingApprovals.append(PendingApproval(cloudling: c, call: call, resume: cont))
            }
            if !approved {
                log(c, ChatEntry(role: .system, text: "You declined \(call.name)."))
                return ToolOutcome(callID: call.id, name: call.name, text: "The human declined this action. Ask or try something else.",
                                   imagesBase64: [], isError: true)
            }
        }

        let result = engine.call(call.name, call.arguments, actor: c.actorName)
        log(c, ChatEntry(role: .tool, text: String(result.text.prefix(1200)), toolName: call.name,
                         imageBase64: result.imagesBase64.first, isError: result.isError))
        return ToolOutcome(callID: call.id, name: call.name, text: result.text, imagesBase64: result.imagesBase64,
                           isError: result.isError)
    }

    func resolve(_ approval: PendingApproval, approved: Bool) {
        pendingApprovals.removeAll { $0.id == approval.id }
        approval.resume.resume(returning: approved)
    }

    // MARK: Pipelines ("flight plans")

    func run(plan: FlightPlan, goal: String) async {
        runningPlan = true
        defer { runningPlan = false }
        planLog = ["Goal: \(goal)"]
        var notes: [String] = []
        for (i, step) in plan.steps.enumerated() {
            guard let member = cloudlings.first(where: { $0.id == step.cloudlingID }) else { continue }
            planLog.append("Step \(i + 1): \(member.name) — \(step.instruction)")
            let handoff = notes.isEmpty ? "" : "\n\nNotes from earlier steps:\n" + notes.joined(separator: "\n")
            let report = await send("Goal: \(goal)\n\nYour step: \(step.instruction)\(handoff)", to: member, depth: 1)
            notes.append("- \(member.name): \(report)")
            planLog.append("  ↳ \(report)")
        }
        planLog.append("Flight plan complete.")
    }

    // MARK: Weave (intent -> Wander)

    /// Asks the gameplay Cloudling to turn a behavior's natural-language intent into Wander code.
    func weave(entity: UInt64, entityName: String, behavior: String, intent: String) async {
        guard let coder = cloudlings.first(where: { $0.roleID == "gameplay" }) ?? cloudlings.first else { return }
        await send("""
            Write the Wander behavior "\(behavior)" for entity #\(entity) (\(entityName)).
            Intent: \(intent)
            Steps: read wander_reference if you have not yet, write the code, check it with wander_check, then save it \
            with behavior_set (entity \(entity), name "\(behavior)", intent exactly as given). Then step the simulation \
            to verify it does what the intent says, and stop the simulation.
            """, to: coder, depth: 1)
    }
}
