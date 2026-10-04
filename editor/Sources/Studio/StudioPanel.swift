import SwiftUI

/// The Studio dock: the team of agents making this game — roster, Kanban board, feedback
/// with director verdicts and measured effects, loops, and message threads. Everything is
/// the engine's studio (shared with the CLI runner and external agents).
enum StudioTab: String, CaseIterable, Identifiable {
    case roster = "Roster", board = "Board", feedback = "Feedback", loops = "Loops", messages = "Messages", tools = "Tools"
    var id: String { rawValue }
    var symbol: String {
        switch self {
        case .roster: "person.3"
        case .board: "rectangle.split.3x1"
        case .feedback: "exclamationmark.bubble"
        case .loops: "arrow.triangle.2.circlepath"
        case .messages: "bubble.left.and.bubble.right"
        case .tools: "wrench.and.screwdriver"
        }
    }
}

struct StudioPanel: View {
    @Environment(StudioStore.self) private var studio
    @AppStorage("studio.tab") private var tab: StudioTab = .roster
    var openAgent: (String) -> Void

    var body: some View {
        VStack(spacing: 0) {
            PanelTabs(tabs: StudioTab.allCases, selection: $tab, title: { title(for: $0) }, icon: { $0.symbol },
                      trailing: AnyView(StudioStats()))
            switch tab {
            case .roster: RosterView(openAgent: openAgent)
            case .board: BoardView()
            case .feedback: FeedbackView()
            case .loops: LoopsView()
            case .messages: MessagesView()
            case .tools: CustomToolsView()
            }
        }
    }

    private func title(for t: StudioTab) -> String {
        switch t {
        case .feedback where studio.needsDecisionCount > 0:
            return "Feedback (\(studio.needsDecisionCount))"
        case .tools where studio.pendingToolCount > 0:
            return "Tools (\(studio.pendingToolCount))"
        case .board:
            let open = studio.tasks.filter { $0.status == "todo" || $0.status == "doing" || $0.status == "review" }.count
            return open > 0 ? "Board (\(open))" : "Board"
        default:
            return t.rawValue
        }
    }
}

/// Compact usage and latest-metrics readout in the tab strip.
private struct StudioStats: View {
    @Environment(StudioStore.self) private var studio

    var body: some View {
        HStack(spacing: 10) {
            if !studio.latestPlaytest.isEmpty {
                let m = studio.latestMetrics
                Text("\(studio.latestPlaytest): \(pct(m["completion_rate"].number)) done · \(num(m["deaths"].number)) deaths")
                    .help("Latest playtest metrics")
            }
            let u = studio.totalUsage
            Text("\(ChatView.compact(u.input + u.cacheRead + u.output)) tok · $\(String(format: "%.2f", u.costUSD))")
                .help("Studio usage: \(u.requests) requests, \(u.toolCalls) tool calls")
        }
        .font(Theme.monoSmall)
        .foregroundStyle(Theme.textFaint)
    }

    private func pct(_ v: Double?) -> String { v.map { "\(Int(($0 * 100).rounded()))%" } ?? "—" }
    private func num(_ v: Double?) -> String { v.map { String(format: "%.1f", $0) } ?? "—" }
}

// MARK: - Roster

private struct RosterView: View {
    @Environment(StudioStore.self) private var studio
    @Environment(CrewStore.self) private var crew
    var openAgent: (String) -> Void
    @State private var designing: String?

    var body: some View {
        ScrollView {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 220), spacing: 6)], spacing: 6) {
                ForEach(studio.agents) { c in
                    AgentCard(agent: c)
                        .onTapGesture { openAgent(c.id) }
                        .contextMenu {
                            Button("Open Chat") { openAgent(c.id) }
                            Button("Design…") { designing = c.id }
                            Divider()
                            Button("Remove from Studio", role: .destructive) { crew.remove(c) }
                        }
                }
            }
            .padding(8)
        }
        .overlay(alignment: .bottomTrailing) {
            AddAgentMenu { id in designing = id }
                .padding(4)
                .background(Theme.panelRaised, in: RoundedRectangle(cornerRadius: 5))
                .padding(8)
        }
        .overlay {
            if studio.agents.isEmpty {
                Text("No agents yet — spawn a team with ＋").font(Theme.label).foregroundStyle(Theme.textFaint)
            }
        }
        .sheet(item: Binding(get: { designing.map(IdentifiedString.init) }, set: { designing = $0?.id })) { item in
            VStack(spacing: 0) {
                AgentDesigner(cloudlingID: item.id)
                HStack {
                    Spacer()
                    Button("Done") { designing = nil }.keyboardShortcut(.defaultAction)
                }
                .padding(10)
            }
            .frame(width: 640, height: 700)
        }
    }
}

