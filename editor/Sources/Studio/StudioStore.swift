import Foundation
import Observation

// The editor's view of the engine's studio: roster, board, feedback with director verdicts
// and measured effects, loops, messages and usage. All data comes from studio tools — the
// same surface the CLI runner and external agents use — and refreshes on studio events.

struct StudioTask: Identifiable, Hashable, Sendable {
    let id: String
    var title: String
    var description: String
    var status: String
    var priority: String
    var assignee: String
    var discipline: String
    var acceptance: [String]
    var feedback: [String]
    var commentCount: Int
    var lastComment: String

    init(json j: JSON) {
        id = j["id"].string ?? ""
        title = j["title"].string ?? ""
        description = j["description"].string ?? ""
        status = j["status"].string ?? "todo"
        priority = j["priority"].string ?? "normal"
        assignee = j["assignee"].string ?? ""
        discipline = j["discipline"].string ?? ""
        acceptance = j["acceptance"].array.compactMap(\.string)
        feedback = j["links"]["feedback"].array.compactMap(\.string)
        commentCount = j["comment_count"].int ?? j["comments"].array.count
        lastComment = j["comments"].array.last?["text"].string ?? ""
    }
}

struct MetricDelta: Hashable, Sendable {
    var name: String
    var before: String
    var after: String
    var verdict: String  // better | worse | same | changed
}

struct StudioFeedback: Identifiable, Hashable, Sendable {
    let id: String
    var by: String
    var category: String
    var severity: String
    var summary: String
    var details: String
    var target: String
    var status: String
    var occurrences: Int
    var tasks: [String]
    var playtest: String
    var mergedInto: String
    var verdict: String = ""
    var rationale: String = ""
    var decidedBy: String = ""
    var effect: String = ""
    var deltas: [MetricDelta] = []

    init(json j: JSON) {
        id = j["id"].string ?? ""
        by = j["by"].string ?? ""
        category = j["category"].string ?? ""
        severity = j["severity"].string ?? "medium"
        summary = j["summary"].string ?? ""
        details = j["details"].string ?? ""
        target = j["target"].string ?? ""
        status = j["status"].string ?? "open"
        occurrences = j["occurrences"].int ?? 1
        tasks = j["tasks"].array.compactMap(\.string)
        playtest = j["evidence"]["playtest"].string ?? ""
        mergedInto = j["merged_into"].string ?? ""
        let v = j["verdict"]
        if !v.isNull {
            verdict = v["verdict"].string ?? ""
            rationale = v["rationale"].string ?? ""
            decidedBy = v["by"].string ?? ""
            effect = v["effect"]["status"].string ?? ""
            deltas = v["effect"]["metrics"].members.map { name, row in
                MetricDelta(name: name, before: Self.fmt(row["before"]), after: Self.fmt(row["after"]),
                            verdict: row["verdict"].string ?? "")
            }
        }
    }

    static func fmt(_ v: JSON) -> String {
        guard let n = v.number else { return "—" }
        return n.rounded() == n ? String(Int(n)) : String(format: "%.2f", n)
    }

    var needsDecision: Bool { status == "open" || status == "regressed" }
}

struct StudioMessage: Identifiable, Hashable, Sendable {
    let id: String
    var at: String
    var from: String
    var channel: String
    var to: [String]
    var mentions: [String]
    var thread: String
    var text: String

    init(json j: JSON) {
        id = j["id"].string ?? ""
        at = j["at"].string ?? ""
        from = j["from"].string ?? ""
        channel = j["channel"].string ?? "general"
        to = j["to"].array.compactMap(\.string)
        mentions = j["mentions"].array.compactMap(\.string)
        thread = j["thread"].string ?? ""
        text = j["text"].string ?? ""
    }

    var time: String { at.count >= 16 ? String(at.dropFirst(11).prefix(5)) : at }
}

struct LoopStageInfo: Hashable, Sendable {
    var id: String
    var title: String
    var kind: String
}

/// A loop's status payload (`studio_loop_status`).
struct StudioLoop: Identifiable, Hashable, Sendable {
    var id: String { name }
    var name: String
    var goal: String
    var status: String
    var iteration: Int
    var maxIterations: Int
    var stage: String
    var stopReason: String
    var stages: [LoopStageInfo]
    var iterations: [JSON]
    var trends: JSON
    var waitingFor: [String]
    var tokensUsed: Int

    init(json j: JSON) {
        name = j["loop"].string ?? ""
        goal = j["goal"].string ?? ""
        status = j["status"].string ?? "idle"
        iteration = j["iteration"].int ?? 0
        maxIterations = j["max_iterations"].int ?? 0
        stage = j["stage"].string ?? ""
        stopReason = j["stop_reason"].string ?? ""
        stages = j["stages"].array.map {
            LoopStageInfo(id: $0["id"].string ?? "", title: $0["title"].string ?? "", kind: $0["kind"].string ?? "agents")
        }
        iterations = j["iterations"].array
        trends = j["trends"]
        waitingFor = j["waiting_for"].array.compactMap(\.string)
        tokensUsed = j["tokens_used"].int ?? 0
    }

