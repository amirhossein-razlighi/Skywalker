import SwiftUI

/// Crew roster + conversation with the focused agent.
struct AgentsPanel: View {
    @Environment(CrewStore.self) private var crew
    @Binding var focused: UUID?

    var body: some View {
        HStack(spacing: 0) {
            VStack(spacing: 0) {
                List(selection: $focused) {
                    ForEach(crew.cloudlings) { c in
                        HStack(spacing: 8) {
                            CloudAvatar(color: c.color, face: c.face, size: 16, working: crew.isWorking(c))
                            VStack(alignment: .leading, spacing: 0) {
                                Text(c.name).font(Theme.body)
                                Text(c.role.title).font(.system(size: 10)).foregroundStyle(Theme.textFaint)
                            }
                            Spacer()
                            if crew.isWorking(c) { ProgressView().controlSize(.mini) }
                        }
                        .tag(c.id)
                    }
                }
                .listStyle(.plain)
                .scrollContentBackground(.hidden)
                HStack {
                    SettingsLink { Label("Manage crew", systemImage: "gearshape").font(Theme.label) }
                        .buttonStyle(.borderless)
                    Spacer()
                }
                .padding(6)
            }
            .frame(width: 190)
            .background(Theme.panel)
            Rectangle().fill(Theme.border).frame(width: 1)
            if let c = crew.cloudlings.first(where: { $0.id == focused }) ?? crew.cloudlings.first {
                ChatView(cloudling: c).id(c.id)
            } else {
                Text("No crew members").foregroundStyle(Theme.textFaint).frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .onAppear { if focused == nil { focused = crew.cloudlings.first?.id } }
    }
}

struct ChatView: View {
    @Environment(CrewStore.self) private var crew
    let cloudling: Cloudling
    @State private var draft = ""
    @State private var designing = false

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                CloudAvatar(color: cloudling.color, face: cloudling.face, size: 16, working: crew.isWorking(cloudling))
                Text(cloudling.name).font(Theme.sectionTitle)
                Text("\(cloudling.role.title) · \(crew.modelLabel(for: cloudling)) · \(cloudling.autonomy.label)")
                    .font(Theme.label).foregroundStyle(Theme.textFaint).lineLimit(1)
                Spacer()
                let u = crew.usage(for: cloudling)
                if u.requests > 0 {
                    Text("\(Self.compact(u.input + u.cacheRead)) in · \(Self.compact(u.output)) out")
                        .font(Theme.monoSmall).foregroundStyle(Theme.textFaint)
                        .help("Tokens used by \(cloudling.name) (\(u.requests) requests, \(u.cacheRead.formatted()) cached)")
                }
                if crew.isWorking(cloudling) {
                    Button("Stop", systemImage: "stop.circle") { crew.stop(cloudling) }.controlSize(.small)
                }
                IconButton(symbol: "slider.horizontal.3", help: "Design \(cloudling.name): role, instructions, permissions, memory") {
                    designing = true
                }
                IconButton(symbol: "square.and.pencil", help: "New conversation") { crew.resetConversation(cloudling) }
            }
            .padding(.horizontal, 10)
            .frame(height: 30)
            .overlay(alignment: .bottom) { Rectangle().fill(Theme.border).frame(height: 1) }

            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 6) {
                        ForEach(crew.transcript(for: cloudling)) { entry in
                            ChatRow(entry: entry, cloudling: cloudling).id(entry.id)
                        }
                        ForEach(crew.pendingApprovals.filter { $0.cloudling.id == cloudling.id }) { approval in
                            ApprovalCard(approval: approval)
                        }
                        if crew.transcript(for: cloudling).isEmpty {
                            Text("Ask \(cloudling.name) to do something in the scene — e.g. “build a small harbor with three boats”.")
                                .font(Theme.label).foregroundStyle(Theme.textFaint).padding(.top, 8)
                        }
                    }
                    .padding(10)
                }
                .onChange(of: crew.transcript(for: cloudling).count) { _, _ in
                    if let last = crew.transcript(for: cloudling).last { proxy.scrollTo(last.id, anchor: .bottom) }
                }
            }

            HStack(alignment: .bottom, spacing: 6) {
                TextField("Message \(cloudling.name)…  (⏎ to send)", text: $draft, axis: .vertical)
                    .textFieldStyle(.plain)
                    .font(Theme.body)
                    .lineLimit(1...6)
                    .onSubmit(send)
                Button(action: send) { Image(systemName: "arrow.up.circle.fill").font(.system(size: 17)) }
                    .buttonStyle(.plain)
                    .foregroundStyle(canSend ? Theme.accent : Theme.textFaint)
                    .disabled(!canSend)
            }
            .padding(8)
            .background(Theme.field)
            .overlay(alignment: .top) { Rectangle().fill(Theme.border).frame(height: 1) }
        }
        .sheet(isPresented: $designing) {
            VStack(spacing: 0) {
                AgentDesigner(cloudlingID: cloudling.id)
                HStack {
                    Spacer()
                    Button("Done") { designing = false }.keyboardShortcut(.defaultAction)
                }
                .padding(10)
            }
            .frame(width: 620, height: 680)
        }
    }

    static func compact(_ n: Int) -> String {
        n >= 1_000_000 ? String(format: "%.1fM", Double(n) / 1e6) : n >= 1000 ? String(format: "%.1fk", Double(n) / 1e3) : "\(n)"
    }

    private var canSend: Bool { !draft.trimmingCharacters(in: .whitespaces).isEmpty && !crew.isWorking(cloudling) }

    private func send() {
        let text = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        guard canSend else { return }
        draft = ""
        Task { await crew.send(text, to: cloudling) }
    }
}

