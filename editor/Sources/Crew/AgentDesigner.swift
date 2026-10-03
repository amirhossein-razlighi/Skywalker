import SwiftUI

/// Full editor for one studio agent: identity, role and focus, instructions, brain,
/// per-category tool permissions, playtester persona, long-term memory and usage.
/// Edits are saved (debounced) to the project's agents/<id>.agent.json through
/// `studio_agent_define`, so the CLI runner and external agents see the same agent.
struct AgentDesigner: View {
    @Environment(CrewStore.self) private var crew
    let cloudlingID: String
    @State private var draft: Cloudling?
    @State private var saveTask: Task<Void, Never>?
    @State private var newNote = ""
    @State private var tagsText = ""

    var body: some View {
        Group {
            if let d = Binding($draft) {
                form(d)
            } else {
                ContentUnavailableView("Select an agent", systemImage: "cloud")
            }
        }
        .onAppear(perform: reload)
        .onDisappear(perform: commit)
        .onChange(of: draft) { old, new in
            guard old != nil, new != nil, old?.id == new?.id else { return }
            scheduleSave()
        }
    }

    private func reload() {
        draft = crew.cloudlings.first { $0.id == cloudlingID }
        tagsText = draft?.focusTags.joined(separator: ", ") ?? ""
    }

    private func scheduleSave() {
        saveTask?.cancel()
        saveTask = Task {
            try? await Task.sleep(for: .milliseconds(700))
            if !Task.isCancelled { commit() }
        }
    }

    private func commit() {
        saveTask?.cancel()
        guard let d = draft, let current = crew.cloudlings.first(where: { $0.id == d.id }), current != d else { return }
        crew.define(d)
    }