    var isActive: Bool { status == "running" || status == "awaiting_approval" }

    /// Numeric series of a metric across iterations (nil entries skipped).
    func series(_ metric: String) -> [Double] { trends[metric].array.compactMap(\.number) }
}

@MainActor
@Observable
final class StudioStore {
    private(set) var agents: [Cloudling] = []
    private(set) var presence: [String: (status: String, activity: String)] = [:]
    private(set) var usage: [String: TokenUsage] = [:]
    private(set) var openTasks: [String: Int] = [:]
    private(set) var tasks: [StudioTask] = []
    private(set) var feedback: [StudioFeedback] = []
    private(set) var loops: [StudioLoop] = []
    private(set) var messages: [StudioMessage] = []
    private(set) var teamTemplates: [(name: String, description: String)] = []
    private(set) var loopTemplates: [(name: String, description: String)] = []
    private(set) var totalUsage = TokenUsage()
    private(set) var latestPlaytest = ""
    private(set) var latestMetrics: JSON = .null
    /// Bumped on every refresh (views that cache derived data can watch it).
    private(set) var revision = 0

    @ObservationIgnored let engine: EngineStore
    @ObservationIgnored private var refreshScheduled = false

    init(engine: EngineStore) {
        self.engine = engine
        loadCatalog()
        refresh()
        engine.onStudioEvent = { [weak self] in self?.scheduleRefresh() }
    }

    private func loadCatalog() {
        let catalog = engine.call("studio_overview", ["include_catalog": true], actor: "editor").structured["catalog"]
        CrewRole.all = catalog["roles"].array.map {
            CrewRole(id: $0["id"].string ?? "", title: $0["title"].string ?? "", discipline: $0["discipline"].string ?? "",
                     symbol: $0["symbol"].string ?? "person.fill", mission: $0["mission"].string ?? "")
        }
        teamTemplates = catalog["team_templates"].array.map { ($0["name"].string ?? "", $0["description"].string ?? "") }
        loopTemplates = catalog["loop_templates"].array.map { ($0["name"].string ?? "", $0["description"].string ?? "") }
    }

    /// Coalesces bursts of studio events into one refresh on the next main-actor turn.
    func scheduleRefresh() {
        guard !refreshScheduled else { return }
        refreshScheduled = true
        Task { @MainActor [weak self] in
            guard let self else { return }
            self.refreshScheduled = false
            self.refresh()
        }
    }

    func refresh() {
        let roster = call("studio_agent_list", ["include_profiles": true])
        var newAgents: [Cloudling] = []
        var newPresence: [String: (status: String, activity: String)] = [:]
        var newUsage: [String: TokenUsage] = [:]
        var newOpen: [String: Int] = [:]
        for a in roster["agents"].array {
            guard let c = Cloudling(json: a) else { continue }
            newAgents.append(c)
            newPresence[c.id] = (a["presence"]["status"].string ?? "idle", a["presence"]["activity"].string ?? "")
            newUsage[c.id] = TokenUsage(json: a["usage"])
            newOpen[c.id] = a["open_tasks"].int ?? 0
        }
        if newAgents != agents { agents = newAgents }
        presence = newPresence
        if newUsage != usage { usage = newUsage }
        openTasks = newOpen
        tasks = call("studio_task_list", ["limit": 1000])["tasks"].array.map(StudioTask.init(json:))
        feedback = call("studio_feedback_list", ["limit": 500])["feedback"].array.map(StudioFeedback.init(json:))
        var newLoops: [StudioLoop] = []
        for l in call("studio_loop_status", [:])["loops"].array {
            let name = l["loop"].string ?? ""
            newLoops.append(StudioLoop(json: call("studio_loop_status", ["loop": .string(name)])))
        }
        loops = newLoops
        messages = call("studio_inbox", ["channel": "*", "limit": 500])["messages"].array.map(StudioMessage.init(json:))
        let overview = call("studio_overview", [:])
        totalUsage = TokenUsage(json: overview["usage"])
        latestPlaytest = overview["latest_playtest"].string ?? ""
        latestMetrics = overview["latest_metrics"]
        revision += 1
    }

    @discardableResult
    func call(_ tool: String, _ args: JSON) -> JSON {
        engine.call(tool, args, actor: "editor").structured
    }

    // MARK: Queries

    func agent(_ id: String) -> Cloudling? { agents.first { $0.id == id } }
    func isWorking(_ id: String) -> Bool { presence[id]?.status == "working" }
    func activity(_ id: String) -> String { presence[id]?.activity ?? "" }
    func tasks(in status: String) -> [StudioTask] { tasks.filter { $0.status == status } }
    var channels: [String] {
        var seen: [String] = ["general"]
        for m in messages where !seen.contains(m.channel) { seen.append(m.channel) }
        return seen
    }
    var needsDecisionCount: Int { feedback.filter(\.needsDecision).count }
    func loop(_ name: String) -> StudioLoop? { loops.first { $0.name == name } }
}
