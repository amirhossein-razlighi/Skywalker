import SwiftUI

/// The "Sky Crew" strip above the viewport: every Cloudling, who is working, and a way in.
struct CrewStrip: View {
    @Environment(CrewStore.self) private var crew
    @Binding var focused: UUID?

    var body: some View {
        // Scrollable so the strip never imposes a minimum width on the window (a fixed-width
        // row next to the inspector made AppKit's constraint passes oscillate).
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 14) {
            ForEach(crew.cloudlings) { c in
                Button { focused = c.id } label: {
                    VStack(spacing: 2) {
                        CloudAvatar(color: c.color, face: c.face, accessory: c.role.symbol, size: 30,
                                    working: crew.isWorking(c))
                        Text(c.name).font(.caption2.weight(focused == c.id ? .bold : .regular))
                    }
                    .padding(.horizontal, 4)
                    .background(focused == c.id ? Theme.sky.opacity(0.12) : .clear, in: RoundedRectangle(cornerRadius: 8))
                }
                .buttonStyle(.plain)
                .help("\(c.name) — \(c.role.title)\(crew.isWorking(c) ? " (working)" : "")")
            }
            if !crew.pendingApprovals.isEmpty {
                Label("\(crew.pendingApprovals.count) waiting for approval", systemImage: "hand.raised.fill")
                    .font(.caption).foregroundStyle(Theme.dawn)
            }
            }
            .padding(.horizontal, 12)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.vertical, 6)
        .background(.bar)
    }
}

/// Chat with one Cloudling; shows its tool calls and screenshots inline.
struct CrewChatView: View {
    @Environment(CrewStore.self) private var crew
    let cloudling: Cloudling
    @State private var draft = ""

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                CloudAvatar(color: cloudling.color, face: cloudling.face, accessory: cloudling.role.symbol, size: 26,
                            working: crew.isWorking(cloudling))
                VStack(alignment: .leading, spacing: 0) {
                    Text(cloudling.name).font(.headline)
                    Text("\(cloudling.role.title) · \(cloudling.autonomy.label)").font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                if crew.isWorking(cloudling) {
                    Button("Stop", systemImage: "stop.fill") { crew.stop(cloudling) }.controlSize(.small)
                }
                Button("New chat", systemImage: "square.and.pencil") { crew.resetConversation(cloudling) }
                    .labelStyle(.iconOnly).buttonStyle(.borderless)
            }
            .padding(8)
            Divider()
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 8) {
                        ForEach(crew.transcript(for: cloudling)) { entry in
                            ChatRow(entry: entry, cloudling: cloudling).id(entry.id)
                        }
                        ForEach(crew.pendingApprovals.filter { $0.cloudling.id == cloudling.id }) { approval in
                            ApprovalCard(approval: approval)
                        }
                    }
                    .padding(10)
                }
                .onChange(of: crew.transcript(for: cloudling).count) { _, _ in
                    if let last = crew.transcript(for: cloudling).last { withAnimation { proxy.scrollTo(last.id, anchor: .bottom) } }
                }
            }
            Divider()
            HStack {
                TextField("Ask \(cloudling.name)…", text: $draft, axis: .vertical)
                    .textFieldStyle(.plain)
                    .lineLimit(1...5)
                    .onSubmit(send)
                Button("Send", systemImage: "paperplane.fill", action: send)
                    .labelStyle(.iconOnly)
                    .disabled(draft.trimmingCharacters(in: .whitespaces).isEmpty || crew.isWorking(cloudling))
            }
            .padding(8)
        }
    }

    private func send() {
        let text = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }
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
            HStack {
                Spacer(minLength: 40)
                Text(entry.text).textSelection(.enabled)
                    .padding(8).background(Theme.sky.opacity(0.18), in: RoundedRectangle(cornerRadius: 10))
            }
        case .agent:
            HStack(alignment: .top) {
                CloudAvatar(color: cloudling.color, face: cloudling.face, size: 18)
                Text(LocalizedStringKey(entry.text)).textSelection(.enabled)
                    .padding(8).background(.background.secondary, in: RoundedRectangle(cornerRadius: 10))
                Spacer(minLength: 20)
            }
        case .tool:
            VStack(alignment: .leading, spacing: 4) {
                Button { expanded.toggle() } label: {
                    Label(entry.toolName ?? "tool", systemImage: entry.isError ? "exclamationmark.triangle" : "hammer")
                        .font(.caption.monospaced())
                        .foregroundStyle(entry.isError ? Theme.rose : .secondary)
                }
                .buttonStyle(.plain)
                if expanded {
                    Text(entry.text).font(Theme.monoSmall).textSelection(.enabled).foregroundStyle(.secondary)
                }
                if let b64 = entry.imageBase64, let data = Data(base64Encoded: b64), let img = NSImage(data: data) {
                    Image(nsImage: img).resizable().scaledToFit().frame(maxWidth: 260)
                        .clipShape(RoundedRectangle(cornerRadius: 6))
                }
            }
            .padding(.leading, 26)
        case .system:
            Text(entry.text).font(.caption).foregroundStyle(.secondary).frame(maxWidth: .infinity)
        case .error:
            Label(entry.text, systemImage: "xmark.octagon").font(.callout).foregroundStyle(Theme.rose)
        }
    }
}

