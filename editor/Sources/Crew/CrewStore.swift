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

/// A mutating tool call waiting for the human's OK (autonomy = ask).
struct PendingApproval: Identifiable {
    let id = UUID()
    let cloudling: Cloudling
    let call: ToolCall
    let resume: CheckedContinuation<Bool, Never>
}

/// Runs the studio's agents inside the editor: their conversations, provider sessions,
/// approvals, and studio loops. The roster itself (and the board, feedback, loops, usage)
/// lives in the engine's studio, shared with the headless runner and external agents; this
/// store adds what only the editor has — API keys in the Keychain and a human to approve.
@MainActor
@Observable
final class CrewStore {
    var providers: [ProviderConfig] = []
    private(set) var transcripts: [String: [ChatEntry]] = [:]
    private(set) var working: Set<String> = []
    var pendingApprovals: [PendingApproval] = []
    /// Loop currently driven by the in-editor crew, and its progress log.
    private(set) var runningLoop: String?
    private(set) var loopLog: [String] = []
    private(set) var lastError: String?

    @ObservationIgnored let engine: EngineStore
    @ObservationIgnored let studio: StudioStore
    @ObservationIgnored private var sessions: [String: LLMSession] = [:]
    /// Tool results not yet delivered to the model (run was stopped / hit the step limit).
    /// They are sent with the next message so the provider history stays valid.
    @ObservationIgnored private var undelivered: [String: [ToolOutcome]] = [:]
    @ObservationIgnored private var cancelled: Set<String> = []
    @ObservationIgnored private var loopCancelled = false
    /// Machine-specific provider choice per agent (agent files only carry a provider name).
    @ObservationIgnored private var providerOverrides: [String: UUID] = [:]
    @ObservationIgnored private let storeURL: URL

    init(engine: EngineStore, studio: StudioStore) {
        self.engine = engine
        self.studio = studio
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appending(path: "Skywalker", directoryHint: .isDirectory)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        storeURL = dir.appending(path: "crew.json")
        load()
        if studio.agents.isEmpty {
            engine.call("studio_team_template", ["template": "starter_crew"], actor: "editor")
            studio.refresh()
        }
    }

    // MARK: Persistence (providers and local choices only; the roster is in the project)

    private struct Saved: Codable {
        var providers: [ProviderConfig]
        var providerOverrides: [String: UUID]?
    }

    private struct LegacySaved: Decodable {
        var cloudlings: [LegacyCloudling]?
    }

    private func load() {
        guard let data = try? Data(contentsOf: storeURL) else {
            providers = ProviderConfig.presets
            return
        }
        if let saved = try? JSONDecoder().decode(Saved.self, from: data) {
            providers = saved.providers
            providerOverrides = saved.providerOverrides ?? [:]
        } else {
            providers = ProviderConfig.presets
        }
        // Crews from earlier editor versions lived in Application Support: move them into the
        // project roster once (agents/<id>.agent.json), then forget the old copy.
        if let legacy = try? JSONDecoder().decode(LegacySaved.self, from: data), let old = legacy.cloudlings, !old.isEmpty {
            if studio.agents.isEmpty {
                for c in old { engine.call("studio_agent_define", c.spec, actor: "editor") }
                studio.refresh()
            }
            save()
        }
    }

    func save() {
        let saved = Saved(providers: providers, providerOverrides: providerOverrides)
        if let data = try? JSONEncoder().encode(saved) { try? data.write(to: storeURL, options: .atomic) }
    }

    // MARK: Roster

    var cloudlings: [Cloudling] { studio.agents }
    func transcript(for c: Cloudling) -> [ChatEntry] { transcripts[c.id] ?? [] }
    func isWorking(_ c: Cloudling) -> Bool { working.contains(c.id) || studio.isWorking(c.id) }
    var workingCount: Int { cloudlings.filter(isWorking).count }
    func cloudling(named name: String) -> Cloudling? {
        let n = name.hasPrefix("@") ? String(name.dropFirst()) : name
        return cloudlings.first { $0.id == n.lowercased() || $0.name.caseInsensitiveCompare(n) == .orderedSame }
    }
    /// The crew member behind an actor id: in-editor agents ("agent:<id>") or external MCP
    /// agents that connect under a crew member's name ("mcp:<id>", "mcp:<client>/<id>").
    func cloudling(actor: String) -> Cloudling? {
        for prefix in ["agent:", "mcp:"] where actor.hasPrefix(prefix) {
            let rest = String(actor.dropFirst(prefix.count))
            return cloudling(named: rest.split(separator: "/").last.map(String.init) ?? rest)
        }
        return nil
    }
    func usage(for c: Cloudling) -> TokenUsage { studio.usage[c.id] ?? TokenUsage() }
    var totalUsage: TokenUsage { studio.totalUsage }