    @ViewBuilder
    private func form(_ c: Binding<Cloudling>) -> some View {
        let agent = c.wrappedValue
        Form {
            Section {
                HStack(spacing: 14) {
                    CloudAvatar(color: agent.color, face: agent.face, size: 56, working: crew.isWorking(agent))
                    VStack(alignment: .leading, spacing: 6) {
                        TextField("Name", text: c.name).font(.title3.weight(.semibold)).textFieldStyle(.plain)
                        HStack {
                            Text("@\(agent.id)").font(Theme.monoSmall).foregroundStyle(.secondary)
                            Picker("Face", selection: c.face) {
                                ForEach(CloudFace.allCases, id: \.self) { Text($0.rawValue.capitalized).tag($0) }
                            }
                            .labelsHidden().fixedSize()
                            ColorPicker("Color", selection: Binding(get: { agent.color }, set: { c.wrappedValue.colorHex = $0.hexString }))
                                .labelsHidden()
                        }
                    }
                }
                .padding(.vertical, 4)
            }

            Section("Role & focus") {
                Picker("Role", selection: c.roleID) {
                    ForEach(disciplines, id: \.self) { d in
                        Section(d.capitalized) {
                            ForEach(CrewRole.all.filter { $0.discipline == d }) { Label($0.title, systemImage: $0.symbol).tag($0.id) }
                        }
                    }
                }
                TextField("Focus — what exactly this agent owns", text: c.focus)
                TextField("Focus tags (comma separated)", text: $tagsText)
                    .onChange(of: tagsText) { _, v in
                        c.wrappedValue.focusTags = v.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
                    }
                Picker("Reports to", selection: c.reportsTo) {
                    Text("Nobody").tag("")
                    ForEach(crew.cloudlings.filter { $0.id != agent.id }) { Text("\($0.name) — \($0.role.title)").tag($0.id) }
                }
                TextField("Mission", text: Binding(
                    get: { agent.missionOverride.isEmpty ? agent.role.mission : agent.missionOverride },
                    set: { c.wrappedValue.missionOverride = $0 == agent.role.mission ? "" : $0 }), axis: .vertical)
                    .lineLimit(2...6)
                if !agent.missionOverride.isEmpty {
                    Button("Reset to the role's mission") { c.wrappedValue.missionOverride = "" }.buttonStyle(.link).font(.caption)
                }
                TextField("Personality", text: c.personality, axis: .vertical)
                TextField("Standing instructions (style guides, constraints, conventions…)", text: c.instructions, axis: .vertical)
                    .lineLimit(3...10)
            }

            Section("Brain") {
                Picker("Provider", selection: Binding(
                    get: { crew.provider(for: agent)?.id },
                    set: { id in
                        guard let p = crew.providers.first(where: { $0.id == id }) else { return }
                        crew.setProviderOverride(p, forAgent: agent.id)
                        c.wrappedValue.provider = p.name
                    })) {
                    ForEach(crew.providers) { Text($0.name).tag(Optional($0.id)) }
                }
                TextField("Model", text: c.model, prompt: Text(crew.provider(for: agent)?.defaultModel ?? "provider default"))
                Stepper("Max tool rounds per task: \(agent.maxRounds)", value: c.maxRounds, in: 5...200, step: 5)
            }

            Section {
                Picker("Autonomy", selection: c.autonomy) {
                    ForEach(Autonomy.allCases) { Text($0.label).tag($0) }
                }
                ForEach(ToolCategory.all) { cat in
                    HStack {
                        Label {
                            VStack(alignment: .leading, spacing: 1) {
                                Text(cat.title)
                                Text(cat.detail).font(.caption).foregroundStyle(.secondary)
                            }
                        } icon: { Image(systemName: cat.symbol).frame(width: 18) }
                        Spacer()
                        Picker(cat.title, selection: Binding(
                            get: { agent.toolAccess[cat.id] ?? .inherit },
                            set: { c.wrappedValue.toolAccess[cat.id] = $0 == .inherit ? nil : $0 })) {
                            ForEach(ToolAccess.allCases) { Text($0.label).tag($0) }
                        }
                        .labelsHidden().pickerStyle(.segmented).frame(width: 230)
                    }
                }
            } header: {
                Text("Permissions")
            } footer: {
                Text("Default follows autonomy. Ask = mutating tools in that group wait for your approval; Off removes the group. "
                    + "Studio tools (board, feedback, messages, playtests) are always available unless turned off. "
                    + "\(enabledToolCount(agent)) of \(crew.engineTools().count) engine tools available.")
                    .font(.caption).foregroundStyle(.secondary)
            }

            if agent.roleID == "playtester" || agent.discipline == "qa" {
                Section {
                    Picker("Bot policy", selection: knob(c, "policy", "goal_seeker")) {
                        Text("Goal seeker").tag("goal_seeker")
                        Text("Explorer").tag("explorer")
                        Text("Random").tag("random")
                    }
                    slider(c, "reaction_time", "Reaction time", 0.05...1.5, 0.25, "%.2f s")
                    slider(c, "skill", "Skill", 0...1, 0.7, "%.2f")
                    slider(c, "curiosity", "Curiosity", 0...1, 0.3, "%.2f")
                    slider(c, "patience", "Patience", 2...60, 15, "%.0f s")
                } header: {
                    Text("Playtester persona")
                } footer: {
                    Text("Used by playtest_run when this agent plays (and by loop playtest stages run as this agent).")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }

            Section("Memory") {
                if agent.memory.isEmpty {
                    Text("No notes yet. \(agent.name) can keep notes with studio_memory.").foregroundStyle(.secondary)
                }
                ForEach(Array(agent.memory.enumerated()), id: \.offset) { k, note in
                    HStack(alignment: .top) {
                        Text("\(k + 1).").foregroundStyle(.secondary).monospacedDigit()
                        Text(note).textSelection(.enabled)
                        Spacer()
                        Button { c.wrappedValue.memory.remove(at: k) } label: { Image(systemName: "minus.circle") }
                            .buttonStyle(.borderless)
                    }
                }
                HStack {
                    TextField("Add a note", text: $newNote).onSubmit { addNote(c) }
                    Button("Add") { addNote(c) }.disabled(newNote.isEmpty)
                }
            }

            Section {
                let u = crew.usage(for: agent)
                LabeledContent("Requests", value: "\(u.requests)")
                LabeledContent("Input tokens", value: u.input.formatted())
                LabeledContent("Cached input tokens", value: u.cacheRead.formatted())
                LabeledContent("Output tokens", value: u.output.formatted())
                LabeledContent("Tool calls", value: u.toolCalls.formatted())
                LabeledContent("Estimated cost", value: String(format: "$%.2f", u.costUSD))
            } header: {
                Text("Usage")
            } footer: {
                Text("Saved to agents/\(agent.id).agent.json in the project — commit it to share \(agent.name) with your team "
                    + "(never API keys). The CLI runner and external agents use the same definition.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }

    private var disciplines: [String] {
        var out: [String] = []
        for r in CrewRole.all where !out.contains(r.discipline) { out.append(r.discipline) }
        return out
    }

    private func knob(_ c: Binding<Cloudling>, _ key: String, _ fallback: String) -> Binding<String> {
        Binding(get: { c.wrappedValue.playtest[key].string ?? fallback }, set: { v in
            var p = c.wrappedValue.playtest.isNull ? JSON.object([]) : c.wrappedValue.playtest
            p.set(key, .string(v))
            c.wrappedValue.playtest = p
        })
    }

    private func slider(_ c: Binding<Cloudling>, _ key: String, _ title: String, _ range: ClosedRange<Double>, _ fallback: Double,
                        _ format: String) -> some View {
        let value = Binding<Double>(get: { c.wrappedValue.playtest[key].number ?? fallback }, set: { v in
            var p = c.wrappedValue.playtest.isNull ? JSON.object([]) : c.wrappedValue.playtest
            p.set(key, .number((v * 100).rounded() / 100))
            c.wrappedValue.playtest = p
        })
        return HStack {
            Text(title).frame(width: 110, alignment: .leading)
            Slider(value: value, in: range)
            Text(String(format: format, value.wrappedValue)).font(Theme.monoSmall).frame(width: 52, alignment: .trailing)
        }
    }

    private func enabledToolCount(_ c: Cloudling) -> Int {
        crew.engineTools().filter { c.access(category: $0.category, readOnly: $0.readOnly) != .off }.count
    }

    private func addNote(_ c: Binding<Cloudling>) {
        let note = newNote.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !note.isEmpty else { return }
        c.wrappedValue.memory.append(note)
        newNote = ""
    }
}

/// Studio roster for Settings: add, remove, spawn teams, and design each member.
struct CrewManager: View {
    @Environment(CrewStore.self) private var crew
    @State var selection: String?

    var body: some View {
        HSplitView {
            List(selection: $selection) {
                Section("Studio roster (agents/*.agent.json)") {
                    ForEach(crew.cloudlings) { c in
                        HStack {
                            CloudAvatar(color: c.color, face: c.face, size: 20, working: crew.isWorking(c))
                            VStack(alignment: .leading) {
                                Text(c.name)
                                Text(c.focus.isEmpty ? c.role.title : "\(c.role.title) · \(c.focus)").font(.caption)
                                    .foregroundStyle(.secondary).lineLimit(1)
                            }
                        }
                        .tag(c.id)
                    }
                }
            }
            .frame(minWidth: 200, maxWidth: 260)
            .safeAreaInset(edge: .bottom) {
                HStack {
                    AddAgentMenu { id in selection = id }
                    Button("Remove", systemImage: "minus") {
                        if let c = crew.cloudlings.first(where: { $0.id == selection }) { crew.remove(c) }
                        selection = nil
                    }
                    .labelStyle(.iconOnly).buttonStyle(.borderless)
                    .disabled(selection == nil)
                    Spacer()
                }
                .padding(6)
            }
            if let selection {
                AgentDesigner(cloudlingID: selection).id(selection)
            } else {
                ContentUnavailableView("Select an agent", systemImage: "cloud",
                                       description: Text("Design roles, focus, instructions, permissions and memory."))
            }
        }
        .onAppear { selection = selection ?? crew.cloudlings.first?.id }
    }
}

/// "+" menu: add one agent by role, or spawn a whole team from a template.
struct AddAgentMenu: View {
    @Environment(CrewStore.self) private var crew
    var onAdd: (String) -> Void = { _ in }

    var body: some View {
        Menu {
            Menu("Spawn Team") {
                ForEach(crew.studio.teamTemplates, id: \.name) { t in
                    Button(t.name.replacingOccurrences(of: "_", with: " ").capitalized) { crew.spawnTeam(t.name) }
                        .help(t.description)
                }
            }
            Divider()
            ForEach(disciplines, id: \.self) { d in
                Menu(d.capitalized) {
                    ForEach(CrewRole.all.filter { $0.discipline == d }) { role in
                        Button(role.title, systemImage: role.symbol) {
                            if let id = crew.add(role: role) { onAdd(id) }
                        }
                    }
                }
            }
        } label: { Image(systemName: "plus") }
        .menuStyle(.borderlessButton).fixedSize()
        .help("Add an agent or spawn a team")
    }

    private var disciplines: [String] {
        var out: [String] = []
        for r in CrewRole.all where !out.contains(r.discipline) { out.append(r.discipline) }
        return out
    }
}