struct ChatRow: View {
    let entry: ChatEntry
    let cloudling: Cloudling
    @State private var expanded = false

    var body: some View {
        switch entry.role {
        case .user:
            HStack(alignment: .top, spacing: 8) {
                Image(systemName: "person.crop.circle.fill").foregroundStyle(Theme.textDim)
                Text(entry.text).font(Theme.body).textSelection(.enabled)
            }
        case .agent:
            HStack(alignment: .top, spacing: 8) {
                CloudAvatar(color: cloudling.color, face: cloudling.face, size: 13)
                Text(LocalizedStringKey(entry.text)).font(Theme.body).textSelection(.enabled)
            }
        case .tool:
            VStack(alignment: .leading, spacing: 4) {
                Button { expanded.toggle() } label: {
                    HStack(spacing: 4) {
                        Image(systemName: expanded ? "chevron.down" : "chevron.right").font(.system(size: 8))
                        Image(systemName: entry.isError ? "exclamationmark.triangle.fill" : "hammer")
                            .foregroundStyle(entry.isError ? Theme.error : Theme.textFaint)
                        Text(entry.toolName ?? "tool").font(Theme.monoSmall)
                        Text(entry.text.prefix(90).replacingOccurrences(of: "\n", with: " ")).font(Theme.monoSmall)
                            .foregroundStyle(Theme.textFaint).lineLimit(1)
                    }
                }
                .buttonStyle(.plain)
                if expanded {
                    Text(entry.text).font(Theme.monoSmall).foregroundStyle(Theme.textDim).textSelection(.enabled)
                        .padding(6).background(Theme.field, in: RoundedRectangle(cornerRadius: 4))
                }
                if let b64 = entry.imageBase64, let data = Data(base64Encoded: b64), let img = NSImage(data: data) {
                    Image(nsImage: img).resizable().scaledToFit().frame(maxWidth: 240)
                        .clipShape(RoundedRectangle(cornerRadius: 4))
                        .overlay(RoundedRectangle(cornerRadius: 4).stroke(Theme.border))
                }
            }
            .padding(.leading, 26)
        case .system:
            Text(entry.text).font(Theme.label).foregroundStyle(Theme.textFaint).padding(.leading, 26)
        case .error:
            Label(entry.text, systemImage: "xmark.octagon").font(Theme.label).foregroundStyle(Theme.error)
                .textSelection(.enabled)
        }
    }
}

struct ApprovalCard: View {
    @Environment(CrewStore.self) private var crew
    let approval: PendingApproval

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Label("\(approval.cloudling.name) wants to run \(approval.call.name)", systemImage: "hand.raised.fill")
                .font(Theme.sectionTitle).foregroundStyle(Theme.warning)
            Text(approval.call.arguments.serialized(pretty: true)).font(Theme.monoSmall).lineLimit(10)
                .foregroundStyle(Theme.textDim)
            HStack {
                Button("Allow") { crew.resolve(approval, approved: true) }.buttonStyle(.borderedProminent)
                Button("Decline") { crew.resolve(approval, approved: false) }
            }
            .controlSize(.small)
        }
        .padding(8)
        .background(Theme.warning.opacity(0.08), in: RoundedRectangle(cornerRadius: 6))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(Theme.warning.opacity(0.4)))
    }
}