    func provider(for c: Cloudling) -> ProviderConfig? {
        if let id = providerOverrides[c.id], let p = providers.first(where: { $0.id == id }) { return p }
        let want = c.provider.lowercased()
        if let p = providers.first(where: { $0.name.lowercased() == want }) { return p }
        if want == "anthropic" || want == "claude" { return providers.first { $0.kind == .anthropic } ?? providers.first }
        if want == "openai" { return providers.first { $0.kind == .openAICompatible } ?? providers.first }
        return providers.first
    }

    /// Remembers which local provider config runs an agent on this machine (the agent file
    /// itself stores the provider's name).
    func setProviderOverride(_ config: ProviderConfig, forAgent id: String) {
        providerOverrides[id] = config.id
        save()
        sessions = sessions.filter { !$0.key.hasPrefix(id + "|") }
    }

    func modelLabel(for c: Cloudling) -> String {
        c.model.isEmpty ? (provider(for: c)?.defaultModel ?? "no provider") : c.model
    }

    /// Writes an agent definition to the project (studio_agent_define).
    @discardableResult
    func define(_ c: Cloudling) -> Bool {
        let r = engine.call("studio_agent_define", c.spec, actor: "editor")
        if r.isError { lastError = r.text }
        applyDefinitionChanges(c)
        studio.refresh()
        return !r.isError
    }

    @discardableResult
    func add(role: CrewRole) -> String? {
        let r = engine.call("studio_agent_define", ["name": .string("Puff \(cloudlings.count + 1)"), "role": .string(role.id)],
                            actor: "editor")
        studio.refresh()
        return r.isError ? nil : r.structured["id"].string
    }

    func remove(_ c: Cloudling) {
        stop(c)
        engine.call("studio_agent_remove", ["agent": .string(c.id)], actor: "editor")
        transcripts[c.id] = nil
        studio.refresh()
    }

    func spawnTeam(_ template: String) {
        let r = engine.call("studio_team_template", ["template": .string(template)], actor: "editor")
        if r.isError { lastError = r.text }
        studio.refresh()
    }

    /// Drops cached provider sessions so definition changes (instructions, tools, model)
    /// apply to the next message. The visible transcript is kept.
    func applyDefinitionChanges(_ c: Cloudling) {
        guard !working.contains(c.id) else { return }
        sessions = sessions.filter { !$0.key.hasPrefix(c.id + "|") }
        undelivered[c.id] = nil
    }