struct IdentifiedString: Identifiable {
    let id: String
}

private struct AgentCard: View {
    @Environment(StudioStore.self) private var studio
    @Environment(CrewStore.self) private var crew
    let agent: Cloudling

    var body: some View {
        let working = crew.isWorking(agent)
        let usage = crew.usage(for: agent)
        HStack(alignment: .top, spacing: 8) {
            CloudAvatar(color: agent.color, face: agent.face, size: 22, working: working)
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 4) {
                    Text(agent.name).font(Theme.sectionTitle)
                    Text("@\(agent.id)").font(Theme.monoSmall).foregroundStyle(Theme.textFaint)
                    Spacer(minLength: 2)
                    Circle().fill(working ? Theme.ai : Theme.textFaint.opacity(0.5)).frame(width: 6, height: 6)
                        .help(working ? "Working" : "Idle")
                }
                Label(agent.role.title, systemImage: agent.role.symbol).font(Theme.label).foregroundStyle(Theme.textDim)
                    .labelStyle(.titleAndIcon)
                if !agent.focus.isEmpty {
                    Text(agent.focus).font(Theme.label).foregroundStyle(Theme.textFaint).lineLimit(1)
                }
                if working, !studio.activity(agent.id).isEmpty {
                    Text(studio.activity(agent.id)).font(Theme.label).foregroundStyle(Theme.ai).lineLimit(1)
                }
                HStack(spacing: 8) {
                    let open = studio.openTasks[agent.id] ?? 0
                    if open > 0 { Text("\(open) open").foregroundStyle(Theme.text) }
                    if usage.requests > 0 {
                        Text("\(ChatView.compact(usage.input + usage.cacheRead + usage.output)) tok")
                        Text("$\(String(format: "%.2f", usage.costUSD))")
                    }
                    Text(agent.autonomy == .auto ? "auto" : agent.autonomy.rawValue)
                }
                .font(Theme.monoSmall).foregroundStyle(Theme.textFaint)
            }
        }
        .padding(8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Theme.panelRaised, in: RoundedRectangle(cornerRadius: 6))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(working ? Theme.ai.opacity(0.5) : Theme.border))
        .contentShape(Rectangle())
    }
}

// MARK: - Board

private let boardColumns = ["backlog", "todo", "doing", "review", "done", "dropped"]

private func priorityColor(_ p: String) -> Color {
    switch p {
    case "critical": Theme.error
    case "high": Theme.warning
    case "low": Theme.textFaint
    default: Theme.accent
    }
}

private struct BoardView: View {
    @Environment(StudioStore.self) private var studio
    @State private var newTitle = ""
    @State private var adding = false

    var body: some View {
        ScrollView(.horizontal) {
            HStack(alignment: .top, spacing: 6) {
                ForEach(boardColumns, id: \.self) { status in
                    BoardColumn(status: status, adding: status == "todo" ? $adding : nil)
                }
            }
            .padding(8)
        }
    }
}

private struct BoardColumn: View {
    @Environment(StudioStore.self) private var studio
    let status: String
    var adding: Binding<Bool>?
    @State private var targeted = false
    @State private var title = ""

    var body: some View {
        let tasks = studio.tasks(in: status)
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(status.uppercased()).font(Theme.caps).tracking(0.5).foregroundStyle(Theme.textDim)
                Text("\(tasks.count)").font(Theme.monoSmall).foregroundStyle(Theme.textFaint)
                Spacer()
                if let adding {
                    IconButton(symbol: "plus", help: "New task") { adding.wrappedValue.toggle() }
                }
            }
            if let adding, adding.wrappedValue {
                TextField("Task title, ⏎ to add", text: $title)
                    .textFieldStyle(.plain).font(Theme.label)
                    .padding(5).background(Theme.field, in: RoundedRectangle(cornerRadius: 4))
                    .onSubmit {
                        guard !title.isEmpty else { return }
                        studio.call("studio_task_create", ["title": .string(title)])
                        title = ""
                        adding.wrappedValue = false
                        studio.refresh()
                    }
            }
            ScrollView {
                LazyVStack(spacing: 4) {
                    ForEach(tasks) { TaskCard(task: $0) }
                }
            }
        }
        .padding(6)
        .frame(width: 210)
        .frame(maxHeight: .infinity, alignment: .top)
        .background(targeted ? Theme.accent.opacity(0.08) : Theme.panel, in: RoundedRectangle(cornerRadius: 6))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(targeted ? Theme.accent.opacity(0.5) : Theme.border))
        .dropDestination(for: String.self) { ids, _ in
            for id in ids { studio.call("studio_task_update", ["task": .string(id), "status": .string(status)]) }
            studio.refresh()
            return true
        } isTargeted: { targeted = $0 }
    }
}