/// Edit and run pipelines of agents. Each step receives the goal plus earlier reports.
struct PipelinesPanel: View {
    @Environment(CrewStore.self) private var crew
    @State private var selected: UUID?
    @State private var goal = ""

    var body: some View {
        @Bindable var crew = crew
        HStack(spacing: 0) {
            VStack(spacing: 0) {
                List(selection: $selected) {
                    ForEach(crew.plans) { Text($0.name).font(Theme.body).tag($0.id) }
                }
                .listStyle(.plain).scrollContentBackground(.hidden)
                HStack {
                    Button {
                        let plan = FlightPlan(name: "Pipeline \(crew.plans.count + 1)", steps: [])
                        crew.plans.append(plan)
                        selected = plan.id
                        crew.save()
                    } label: { Label("New", systemImage: "plus").font(Theme.label) }
                    .buttonStyle(.borderless)
                    Spacer()
                    if let selected {
                        Button(role: .destructive) {
                            crew.plans.removeAll { $0.id == selected }
                            self.selected = nil
                            crew.save()
                        } label: { Image(systemName: "trash") }
                        .buttonStyle(.borderless)
                    }
                }
                .padding(6)
            }
            .frame(width: 190)
            Rectangle().fill(Theme.border).frame(width: 1)
            if let index = crew.plans.firstIndex(where: { $0.id == selected }) {
                VStack(alignment: .leading, spacing: 8) {
                    TextField("Name", text: $crew.plans[index].name).textFieldStyle(.plain).font(Theme.sectionTitle)
                    ScrollView(.horizontal) {
                        HStack(alignment: .center, spacing: 6) {
                            ForEach(Array(CrewStore.stages(of: crew.plans[index]).enumerated()), id: \.offset) { _, stage in
                                VStack(spacing: 4) {
                                    ForEach(stage) { step in
                                        if let k = crew.plans[index].steps.firstIndex(where: { $0.id == step.id }) {
                                            StepCard(step: $crew.plans[index].steps[k], canParallel: k > 0) {
                                                crew.plans[index].steps.removeAll { $0.id == step.id }
                                            }
                                        }
                                    }
                                }
                                .padding(stage.count > 1 ? 4 : 0)
                                .background(stage.count > 1 ? Theme.accent.opacity(0.06) : .clear, in: RoundedRectangle(cornerRadius: 8))
                                .overlay {
                                    if stage.count > 1 {
                                        RoundedRectangle(cornerRadius: 8).stroke(Theme.accent.opacity(0.35), style: StrokeStyle(lineWidth: 1, dash: [3, 3]))
                                    }
                                }
                                Image(systemName: "arrow.right").font(.system(size: 10)).foregroundStyle(Theme.textFaint)
                            }
                            Menu {
                                ForEach(crew.cloudlings) { c in
                                    Button("\(c.name) — \(c.role.title)") {
                                        crew.plans[index].steps.append(FlightStep(cloudlingID: c.id, instruction: ""))
                                    }
                                }
                            } label: { Label("Add Step", systemImage: "plus").font(Theme.label) }
                            .menuStyle(.borderlessButton).fixedSize()
                        }
                        .padding(2)
                    }
                    HStack {
                        TextField("Goal for this run, e.g. “a moody lighthouse level with one secret”", text: $goal)
                            .textFieldStyle(.plain).font(Theme.body)
                            .padding(.horizontal, 6).frame(height: 24)
                            .background(Theme.field, in: RoundedRectangle(cornerRadius: 4))
                        Button(crew.runningPlan ? "Running…" : "Run", systemImage: "play.fill") {
                            crew.save()
                            let plan = crew.plans[index]
                            Task { await crew.run(plan: plan, goal: goal) }
                        }
                        .buttonStyle(.borderedProminent).controlSize(.small)
                        .disabled(crew.runningPlan || goal.isEmpty || crew.plans[index].steps.isEmpty)
                    }
                    ScrollView {
                        Text(crew.planLog.joined(separator: "\n")).font(Theme.monoSmall).foregroundStyle(Theme.textDim)
                            .textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                .padding(10)
                .onChange(of: crew.plans) { _, _ in crew.save() }
            } else {
                VStack(spacing: 6) {
                    Text("Pipelines").font(Theme.sectionTitle)
                    Text("Chain agents into stages — steps can run in parallel — or let your director delegate dynamically with crew_delegate.")
                        .font(Theme.label).foregroundStyle(Theme.textFaint).multilineTextAlignment(.center)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .padding()
            }
        }
        .onAppear { selected = selected ?? crew.plans.first?.id }
    }
}

private struct StepCard: View {
    @Environment(CrewStore.self) private var crew
    @Binding var step: FlightStep
    var canParallel = false
    let onDelete: () -> Void

    var body: some View {
        let c = crew.cloudlings.first { $0.id == step.cloudlingID }
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 6) {
                if let c {
                    CloudAvatar(color: c.color, face: c.face, size: 13, working: crew.isWorking(c))
                    Text(c.name).font(Theme.sectionTitle)
                }
                Spacer()
                if canParallel {
                    Button { step.parallelWithPrevious.toggle() } label: {
                        Image(systemName: "arrow.triangle.branch").font(.system(size: 10))
                            .foregroundStyle(step.parallelWithPrevious ? Theme.accent : Theme.textFaint)
                    }
                    .buttonStyle(.borderless)
                    .help(step.parallelWithPrevious ? "Runs in parallel with the previous step" : "Run in parallel with the previous step")
                }
                Button(action: onDelete) { Image(systemName: "xmark").font(.system(size: 9)) }.buttonStyle(.borderless)
            }
            TextField("What should this step do?", text: $step.instruction, axis: .vertical)
                .textFieldStyle(.plain).font(Theme.label).lineLimit(2...4)
        }
        .padding(7)
        .frame(width: 190)
        .background(Theme.panelRaised, in: RoundedRectangle(cornerRadius: 6))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(Theme.border))
    }
}

