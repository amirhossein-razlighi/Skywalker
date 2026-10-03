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
    /// Provider-reported token usage per Cloudling (persisted).
    private(set) var usage: [UUID: TokenUsage] = [:]

    @ObservationIgnored private let engine: EngineStore
    @ObservationIgnored private var sessions: [String: LLMSession] = [:]
    /// Tool results not yet delivered to the model (run was stopped / hit the step limit).
    /// They are sent with the next message so the provider history stays valid.
    @ObservationIgnored private var undelivered: [UUID: [ToolOutcome]] = [:]
    @ObservationIgnored private var cancelled: Set<UUID> = []
    @ObservationIgnored private let storeURL: URL

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
        var usage: [UUID: TokenUsage]?
    }

    private func load() {
        if let data = try? Data(contentsOf: storeURL), let saved = try? JSONDecoder().decode(Saved.self, from: data) {
            providers = saved.providers
            cloudlings = saved.cloudlings
            plans = saved.plans
            usage = saved.usage ?? [:]
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
        let saved = Saved(providers: providers, cloudlings: cloudlings, plans: plans, usage: usage)
        if let data = try? JSONEncoder().encode(saved) { try? data.write(to: storeURL, options: .atomic) }
    }

    // MARK: Queries

    func transcript(for c: Cloudling) -> [ChatEntry] { transcripts[c.id] ?? [] }
    func isWorking(_ c: Cloudling) -> Bool { working.contains(c.id) }
    var workingCount: Int { working.count }
    func modelLabel(for c: Cloudling) -> String {
        let provider = providers.first { $0.id == c.providerID } ?? providers.first
        return c.model.isEmpty ? (provider?.defaultModel ?? "no provider") : c.model
    }
    func cloudling(named name: String) -> Cloudling? {
        cloudlings.first { $0.name.caseInsensitiveCompare(name) == .orderedSame }
    }
    /// The crew member behind an actor id: in-editor agents ("agent:Name") or external MCP
    /// agents that connect under a crew member's name ("mcp:Name").
    func cloudling(actor: String) -> Cloudling? {
        for prefix in ["agent:", "mcp:"] where actor.hasPrefix(prefix) {
            return cloudling(named: String(actor.dropFirst(prefix.count)))
        }
        return nil
    }

    func usage(for c: Cloudling) -> TokenUsage { usage[c.id] ?? TokenUsage() }
    var totalUsage: TokenUsage { usage.values.reduce(TokenUsage(), +) }
    func resetUsage(_ c: Cloudling) {
        usage[c.id] = nil
        save()
    }

    /// Drops cached provider sessions so definition changes (instructions, tools, model)
    /// apply to the next message. The visible transcript is kept.
    func applyDefinitionChanges(_ c: Cloudling) {
        guard !working.contains(c.id) else { return }
        sessions = sessions.filter { !$0.key.hasPrefix(c.id.uuidString) }
        undelivered[c.id] = nil
    }

    func resetConversation(_ c: Cloudling) {
        stop(c)
        sessions = sessions.filter { !$0.key.hasPrefix(c.id.uuidString) }
        undelivered[c.id] = nil
        transcripts[c.id] = []
    }

    func stop(_ c: Cloudling) {
        cancelled.insert(c.id)
        // Release any loop suspended on an approval prompt.
        for approval in pendingApprovals where approval.cloudling.id == c.id { resolve(approval, approved: false) }
    }

    private func log(_ c: Cloudling, _ entry: ChatEntry) {
        transcripts[c.id, default: []].append(entry)
    }

    // MARK: Running agents

    /// Sends a message to a Cloudling and runs its tool loop until it is done.
    /// Returns the agent's final text (used for delegation and pipelines).
    @discardableResult
    func send(_ text: String, to c: Cloudling, depth: Int = 0) async -> String {
        // One loop per agent at a time: a second loop would interleave messages in the
        // same provider history (weave / pipelines / delegation can all target a busy agent).
        guard !working.contains(c.id) else {
            log(c, ChatEntry(role: .system, text: "Busy — skipped a request while already working."))
            return "\(c.name) is busy with another task; try again later."
        }
        // Always run with the latest definition (the caller may hold a stale copy).
        let c = cloudlings.first { $0.id == c.id } ?? c
        log(c, ChatEntry(role: .user, text: text))
        working.insert(c.id)
        cancelled.remove(c.id)
        defer {
            working.remove(c.id)
            save()  // persist usage and memory
        }

        let allowDelegation = depth == 0
        let key = "\(c.id.uuidString)|\(allowDelegation)"
        let session: LLMSession
        do {
            session = try sessionFor(c, key: key, allowDelegation: allowDelegation)
        } catch {
            log(c, ChatEntry(role: .error, text: error.localizedDescription, isError: true))
            return "error: \(error.localizedDescription)"
        }

        var outcomes: [ToolOutcome] = undelivered.removeValue(forKey: c.id) ?? []
        var userText: String? = text
        var finalText = ""
        let rounds = max(1, min(c.maxRounds, 200))
        for _ in 0..<rounds {
            if cancelled.contains(c.id) {
                undelivered[c.id] = outcomes
                log(c, ChatEntry(role: .system, text: "Stopped."))
                return finalText.isEmpty ? "stopped" : finalText
            }
            let turn: AssistantTurn
            do {
                turn = try await session.send(userText: userText, toolOutcomes: outcomes)
            } catch {
                log(c, ChatEntry(role: .error, text: error.localizedDescription, isError: true))
                sessions[key] = nil  // history may be inconsistent after a failed request
                return "error: \(error.localizedDescription)"
            }
            userText = nil
            usage[c.id] = (usage[c.id] ?? TokenUsage()) + turn.usage
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
                if cancelled.contains(c.id) {
                    outcomes.append(ToolOutcome(callID: call.id, name: call.name, text: "Cancelled by the user.",
                                                imagesBase64: [], isError: true))
                } else {
                    outcomes.append(await execute(call, by: c, depth: depth))
                }
            }
        }
        undelivered[c.id] = outcomes  // delivered with the next message
        log(c, ChatEntry(role: .system, text: "Reached the step limit (\(rounds) rounds). Send a message to continue."))
        return finalText
    }

    private func sessionFor(_ c: Cloudling, key: String, allowDelegation: Bool) throws -> LLMSession {
        if let s = sessions[key] { return s }
        guard let provider = providers.first(where: { $0.id == c.providerID }) ?? providers.first else {
            throw ProviderError.badResponse("no provider configured")
        }
        let model = c.model.isEmpty ? provider.defaultModel : c.model
        let s = try Providers.makeSession(config: provider, model: model, system: systemPrompt(for: c),
                                          tools: tools(for: c, allowDelegation: allowDelegation))
        sessions[key] = s
        return s
    }

    private func tools(for c: Cloudling, allowDelegation: Bool) -> [AgentTool] {
        var out: [AgentTool] = engineTools().filter { c.access(category: $0.category, readOnly: $0.readOnly) != .off }
        out.append(AgentTool(
            name: "memory_note",
            description: "Save a short note to your long-term memory (kept across conversations and shown to you at " +
                "the start of each one): project conventions, decisions, the human's preferences, unfinished work.",
            inputSchema: ["type": "object", "properties": ["note": ["type": "string", "description": "One concise fact"]],
                          "required": ["note"], "additionalProperties": false],
            readOnly: false))
        out.append(AgentTool(
            name: "memory_forget",
            description: "Remove an outdated note from your long-term memory by its number (1-based, as listed in your instructions).",
            inputSchema: ["type": "object", "properties": ["number": ["type": "integer", "description": "Note number"]],
                          "required": ["number"], "additionalProperties": false],
            readOnly: false))
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

    /// Engine tools with their categories and read-only flags.
    func engineTools() -> [AgentTool] {
        engine.toolList()["tools"].array.map {
            AgentTool(name: $0["name"].string ?? "", description: $0["description"].string ?? "",
                      inputSchema: $0["inputSchema"], readOnly: $0["annotations"]["readOnlyHint"].bool ?? false,
                      category: $0["_meta"]["skywalker/category"].string ?? "scene")
        }
    }

    private func systemPrompt(for c: Cloudling) -> String {
        var extra = ""
        if !c.instructions.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            extra += "\nStanding instructions from the human:\n\(c.instructions)\n"
        }
        if !c.memory.isEmpty {
            extra += "\nYour long-term memory (manage with memory_note / memory_forget):\n" +
                c.memory.enumerated().map { "\($0.offset + 1). \($0.element)" }.joined(separator: "\n") + "\n"
        }
        return """
        You are \(c.name), the \(c.role.title) on a small game-development crew working inside the Skywalker \
        game engine. \(c.mission)
        Personality: \(c.personality.isEmpty ? "friendly and concise" : c.personality)
        \(extra)
        You act on the live game through tools. Every change you make is undoable and shown to the human as \
        done by you. Work loop: understand (scene_overview, selection_get) -> act (use batch for many edits) -> \
        look (viewport_capture; boxes are labelled with #ids) -> verify -> report.
        Conventions: meters, +Y up, entities face -Z, rotations are Euler degrees [pitch, yaw, roll], colors \
        "#rrggbb". Behaviors are written in the Wander language (read wander_reference before writing any).
        Reuse before you rebuild: search project assets with asset_list, look with asset_preview, save reusable \
        groups with prefab_create and shared looks with material_create. Use scatter and place_on_surface for \
        natural placement, viewport_multi to check layouts, and sim_trace to verify behaviors numerically.
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

        if call.name == "memory_note" || call.name == "memory_forget" {
            return remember(call, by: c)
        }

        guard let tool = engineTools().first(where: { $0.name == call.name }) else {
            return ToolOutcome(callID: call.id, name: call.name, text: "Unknown tool \(call.name).", imagesBase64: [], isError: true)
        }
        let access = c.access(category: tool.category, readOnly: tool.readOnly)
        if access == .off {
            return ToolOutcome(callID: call.id, name: call.name,
                               text: "You are not permitted to use \(tool.category) tools. Ask the human or a crew member.",
                               imagesBase64: [], isError: true)
        }
        if access == .ask {
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

    private func remember(_ call: ToolCall, by c: Cloudling) -> ToolOutcome {
        guard let i = cloudlings.firstIndex(where: { $0.id == c.id }) else {
            return ToolOutcome(callID: call.id, name: call.name, text: "unknown agent", imagesBase64: [], isError: true)
        }
        var text: String
        var isError = false
        if call.name == "memory_note" {
            let note = (call.arguments["note"].string ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            if note.isEmpty {
                text = "Empty note."
                isError = true
            } else {
                cloudlings[i].memory.append(String(note.prefix(500)))
                if cloudlings[i].memory.count > 50 { cloudlings[i].memory.removeFirst() }
                text = "Remembered (note \(cloudlings[i].memory.count))."
            }
        } else {
            let n = call.arguments["number"].int ?? 0
            if n >= 1 && n <= cloudlings[i].memory.count {
                let removed = cloudlings[i].memory.remove(at: n - 1)
                text = "Forgot: \(removed)"
            } else {
                text = "No note number \(n)."
                isError = true
            }
        }
        log(c, ChatEntry(role: .tool, text: text, toolName: call.name, isError: isError))
        save()
        return ToolOutcome(callID: call.id, name: call.name, text: text, imagesBase64: [], isError: isError)
    }

    // MARK: Project agents (agents/*.agent.json)

    func agentFiles(in project: URL) -> [URL] {
        let dir = project.appending(path: "agents", directoryHint: .isDirectory)
        let files = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? []
        return files.filter { $0.lastPathComponent.hasSuffix(".agent.json") }.sorted { $0.path < $1.path }
    }

    @discardableResult
    func exportAgent(_ c: Cloudling, to project: URL) throws -> URL {
        let def = AgentDefinition(c, providerName: providers.first { $0.id == c.providerID }?.name)
        let slug = c.name.lowercased().map { $0.isLetter || $0.isNumber ? String($0) : "-" }.joined()
        let dir = project.appending(path: "agents", directoryHint: .isDirectory)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let url = dir.appending(path: "\(slug).agent.json")
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(def).write(to: url, options: .atomic)
        return url
    }

    /// Adds (or updates, by name) a crew member from an agent file.
    @discardableResult
    func importAgent(from url: URL) throws -> Cloudling {
        let def = try JSONDecoder().decode(AgentDefinition.self, from: Data(contentsOf: url))
        var c = def.cloudling(providers: providers)
        if let i = cloudlings.firstIndex(where: { $0.name.caseInsensitiveCompare(def.name) == .orderedSame }) {
            c.id = cloudlings[i].id
            c.providerID = cloudlings[i].providerID ?? c.providerID
            cloudlings[i] = c
            applyDefinitionChanges(c)
        } else {
            cloudlings.append(c)
        }
        save()
        return c
    }

    func resolve(_ approval: PendingApproval, approved: Bool) {
        // Resume exactly once, even if the button is clicked twice before the UI updates.
        guard let index = pendingApprovals.firstIndex(where: { $0.id == approval.id }) else { return }
        let pending = pendingApprovals.remove(at: index)
        pending.resume.resume(returning: approved)
    }

    // MARK: Pipelines ("flight plans")

    func run(plan: FlightPlan, goal: String) async {
        runningPlan = true
        defer { runningPlan = false }
        planLog = ["Goal: \(goal)"]
        var notes: [String] = []
        for (i, stage) in Self.stages(of: plan).enumerated() {
            let members = stage.compactMap { step in cloudlings.first { $0.id == step.cloudlingID }.map { (step, $0) } }
            guard !members.isEmpty else { continue }
            if members.count == 1 {
                planLog.append("Stage \(i + 1): \(members[0].1.name) — \(members[0].0.instruction)")
            } else {
                planLog.append("Stage \(i + 1) (parallel): " + members.map(\.1.name).joined(separator: " + "))
            }
            let handoff = notes.isEmpty ? "" : "\n\nNotes from earlier steps:\n" + notes.joined(separator: "\n")
            // Agents in a stage work at the same time: their loops interleave on the main actor
            // while each waits for its model, and every edit is still one attributed transaction.
            var tasks: [Task<String, Never>] = []
            for (step, member) in members {
                var prompt = "Goal: \(goal)\n\nYour step: \(step.instruction)\(handoff)"
                if members.count > 1 {
                    let others = members.filter { $0.1.id != member.id }.map { "\($0.1.name) (\($0.0.instruction))" }
                    prompt += "\n\nWorking in parallel with: \(others.joined(separator: "; ")). Stay within your step."
                }
                let message = prompt
                tasks.append(Task { await self.send(message, to: member, depth: 1) })
            }
            var reports: [String] = []
            for task in tasks { reports.append(await task.value) }
            for (pair, report) in zip(members, reports) {
                notes.append("- \(pair.1.name): \(report)")
                planLog.append("  ↳ \(pair.1.name): \(report)")
            }
        }
        planLog.append("Flight plan complete.")
    }

    /// Groups steps into stages: a step marked parallel joins the previous step's stage.
    static func stages(of plan: FlightPlan) -> [[FlightStep]] {
        var out: [[FlightStep]] = []
        for step in plan.steps {
            if step.parallelWithPrevious, !out.isEmpty { out[out.count - 1].append(step) } else { out.append([step]) }
        }
        return out
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