private struct TaskCard: View {
    @Environment(StudioStore.self) private var studio
    let task: StudioTask
    @State private var showing = false

    var body: some View {
        HStack(spacing: 0) {
            Rectangle().fill(priorityColor(task.priority)).frame(width: 3)
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 4) {
                    Text(task.id).font(Theme.monoSmall).foregroundStyle(Theme.textFaint)
                    ForEach(task.feedback, id: \.self) { f in
                        Text(f).font(Theme.monoSmall).padding(.horizontal, 3)
                            .background(Theme.warning.opacity(0.15), in: RoundedRectangle(cornerRadius: 3))
                            .foregroundStyle(Theme.warning)
                    }
                    Spacer(minLength: 0)
                    if let a = studio.agent(task.assignee) {
                        CloudAvatar(color: a.color, face: a.face, size: 12, working: studio.isWorking(a.id)).help("@\(a.id)")
                    }
                }
                Text(task.title).font(Theme.label).foregroundStyle(Theme.text).lineLimit(2)
                if !task.lastComment.isEmpty {
                    Text(task.lastComment).font(.system(size: 10)).foregroundStyle(Theme.textFaint).lineLimit(1)
                }
            }
            .padding(6)
        }
        .background(Theme.panelRaised, in: RoundedRectangle(cornerRadius: 5))
        .overlay(RoundedRectangle(cornerRadius: 5).stroke(Theme.border))
        .clipShape(RoundedRectangle(cornerRadius: 5))
        .draggable(task.id)
        .onTapGesture { showing = true }
        .popover(isPresented: $showing, arrowEdge: .trailing) { TaskDetail(task: task) }
        .contextMenu {
            Menu("Move to") {
                ForEach(boardColumns, id: \.self) { s in
                    Button(s.capitalized) { update(["status": .string(s)]) }.disabled(s == task.status)
                }
            }
            Menu("Assign to") {
                Button("Nobody") { update(["assignee": ""]) }
                ForEach(studio.agents) { a in Button("\(a.name) — \(a.role.title)") { update(["assignee": .string(a.id)]) } }
            }
        }
    }

    private func update(_ patch: JSON) {
        var args = patch
        args.set("task", .string(task.id))
        studio.call("studio_task_update", args)
        studio.refresh()
    }
}

private struct TaskDetail: View {
    @Environment(StudioStore.self) private var studio
    let task: StudioTask
    @State private var comment = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(task.id).font(Theme.mono).foregroundStyle(Theme.textFaint)
                Text(task.status).font(Theme.caps).padding(.horizontal, 5).background(Theme.accent.opacity(0.15), in: Capsule())
                Text(task.priority).font(Theme.caps).foregroundStyle(priorityColor(task.priority))
            }
            Text(task.title).font(.system(size: 13, weight: .semibold))
            if !task.description.isEmpty { Text(task.description).font(Theme.label).foregroundStyle(Theme.textDim) }
            if !task.acceptance.isEmpty {
                VStack(alignment: .leading, spacing: 2) {
                    Text("ACCEPTANCE").font(Theme.caps).foregroundStyle(Theme.textFaint)
                    ForEach(task.acceptance, id: \.self) { Label($0, systemImage: "checkmark.circle").font(Theme.label) }
                }
            }
            HStack {
                TextField("Comment…", text: $comment).textFieldStyle(.roundedBorder).font(Theme.label)
                Button("Post") {
                    studio.call("studio_task_update", ["task": .string(task.id), "comment": .string(comment)])
                    comment = ""
                    studio.refresh()
                }
                .disabled(comment.isEmpty)
            }
        }
        .padding(12)
        .frame(width: 320)
    }
}

// MARK: - Feedback