struct ActivityPanel: View {
    @Environment(EngineStore.self) private var engine
    @Environment(CrewStore.self) private var crew

    var body: some View {
        List(engine.activity.reversed()) { item in
            HStack(spacing: 8) {
                Text(item.date, format: .dateTime.hour().minute().second()).font(Theme.monoSmall).foregroundStyle(Theme.textFaint)
                actorBadge(item.actor)
                Text(item.kind == "tool" ? "→ \(item.text)" : item.text).font(Theme.body)
                    .foregroundStyle(item.ok ? Theme.text : Theme.error).lineLimit(1)
                Spacer()
            }
            .listRowSeparator(.hidden)
        }
        .listStyle(.plain)
        .scrollContentBackground(.hidden)
        .environment(\.defaultMinListRowHeight, 20)
        .overlay {
            if engine.activity.isEmpty {
                Text("Edits by you, your crew and connected agents appear here.").font(Theme.label).foregroundStyle(Theme.textFaint)
            }
        }
    }

    @ViewBuilder
    private func actorBadge(_ actor: String) -> some View {
        HStack(spacing: 4) {
            if let c = crew.cloudling(actor: actor) {
                CloudAvatar(color: c.color, face: c.face, size: 12)
                Text(c.name)
            } else if actor.hasPrefix("mcp:") {
                Image(systemName: "link").font(.system(size: 9))
                Text(String(actor.dropFirst(4)))
            } else {
                Image(systemName: "person.fill").font(.system(size: 9))
                Text(actor == "user" ? "You" : actor)
            }
        }
        .font(Theme.label.weight(.medium))
        .foregroundStyle(Theme.textDim)
        .frame(width: 120, alignment: .leading)
    }
}

struct ConsolePanel: View {
    @Environment(EngineStore.self) private var engine
    @State private var filter = ""

    var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 1) {
                    ForEach(Array(lines.enumerated()), id: \.offset) { _, line in
                        Text(line).font(Theme.monoSmall).foregroundStyle(color(for: line)).textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                .padding(8)
            }
            .defaultScrollAnchor(.bottom)
            .overlay {
                if engine.logs.isEmpty {
                    Text("Wander `log` output and script errors appear here while the game runs.")
                        .font(Theme.label).foregroundStyle(Theme.textFaint)
                }
            }
            HStack(spacing: 6) {
                Image(systemName: "line.3.horizontal.decrease").font(.system(size: 10)).foregroundStyle(Theme.textFaint)
                TextField("Filter", text: $filter).textFieldStyle(.plain).font(Theme.label)
                Spacer()
                Button("Clear") { engine.clearLogs() }.buttonStyle(.borderless).font(Theme.label)
            }
            .padding(.horizontal, 8)
            .frame(height: 22)
            .background(Theme.header)
        }
    }

    private var lines: [String] { filter.isEmpty ? engine.logs : engine.logs.filter { $0.localizedCaseInsensitiveContains(filter) } }

    private func color(for line: String) -> Color {
        if line.contains("[runtime_error]") || line.contains("[compile_error]") { return Theme.error }
        return Theme.textDim
    }
}