    func resetConversation(_ c: Cloudling) {
        stop(c)
        sessions = sessions.filter { !$0.key.hasPrefix(c.id + "|") }
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
    /// Returns the agent's final text (used for delegation and loops).
    @discardableResult
    func send(_ text: String, to c: Cloudling, depth: Int = 0, loopMember: Bool = false) async -> String {
        // One loop per agent at a time: a second loop would interleave messages in the
        // same provider history (weave / loops / delegation can all target a busy agent).
        guard !working.contains(c.id) else {
            log(c, ChatEntry(role: .system, text: "Busy — skipped a request while already working."))
            return "\(c.name) is busy with another task; try again later."
        }
        // Always run with the latest definition (the caller may hold a stale copy).
        let c = cloudlings.first { $0.id == c.id } ?? c
        log(c, ChatEntry(role: .user, text: text))
        working.insert(c.id)
        cancelled.remove(c.id)
        defer { working.remove(c.id) }

        let allowDelegation = depth == 0 && !loopMember
        let key = "\(c.id)|\(allowDelegation)|\(loopMember)"
        let session: LLMSession
        let model: String
        do {
            (session, model) = try sessionFor(c, key: key, allowDelegation: allowDelegation, loopMember: loopMember)
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
            reportUsage(turn.usage, model: model, for: c)
            if case .refusal(let why) = turn.stop {
                // A declined turn's partial output is discarded and its tools never run.
                log(c, ChatEntry(role: .error, text: "The model declined: \(why)", isError: true))
                sessions[key] = nil
                return finalText.isEmpty ? "(declined)" : finalText
            }
            if !turn.text.isEmpty {
                log(c, ChatEntry(role: .agent, text: turn.text))
                finalText = turn.text
            }
            if case .maxTokens = turn.stop, turn.toolCalls.isEmpty {
                log(c, ChatEntry(role: .system, text: "Reply was cut off (max tokens)."))
                return finalText
            }
            if turn.toolCalls.isEmpty { return finalText }
            outcomes = []
            for call in turn.toolCalls {
                if cancelled.contains(c.id) {
                    outcomes.append(ToolOutcome(callID: call.id, name: call.name, text: "Cancelled by the user.",
                                                imagesBase64: [], isError: true))
                } else if case .maxTokens = turn.stop {
                    outcomes.append(ToolOutcome(callID: call.id, name: call.name,
                                                text: "Your reply was cut off (max_tokens) before this call was complete, so it was not run. Re-issue it with smaller arguments.",
                                                imagesBase64: [], isError: true))
                } else {
                    outcomes.append(await execute(call, by: c, depth: depth, loopMember: loopMember))
                }
            }
        }
        undelivered[c.id] = outcomes  // delivered with the next message
        log(c, ChatEntry(role: .system, text: "Reached the step limit (\(rounds) rounds). Send a message to continue."))
        return finalText
    }

    /// The engine's brief for an agent: its system prompt and permitted tools (the same
    /// ones the headless runner and external agents use).
    private func brief(_ c: Cloudling, loopMember: Bool) -> (prompt: String, access: [String: String]) {
        let r = engine.call("studio_agent_brief", ["agent": .string(c.id), "loop_member": .bool(loopMember)], actor: "editor")
        var access: [String: String] = [:]
        for t in r.structured["tools"].array {
            if let n = t["name"].string { access[n] = t["access"].string ?? "allow" }
        }
        return (r.structured["system_prompt"].string ?? "", access)
    }

    private func sessionFor(_ c: Cloudling, key: String, allowDelegation: Bool, loopMember: Bool) throws -> (LLMSession, String) {
        guard let provider = provider(for: c) else { throw ProviderError.badResponse("no provider configured") }
        let model = c.model.isEmpty ? provider.defaultModel : c.model
        if let s = sessions[key] { return (s, model) }
        let (prompt, access) = brief(c, loopMember: loopMember)
        var tools = engineTools().filter { access[$0.name] != nil }
        if allowDelegation && (c.discipline == "direction" || c.discipline == "production") {
            tools.append(AgentTool(
                name: "crew_delegate",
                description: "Give a task to a crew member and wait for their report. Members: " +
                    cloudlings.filter { $0.id != c.id }.map { "@\($0.id) (\($0.role.title))" }.joined(separator: ", ") +
                    ". Be specific about what done looks like. For tracked work use studio_task_create instead.",
                inputSchema: ["type": "object",
                              "properties": ["member": ["type": "string", "description": "Crew member id or name"],
                                             "task": ["type": "string", "description": "The task"]],
                              "required": ["member", "task"], "additionalProperties": false],
                readOnly: false))
        }
        let s = try Providers.makeSession(config: provider, model: model, system: prompt, tools: tools)
        sessions[key] = s
        return (s, model)
    }

    /// Engine tools with their categories and read-only flags.
    func engineTools() -> [AgentTool] {
        engine.toolList()["tools"].array.map {
            AgentTool(name: $0["name"].string ?? "", description: $0["description"].string ?? "",
                      inputSchema: $0["inputSchema"], readOnly: $0["annotations"]["readOnlyHint"].bool ?? false,
                      category: $0["_meta"]["skywalker/category"].string ?? "scene")
        }
    }

    private func reportUsage(_ u: TokenUsage, model: String, for c: Cloudling) {
        guard u.requests > 0 || u.input > 0 || u.output > 0 else { return }
        engine.call("studio_usage_report", ["agent": .string(c.id), "model": .string(model),
                                            "input_tokens": .number(Double(u.input)), "output_tokens": .number(Double(u.output)),
                                            "cache_read_tokens": .number(Double(u.cacheRead))], actor: "editor")
    }

    private func execute(_ call: ToolCall, by c: Cloudling, depth: Int, loopMember: Bool) async -> ToolOutcome {
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

        let access = brief(c, loopMember: loopMember).access[call.name]
        guard let access else {
            return ToolOutcome(callID: call.id, name: call.name,
                               text: "The tool \(call.name) is not available to you (permissions or autonomy). Ask the human or a crew member.",
                               imagesBase64: [], isError: true)
        }
        if access == "ask" {
            let approved = await withCheckedContinuation { cont in
                pendingApprovals.append(PendingApproval(cloudling: c, call: call, resume: cont))
            }
            if !approved {
                log(c, ChatEntry(role: .system, text: "You declined \(call.name)."))
                return ToolOutcome(callID: call.id, name: call.name, text: "The human declined this action. Ask or try something else.",
                                   imagesBase64: [], isError: true)
            }
        }

        // Slow tools (design apps) run off the main thread, so the editor stays responsive.
        let result = await engine.callAsync(call.name, call.arguments, actor: c.actorName)
        log(c, ChatEntry(role: .tool, text: String(result.text.prefix(1200)), toolName: call.name,
                         imageBase64: result.imagesBase64.first, isError: result.isError))
        return ToolOutcome(callID: call.id, name: call.name, text: result.text, imagesBase64: result.imagesBase64,
                           isError: result.isError)
    }

    func resolve(_ approval: PendingApproval, approved: Bool) {
        // Resume exactly once, even if the button is clicked twice before the UI updates.
        guard let index = pendingApprovals.firstIndex(where: { $0.id == approval.id }) else { return }
        let pending = pendingApprovals.remove(at: index)
        pending.resume.resume(returning: approved)
    }

    // MARK: Studio loops

    /// Runs a studio loop with the in-editor crew: the engine runs playtest stages and the
    /// loop state machine; agent stages run here (in parallel when the stage allows it).
    func runLoop(_ name: String, goal: String = "", maxIterations: Int = 0) async {
        guard runningLoop == nil else { return }
        var args: JSON = ["loop": .string(name)]
        if !goal.isEmpty { args.set("goal", .string(goal)) }
        if maxIterations > 0 { args.set("max_iterations", .number(Double(maxIterations))) }
        loopLog = []
        await drive(name, engine.call("studio_loop_start", args, actor: "editor"))
    }

    /// Answers a human approval gate and continues the loop.
    func approveLoop(_ name: String, approved: Bool) async {
        guard runningLoop == nil else { return }
        await drive(name, engine.call("studio_loop_advance", ["loop": .string(name), "approve": .bool(approved)], actor: "editor"))
    }

    func stopLoop(_ name: String) {
        if runningLoop == name {
            loopCancelled = true
            for c in cloudlings where working.contains(c.id) { stop(c) }
        } else {
            engine.call("studio_loop_stop", ["loop": .string(name), "reason": "stopped from the editor"], actor: "editor")
        }
    }

    private func drive(_ name: String, _ first: ToolCallResult) async {
        runningLoop = name
        loopCancelled = false
        defer {
            runningLoop = nil
            studio.refresh()
        }
        var r = first
        while true {
            if r.isError {
                lastError = r.text
                loopLog.append("✗ \(r.text)")
                return
            }
            let st = r.structured
            guard st["status"].string == "running" else {
                loopLog.append("■ \(st["status"].string ?? "")\(st["stop_reason"].string.map { " — \($0)" } ?? "")")
                return
            }
            let assignments = st["assignments"].array
            let parallel = st["parallel"].bool ?? true
            loopLog.append("◆ iteration \(st["iteration"].int ?? 0) · \(st["stage"].string ?? "") → "
                + assignments.compactMap { $0["agent"].string.map { "@\($0)" } }.joined(separator: ", "))
            var reports: [JSON] = []
            if parallel && assignments.count > 1 {
                // Agents of a parallel stage interleave on the main actor while each waits for
                // its model; every edit is still one attributed transaction.
                let tasks = assignments.map { a in Task { await self.runAssignment(a) } }
                for t in tasks { reports.append(await t.value) }
            } else {
                for a in assignments { reports.append(await runAssignment(a)) }
            }
            if loopCancelled {
                engine.call("studio_loop_stop", ["loop": .string(name), "reason": "stopped from the editor"], actor: "editor")
                loopLog.append("■ stopped")
                return
            }
            studio.refresh()
            r = engine.call("studio_loop_advance", ["loop": .string(name), "reports": .array(reports)], actor: "editor")
        }
    }

    private func runAssignment(_ a: JSON) async -> JSON {
        let id = a["agent"].string ?? ""
        guard let c = cloudlings.first(where: { $0.id == id }) else {
            return ["agent": .string(id), "report": "(agent no longer on the roster)"]
        }
        let report = await send(a["prompt"].string ?? "", to: c, depth: 1, loopMember: true)
        loopLog.append("  ↳ @\(id): \(report.prefix(160))")
        return ["agent": .string(id), "report": .string(report)]
    }

    // MARK: Weave (intent -> Wander)

    /// Asks the gameplay programmer to turn a behavior's natural-language intent into Wander code.
    func weave(entity: UInt64, entityName: String, behavior: String, intent: String) async {
        guard let coder = cloudlings.first(where: { $0.roleID == "gameplay_programmer" })
            ?? cloudlings.first(where: { $0.discipline == "engineering" }) ?? cloudlings.first else { return }
        await send("""
            Write the Wander behavior "\(behavior)" for entity #\(entity) (\(entityName)).
            Intent: \(intent)
            Steps: read wander_reference if you have not yet, write the code, check it with wander_check, then save it \
            with behavior_set (entity \(entity), name "\(behavior)", intent exactly as given). Then step the simulation \
            to verify it does what the intent says, and stop the simulation.
            """, to: coder, depth: 1)
    }
}