private struct FeedbackView: View {
    @Environment(StudioStore.self) private var studio
    @AppStorage("studio.feedbackFilter") private var filter = "needs"

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Picker("Filter", selection: $filter) {
                    Text("Needs decision").tag("needs")
                    Text("Active").tag("active")
                    Text("All").tag("all")
                }
                .pickerStyle(.segmented).labelsHidden().fixedSize()
                Spacer()
                Text("\(items.count) item\(items.count == 1 ? "" : "s")").font(Theme.label).foregroundStyle(Theme.textFaint)
            }
            .padding(.horizontal, 8).padding(.vertical, 4)
            List(items) { f in
                FeedbackRow(item: f).listRowSeparator(.visible).listRowBackground(Theme.panel)
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .overlay {
                if items.isEmpty {
                    Text(filter == "needs" ? "Nothing waits for a decision." : "No feedback yet — run a playtest or a loop.")
                        .font(Theme.label).foregroundStyle(Theme.textFaint)
                }
            }
        }
    }

    private var items: [StudioFeedback] {
        switch filter {
        case "needs": studio.feedback.filter(\.needsDecision)
        case "active": studio.feedback.filter { ["accepted", "in_progress", "fixed", "regressed"].contains($0.status) }
        default: studio.feedback
        }
    }
}

private func severityColor(_ s: String) -> Color {
    switch s {
    case "critical": Theme.error
    case "high": Theme.warning
    case "medium": Theme.accent
    default: Theme.textFaint
    }
}

private func statusColor(_ s: String) -> Color {
    switch s {
    case "open": Theme.warning
    case "regressed": Theme.error
    case "verified": Theme.success
    case "fixed": Theme.success.opacity(0.7)
    case "accepted", "in_progress": Theme.accent
    default: Theme.textFaint
    }
}

private struct FeedbackRow: View {
    @Environment(StudioStore.self) private var studio
    let item: StudioFeedback
    @State private var deciding = false

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 6) {
                Circle().fill(severityColor(item.severity)).frame(width: 7, height: 7).help(item.severity)
                Text(item.id).font(Theme.monoSmall).foregroundStyle(Theme.textFaint)
                Text(item.category).font(Theme.caps).foregroundStyle(Theme.textDim)
                    .padding(.horizontal, 4).background(Theme.panelRaised, in: RoundedRectangle(cornerRadius: 3))
                Text(item.summary).font(Theme.body).lineLimit(1)
                if item.occurrences > 1 { Text("×\(item.occurrences)").font(Theme.monoSmall).foregroundStyle(Theme.textFaint) }
                Spacer()
                Text(item.status.replacingOccurrences(of: "_", with: " ")).font(Theme.caps)
                    .padding(.horizontal, 5).padding(.vertical, 1)
                    .foregroundStyle(statusColor(item.status))
                    .background(statusColor(item.status).opacity(0.14), in: Capsule())
                if item.needsDecision {
                    Button("Decide…") { deciding = true }.controlSize(.small)
                        .popover(isPresented: $deciding, arrowEdge: .leading) { DecisionForm(item: item) { deciding = false } }
                }
            }
            HStack(spacing: 6) {
                Text("by \(item.by)")
                if !item.target.isEmpty { Text("· \(item.target)").lineLimit(1) }
                if !item.playtest.isEmpty { Text("· \(item.playtest)") }
                if !item.mergedInto.isEmpty { Text("· merged into \(item.mergedInto)") }
            }
            .font(Theme.label).foregroundStyle(Theme.textFaint).padding(.leading, 13)
            if !item.verdict.isEmpty {
                HStack(alignment: .firstTextBaseline, spacing: 5) {
                    Image(systemName: "scalemass").font(.system(size: 9))
                    Text(item.verdict.replacingOccurrences(of: "_", with: " ")).fontWeight(.semibold)
                        .foregroundStyle(item.verdict == "act" ? Theme.accent : item.verdict == "defer" ? Theme.warning : Theme.textDim)
                    Text("by \(item.decidedBy):").foregroundStyle(Theme.textFaint)
                    Text(item.rationale).foregroundStyle(Theme.textDim).lineLimit(2)
                    if !item.tasks.isEmpty { Text("→ " + item.tasks.joined(separator: " ")).font(Theme.monoSmall).foregroundStyle(Theme.textFaint) }
                }
                .font(Theme.label).padding(.leading, 13)
            }
            if !item.effect.isEmpty && item.effect != "n/a" {
                HStack(spacing: 6) {
                    Text("effect").font(Theme.caps).foregroundStyle(Theme.textFaint)
                    Text(item.effect).font(Theme.caps).foregroundStyle(effectColor)
                    ForEach(item.deltas, id: \.name) { d in
                        Text("\(d.name) \(d.before)→\(d.after)")
                            .font(Theme.monoSmall)
                            .foregroundStyle(d.verdict == "better" ? Theme.success : d.verdict == "worse" ? Theme.error : Theme.textDim)
                    }
                }
                .padding(.leading, 13)
            }
        }
        .padding(.vertical, 2)
    }

    private var effectColor: Color {
        switch item.effect {
        case "improved": Theme.success
        case "regressed": Theme.error
        case "pending": Theme.textFaint
        default: Theme.warning
        }
    }
}