struct ApprovalCard: View {
    @Environment(CrewStore.self) private var crew
    let approval: PendingApproval

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Label("\(approval.cloudling.name) wants to run \(approval.call.name)", systemImage: "hand.raised.fill")
                .font(.callout.weight(.semibold))
            Text(approval.call.arguments.serialized(pretty: true)).font(Theme.monoSmall).lineLimit(8)
            HStack {
                Button("Allow") { crew.resolve(approval, approved: true) }.buttonStyle(.borderedProminent)
                Button("Decline") { crew.resolve(approval, approved: false) }
            }
            .controlSize(.small)
        }
        .card()
    }
}

/// Edit and run pipelines of Cloudlings ("flight plans").
struct FlightPlanView: View {
    @Environment(CrewStore.self) private var crew
    @State private var selectedPlan: UUID?
    @State private var goal = ""

    var body: some View {
        @Bindable var crew = crew
        HSplitView {
            List(selection: $selectedPlan) {
                ForEach(crew.plans) { plan in Text(plan.name).tag(plan.id) }
            }
            .frame(minWidth: 150, maxWidth: 220)
            .safeAreaInset(edge: .bottom) {
                Button("New Flight Plan", systemImage: "plus") {
                    let plan = FlightPlan(name: "Flight Plan \(crew.plans.count + 1)", steps: [])
                    crew.plans.append(plan)
                    selectedPlan = plan.id
                    crew.save()
                }
                .buttonStyle(.borderless).padding(6)
            }

            if let index = crew.plans.firstIndex(where: { $0.id == selectedPlan }) {
                VStack(alignment: .leading, spacing: 10) {
                    TextField("Plan name", text: $crew.plans[index].name).font(.headline).textFieldStyle(.plain)
                    ScrollView(.horizontal) {
                        HStack(alignment: .top, spacing: 8) {
                            ForEach($crew.plans[index].steps) { $step in
                                StepCard(step: $step) {
                                    crew.plans[index].steps.removeAll { $0.id == step.id }
                                }
                                Image(systemName: "arrow.right").foregroundStyle(.tertiary).padding(.top, 30)
                            }
                            Menu {
                                ForEach(crew.cloudlings) { c in
                                    Button(c.name) {
                                        crew.plans[index].steps.append(FlightStep(cloudlingID: c.id, instruction: "Describe this step…"))
                                    }
                                }
                            } label: { Label("Step", systemImage: "plus.circle") }
                            .fixedSize()
                            .padding(.top, 24)
                        }
                        .padding(4)
                    }
                    HStack {
                        TextField("Goal for this run, e.g. “a cozy sky village with a hidden coin”", text: $goal)
                        Button(crew.runningPlan ? "Flying…" : "Run", systemImage: "airplane.departure") {
                            crew.save()
                            let plan = crew.plans[index]
                            Task { await crew.run(plan: plan, goal: goal) }
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(crew.runningPlan || goal.isEmpty || crew.plans[index].steps.isEmpty)
                    }
                    ScrollView {
                        Text(crew.planLog.joined(separator: "\n")).font(Theme.monoSmall).textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                .padding(10)
                .onDisappear { crew.save() }
            } else {
                ContentUnavailableView("Flight Plans", systemImage: "point.3.connected.trianglepath.dotted",
                                       description: Text("Chain Cloudlings into a pipeline. Each step gets the goal and the reports of earlier steps. Or let Nimbus, your director, delegate on the fly."))
            }
        }
        .onAppear { selectedPlan = selectedPlan ?? crew.plans.first?.id }
    }
}

private struct StepCard: View {
    @Environment(CrewStore.self) private var crew
    @Binding var step: FlightStep
    let onDelete: () -> Void

    var body: some View {
        let c = crew.cloudlings.first { $0.id == step.cloudlingID }
        VStack(spacing: 6) {
            if let c {
                CloudAvatar(color: c.color, face: c.face, accessory: c.role.symbol, size: 28, working: crew.isWorking(c))
                Text(c.name).font(.caption.weight(.semibold))
            }
            TextEditor(text: $step.instruction)
                .font(.caption)
                .frame(width: 170, height: 70)
                .scrollContentBackground(.hidden)
                .background(.background, in: RoundedRectangle(cornerRadius: 6))
            Button("Remove", systemImage: "trash", role: .destructive, action: onDelete)
                .labelStyle(.iconOnly).buttonStyle(.borderless)
        }
        .card()
    }
}

/// Who did what, live. Agents appear with their cloud avatars; external MCP agents too.
struct ActivityView: View {
    @Environment(EngineStore.self) private var engine
    @Environment(CrewStore.self) private var crew

    var body: some View {
        List(engine.activity.reversed()) { item in
            HStack(spacing: 8) {
                avatar(for: item.actor)
                VStack(alignment: .leading, spacing: 1) {
                    Text(displayName(item.actor)).font(.caption.weight(.semibold))
                    Text(item.kind == "tool" ? "used \(item.text)" : item.text).font(.caption)
                        .foregroundStyle(item.ok ? Color.primary : Theme.rose)
                }
                Spacer()
                Text(item.date, style: .time).font(.caption2).foregroundStyle(.tertiary)
            }
        }
        .overlay {
            if engine.activity.isEmpty {
                ContentUnavailableView("No activity yet", systemImage: "cloud",
                                       description: Text("Edits by you, your crew and connected agents appear here."))
            }
        }
    }

    @ViewBuilder
    private func avatar(for actor: String) -> some View {
        if let c = crew.cloudling(actor: actor) {
            CloudAvatar(color: c.color, face: c.face, size: 18, working: crew.isWorking(c))
        } else if actor.hasPrefix("mcp:") {
            CloudAvatar(color: .gray, face: .curious, accessory: "link", size: 18)
        } else {
            Image(systemName: "person.crop.circle.fill").foregroundStyle(Theme.sky).frame(width: 24)
        }
    }

    private func displayName(_ actor: String) -> String {
        if actor.hasPrefix("agent:") { return String(actor.dropFirst(6)) }
        if actor.hasPrefix("mcp:") { return String(actor.dropFirst(4)) + " (external)" }
        return actor == "user" ? "You" : actor
    }
}

struct ConsoleView: View {
    @Environment(EngineStore.self) private var engine
    var body: some View {
        ScrollView {
            Text(engine.logs.joined(separator: "\n"))
                .font(Theme.monoSmall).textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading).padding(8)
        }
        .defaultScrollAnchor(.bottom)
        .overlay {
            if engine.logs.isEmpty {
                ContentUnavailableView("Console", systemImage: "terminal", description: Text("Wander `log` output and script errors appear here while playing."))
            }
        }
    }
}