/// The human's own verdict on a feedback item (the same studio_decide the director uses).
private struct DecisionForm: View {
    @Environment(StudioStore.self) private var studio
    let item: StudioFeedback
    var done: () -> Void
    @State private var verdict = "act"
    @State private var rationale = ""
    @State private var taskTitle = ""
    @State private var assignee = ""
    @State private var mergeInto = ""
    @State private var error = ""

    var body: some View {
        Form {
            Text("\(item.id): \(item.summary)").font(Theme.sectionTitle)
            Picker("Verdict", selection: $verdict) {
                Text("Act").tag("act")
                Text("Drop").tag("drop")
                Text("Defer").tag("defer")
                Text("Merge").tag("merge_into")
            }
            .pickerStyle(.segmented)
            TextField("Rationale (shown to everyone)", text: $rationale, axis: .vertical).lineLimit(2...4)
            if verdict == "act" {
                TextField("Task title", text: $taskTitle)
                Picker("Assignee", selection: $assignee) {
                    Text("Unassigned").tag("")
                    ForEach(studio.agents) { Text("\($0.name) — \($0.role.title)").tag($0.id) }
                }
            }
            if verdict == "merge_into" {
                Picker("Duplicate of", selection: $mergeInto) {
                    Text("—").tag("")
                    ForEach(studio.feedback.filter { $0.id != item.id }) { Text("\($0.id) \($0.summary)").tag($0.id) }
                }
            }
            if !error.isEmpty { Text(error).font(.caption).foregroundStyle(Theme.error) }
            HStack {
                Spacer()
                Button("Cancel", action: done)
                Button("Decide", action: decide).buttonStyle(.borderedProminent).disabled(rationale.isEmpty)
            }
        }
        .padding(10)
        .frame(width: 380)
    }

    private func decide() {
        var args: JSON = ["feedback": .string(item.id), "verdict": .string(verdict), "rationale": .string(rationale)]
        if verdict == "act" {
            var t: JSON = ["title": .string(taskTitle.isEmpty ? item.summary : taskTitle)]
            if !assignee.isEmpty { t.set("assignee", .string(assignee)) }
            args.set("tasks", [t])
        }
        if verdict == "merge_into" { args.set("merge_into", .string(mergeInto)) }
        let r = studio.engine.call("studio_decide", args, actor: "user")
        if r.isError {
            error = r.text
            return
        }
        studio.refresh()
        done()
    }
}

// MARK: - Loops

private struct LoopsView: View {
    @Environment(StudioStore.self) private var studio
    @Environment(CrewStore.self) private var crew
    @State private var selected: String?

    var body: some View {
        HStack(spacing: 0) {
            VStack(spacing: 0) {
                List(selection: $selected) {
                    ForEach(studio.loops) { l in
                        HStack(spacing: 6) {
                            Circle().fill(loopColor(l.status)).frame(width: 6, height: 6)
                            Text(l.name.replacingOccurrences(of: "_", with: " ")).font(Theme.body).lineLimit(1)
                            Spacer()
                            if l.iteration > 0 { Text("\(l.iteration)/\(l.maxIterations)").font(Theme.monoSmall).foregroundStyle(Theme.textFaint) }
                        }
                        .tag(l.name)
                    }
                }
                .listStyle(.plain).scrollContentBackground(.hidden)
                HStack {
                    Menu {
                        ForEach(studio.loopTemplates, id: \.name) { t in
                            Button(t.name.replacingOccurrences(of: "_", with: " ").capitalized) {
                                studio.call("studio_loop_define", ["template": .string(t.name)])
                                studio.refresh()
                                selected = t.name
                            }
                            .help(t.description)
                        }
                    } label: { Label("New Loop", systemImage: "plus").font(Theme.label) }
                    .menuStyle(.borderlessButton).fixedSize()
                    Spacer()
                }
                .padding(6)
            }
            .frame(width: 200)
            Rectangle().fill(Theme.border).frame(width: 1)
            if let l = studio.loop(selected ?? "") {
                LoopDetail(loop: l)
            } else {
                VStack(spacing: 6) {
                    Text("Loops").font(Theme.sectionTitle)
                    Text("Repeatable cycles: bots and playtesters play, the director triages feedback into tasks (or drops it with a "
                        + "reason), the team fixes, and a verification playtest measures the effect. Create one from a template.")
                        .font(Theme.label).foregroundStyle(Theme.textFaint).multilineTextAlignment(.center)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .padding()
            }
        }
        .onAppear { selected = selected ?? studio.loops.first?.name }
    }
}

private func loopColor(_ s: String) -> Color {
    switch s {
    case "running": Theme.ai
    case "awaiting_approval": Theme.warning
    case "done": Theme.success
    case "stopped": Theme.error
    default: Theme.textFaint
    }
}

private struct LoopDetail: View {
    @Environment(StudioStore.self) private var studio
    @Environment(CrewStore.self) private var crew
    let loop: StudioLoop
    @State private var goal = ""
    @State private var iterations = 0
    @AppStorage("studio.metric") private var metric = "completion_rate"

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            controls
            HStack(alignment: .top, spacing: 10) {
                LoopTimeline(loop: loop)
                VStack(alignment: .leading, spacing: 4) {
                    Picker("Metric", selection: $metric) {
                        ForEach(["completion_rate", "deaths", "time_to_goal", "stuck_seconds", "coverage", "est_fps"], id: \.self) {
                            Text($0.replacingOccurrences(of: "_", with: " ")).tag($0)
                        }
                    }
                    .labelsHidden().pickerStyle(.menu).fixedSize().font(Theme.label)
                    Sparkline(values: loop.series(metric)).frame(width: 180, height: 44)
                    if let last = loop.series(metric).last {
                        Text("\(metric): \(String(format: "%.2f", last))").font(Theme.monoSmall).foregroundStyle(Theme.textDim)
                    }
                    if crew.runningLoop == loop.name || !crew.loopLog.isEmpty {
                        ScrollView {
                            Text(crew.loopLog.suffix(40).joined(separator: "\n")).font(Theme.monoSmall).foregroundStyle(Theme.textFaint)
                                .textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
                        }
                        .defaultScrollAnchor(.bottom)
                    }
                }
                .frame(width: 200)
            }
        }
        .padding(8)
        .onAppear { goal = loop.goal }
    }

    private var controls: some View {
        HStack(spacing: 8) {
            Text(loop.name.replacingOccurrences(of: "_", with: " ")).font(Theme.sectionTitle)
            Text(loop.status.replacingOccurrences(of: "_", with: " ")).font(Theme.caps)
                .foregroundStyle(loopColor(loop.status))
                .padding(.horizontal, 5).background(loopColor(loop.status).opacity(0.15), in: Capsule())
            if !loop.stopReason.isEmpty { Text(loop.stopReason).font(Theme.label).foregroundStyle(Theme.textFaint).lineLimit(1) }
            Spacer()
            TextField("Goal", text: $goal).textFieldStyle(.plain).font(Theme.label)
                .padding(.horizontal, 6).frame(width: 240, height: 22)
                .background(Theme.field, in: RoundedRectangle(cornerRadius: 4))
            Stepper(iterations == 0 ? "iterations: loop's" : "iterations: \(iterations)", value: $iterations, in: 0...20)
                .font(Theme.label).fixedSize()
            if loop.status == "awaiting_approval" {
                Button("Approve", systemImage: "checkmark") { Task { await crew.approveLoop(loop.name, approved: true) } }
                    .controlSize(.small).buttonStyle(.borderedProminent)
                Button("Decline") { Task { await crew.approveLoop(loop.name, approved: false) } }.controlSize(.small)
            } else if crew.runningLoop == loop.name || loop.isActive {
                Button("Stop", systemImage: "stop.fill") { crew.stopLoop(loop.name) }.controlSize(.small)
            } else {
                Button("Run with Crew", systemImage: "play.fill") {
                    Task { await crew.runLoop(loop.name, goal: goal == loop.goal ? "" : goal, maxIterations: iterations) }
                }
                .controlSize(.small).buttonStyle(.borderedProminent)
                .disabled(crew.runningLoop != nil)
                .help("Run this loop with the in-editor crew. Headless: skywalker studio run --loop \(loop.name)")
            }
        }
    }
}

/// Iterations × stages grid: what ran, who ran it, what came out.
private struct LoopTimeline: View {
    @Environment(StudioStore.self) private var studio
    let loop: StudioLoop

    var body: some View {
        ScrollView([.horizontal, .vertical]) {
            Grid(alignment: .leading, horizontalSpacing: 4, verticalSpacing: 4) {
                GridRow {
                    Text("").frame(width: 26)
                    ForEach(loop.stages, id: \.id) { s in
                        Label(s.title, systemImage: s.kind == "playtest" ? "gamecontroller" : "person.2")
                            .font(Theme.caps).foregroundStyle(Theme.textDim).lineLimit(1).frame(width: 150, alignment: .leading)
                    }
                }
                ForEach(Array(loop.iterations.enumerated()), id: \.offset) { _, it in
                    GridRow {
                        Text("#\(it["n"].int ?? 0)").font(Theme.monoSmall).foregroundStyle(Theme.textFaint).frame(width: 26)
                        ForEach(loop.stages, id: \.id) { s in
                            StageCell(record: it["stages"].array.first { $0["id"].string == s.id },
                                      active: loop.status == "running" && loop.stage == s.id && (it["n"].int ?? 0) == loop.iteration)
                        }
                    }
                }
            }
            .padding(2)
        }
    }
}

private struct StageCell: View {
    @Environment(StudioStore.self) private var studio
    let record: JSON?
    let active: Bool

    var body: some View {
        let status = record?["status"].string ?? "pending"
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 3) {
                Image(systemName: icon(status)).font(.system(size: 9)).foregroundStyle(color(status))
                Text(status).font(Theme.caps).foregroundStyle(color(status))
                Spacer(minLength: 0)
                ForEach(record?["assignments"].array.compactMap(\.string) ?? [], id: \.self) { id in
                    if let a = studio.agent(id) { CloudAvatar(color: a.color, face: a.face, size: 10, working: active) }
                }
            }
            if let pt = record?["playtest"].string {
                let m = record?["metrics"] ?? .null
                Text("\(pt) · \(Int(((m["completion_rate"].number ?? 0) * 100).rounded()))% · \(String(format: "%.1f", m["deaths"].number ?? 0)) d")
                    .font(Theme.monoSmall).foregroundStyle(Theme.textDim)
            }
            if let filed = record?["filed"].array, !filed.isEmpty {
                Text("filed " + filed.compactMap { $0["id"].string }.joined(separator: " ")).font(Theme.monoSmall).foregroundStyle(Theme.warning)
            }
            if let verified = record?["verified"].array, !verified.isEmpty {
                Text(verified.map { "\($0["feedback"].string ?? "") \($0["effect"].string ?? "")" }.joined(separator: ", "))
                    .font(Theme.monoSmall).foregroundStyle(Theme.success).lineLimit(1)
            }
            if let reports = record?["reports"].array, let first = reports.first {
                Text(first["report"].string ?? "").font(.system(size: 9.5)).foregroundStyle(Theme.textFaint).lineLimit(2)
                    .help(reports.map { "@\($0["agent"].string ?? ""): \($0["report"].string ?? "")" }.joined(separator: "\n\n"))
            } else if status == "skipped", let s = record?["summary"].string {
                Text(s).font(.system(size: 9.5)).foregroundStyle(Theme.textFaint).lineLimit(2)
            }
        }
        .padding(5)
        .frame(width: 150, height: 64, alignment: .topLeading)
        .background(active ? Theme.ai.opacity(0.10) : Theme.panelRaised, in: RoundedRectangle(cornerRadius: 5))
        .overlay(RoundedRectangle(cornerRadius: 5).stroke(active ? Theme.ai.opacity(0.6) : Theme.border))
    }

    private func icon(_ s: String) -> String {
        switch s {
        case "done": "checkmark.circle.fill"
        case "running": "circle.dotted"
        case "skipped": "arrow.turn.down.right"
        case "failed": "xmark.octagon.fill"
        case "awaiting_approval": "hand.raised.fill"
        default: "circle"
        }
    }

    private func color(_ s: String) -> Color {
        switch s {
        case "done": Theme.success
        case "running": Theme.ai
        case "failed": Theme.error
        case "awaiting_approval": Theme.warning
        default: Theme.textFaint
        }
    }
}

/// Tiny line chart of a metric across iterations.
struct Sparkline: View {
    let values: [Double]

    var body: some View {
        Canvas { ctx, size in
            guard values.count > 0 else { return }
            let lo = values.min()!, hi = values.max()!
            let span = max(hi - lo, 1e-9)
            func point(_ i: Int) -> CGPoint {
                let x = values.count == 1 ? size.width / 2 : size.width * CGFloat(i) / CGFloat(values.count - 1)
                let y = size.height - 4 - (size.height - 8) * CGFloat((values[i] - lo) / span)
                return CGPoint(x: x, y: hi == lo ? size.height / 2 : y)
            }
            var path = Path()
            path.move(to: point(0))
            for i in values.indices.dropFirst() { path.addLine(to: point(i)) }
            ctx.stroke(path, with: .color(Theme.accent), lineWidth: 1.5)
            for i in values.indices {
                let p = point(i)
                ctx.fill(Path(ellipseIn: CGRect(x: p.x - 2.5, y: p.y - 2.5, width: 5, height: 5)), with: .color(Theme.accent))
            }
        }
        .background(Theme.field, in: RoundedRectangle(cornerRadius: 4))
        .overlay {
            if values.isEmpty { Text("no data yet").font(Theme.label).foregroundStyle(Theme.textFaint) }
        }
    }
}

// MARK: - Messages

private struct MessagesView: View {
    @Environment(StudioStore.self) private var studio
    @State private var channel = "general"
    @State private var draft = ""

    var body: some View {
        HStack(spacing: 0) {
            List(selection: $channel) {
                ForEach(studio.channels, id: \.self) { c in
                    HStack {
                        Text("#\(c)").font(Theme.body)
                        Spacer()
                        Text("\(studio.messages.filter { $0.channel == c }.count)").font(Theme.monoSmall).foregroundStyle(Theme.textFaint)
                    }
                    .tag(c)
                }
            }
            .listStyle(.plain).scrollContentBackground(.hidden)
            .frame(width: 150)
            Rectangle().fill(Theme.border).frame(width: 1)
            VStack(spacing: 0) {
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 5) {
                        ForEach(studio.messages.filter { $0.channel == channel }) { MessageRow(message: $0) }
                    }
                    .padding(8)
                }
                .defaultScrollAnchor(.bottom)
                HStack(spacing: 6) {
                    TextField("Message #\(channel) — @mention agents, roles or disciplines", text: $draft)
                        .textFieldStyle(.plain).font(Theme.body).onSubmit(send)
                    Button(action: send) { Image(systemName: "arrow.up.circle.fill").font(.system(size: 16)) }
                        .buttonStyle(.plain).foregroundStyle(draft.isEmpty ? Theme.textFaint : Theme.accent).disabled(draft.isEmpty)
                }
                .padding(7)
                .background(Theme.field)
                .overlay(alignment: .top) { Rectangle().fill(Theme.border).frame(height: 1) }
            }
        }
    }

    private func send() {
        guard !draft.isEmpty else { return }
        studio.engine.call("studio_message_send", ["text": .string(draft), "channel": .string(channel)], actor: "user")
        draft = ""
        studio.refresh()
    }
}

private struct MessageRow: View {
    @Environment(StudioStore.self) private var studio
    let message: StudioMessage

    var body: some View {
        HStack(alignment: .top, spacing: 6) {
            if let a = studio.agent(message.from) {
                CloudAvatar(color: a.color, face: a.face, size: 13)
            } else {
                Image(systemName: message.from.hasPrefix("mcp:") ? "link" : "person.crop.circle.fill")
                    .font(.system(size: 12)).foregroundStyle(Theme.textDim)
            }
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 5) {
                    Text(studio.agent(message.from)?.name ?? (message.from == "user" ? "You" : message.from)).font(Theme.sectionTitle)
                    Text(message.time).font(Theme.monoSmall).foregroundStyle(Theme.textFaint)
                    if !message.to.isEmpty {
                        Text("→ " + message.to.map { "@\($0)" }.joined(separator: " ")).font(Theme.monoSmall).foregroundStyle(Theme.textFaint)
                    }
                }
                Text(message.text).font(Theme.body).textSelection(.enabled)
            }
        }
        .padding(.leading, message.thread.isEmpty ? 0 : 18)
    }
}
