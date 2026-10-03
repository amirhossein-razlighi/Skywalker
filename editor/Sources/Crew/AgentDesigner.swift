import SwiftUI

/// Full editor for one Cloudling: identity, role & instructions, brain, per-category tool
/// permissions, long-term memory, token usage, and sharing as a project asset.
struct AgentDesigner: View {
    @Environment(CrewStore.self) private var crew
    @Environment(EngineStore.self) private var engine
    let cloudlingID: UUID
    @State private var newNote = ""
    @State private var status = ""

    var body: some View {
        @Bindable var crew = crew
        if let i = crew.cloudlings.firstIndex(where: { $0.id == cloudlingID }) {
            let c = crew.cloudlings[i]
            Form {
                Section {
                    HStack(spacing: 14) {
                        CloudAvatar(color: c.color, face: c.face, size: 56, working: crew.isWorking(c))
                        VStack(alignment: .leading, spacing: 6) {
                            TextField("Name", text: $crew.cloudlings[i].name).font(.title3.weight(.semibold))
                                .textFieldStyle(.plain)
                            HStack {
                                Picker("Face", selection: $crew.cloudlings[i].face) {
                                    ForEach(CloudFace.allCases, id: \.self) { Text($0.rawValue.capitalized).tag($0) }
                                }
                                .labelsHidden().fixedSize()
                                ColorPicker("Color", selection: Binding(get: { crew.cloudlings[i].color },
                                                                        set: { crew.cloudlings[i].colorHex = $0.hexString }))
                                    .labelsHidden()
                            }
                        }
                    }
                    .padding(.vertical, 4)
                }

                Section("Role") {
                    Picker("Role", selection: $crew.cloudlings[i].roleID) {
                        ForEach(CrewRole.all) { Label($0.title, systemImage: $0.symbol).tag($0.id) }
                    }
                    TextField("Mission", text: Binding(
                        get: { crew.cloudlings[i].missionOverride.isEmpty ? crew.cloudlings[i].role.mission : crew.cloudlings[i].missionOverride },
                        set: { crew.cloudlings[i].missionOverride = $0 == crew.cloudlings[i].role.mission ? "" : $0 }),
                              axis: .vertical)
                        .lineLimit(2...6)
                    if !c.missionOverride.isEmpty {
                        Button("Reset to the role's mission") { crew.cloudlings[i].missionOverride = "" }
                            .buttonStyle(.link).font(.caption)
                    }
                    TextField("Personality", text: $crew.cloudlings[i].personality, axis: .vertical)
                    TextField("Standing instructions (style guides, constraints, conventions…)",
                              text: $crew.cloudlings[i].instructions, axis: .vertical)
                        .lineLimit(3...10)
                }

                Section("Brain") {
                    Picker("Provider", selection: $crew.cloudlings[i].providerID) {
                        ForEach(crew.providers) { Text($0.name).tag(Optional($0.id)) }
                    }
                    TextField("Model", text: $crew.cloudlings[i].model,
                              prompt: Text(crew.providers.first { $0.id == c.providerID }?.defaultModel ?? "provider default"))
                    Stepper("Max tool rounds per message: \(c.maxRounds)", value: $crew.cloudlings[i].maxRounds, in: 5...200, step: 5)
                }

                Section {
                    Picker("Autonomy", selection: $crew.cloudlings[i].autonomy) {
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
                                get: { crew.cloudlings[i].toolAccess[cat.id] ?? .inherit },
                                set: { crew.cloudlings[i].toolAccess[cat.id] = $0 == .inherit ? nil : $0 })) {
                                ForEach(ToolAccess.allCases) { Text($0.label).tag($0) }
                            }
                            .labelsHidden().pickerStyle(.segmented).frame(width: 230)
                        }
                    }
                } header: {
                    Text("Permissions")
                } footer: {
                    Text("Default follows autonomy. Ask = mutating tools in that group wait for your approval; Off removes the group. "
                        + "\(enabledToolCount(c)) of \(crew.engineTools().count) engine tools available.")
                        .font(.caption).foregroundStyle(.secondary)
                }

                Section {
                    if c.memory.isEmpty {
                        Text("No notes yet. \(c.name) can save notes with memory_note.").foregroundStyle(.secondary)
                    }
                    ForEach(Array(c.memory.enumerated()), id: \.offset) { k, note in
                        HStack(alignment: .top) {
                            Text("\(k + 1).").foregroundStyle(.secondary).monospacedDigit()
                            Text(note).textSelection(.enabled)
                            Spacer()
                            Button { crew.cloudlings[i].memory.remove(at: k) } label: { Image(systemName: "minus.circle") }
                                .buttonStyle(.borderless)
                        }
                    }
                    HStack {
                        TextField("Add a note", text: $newNote).onSubmit(addNote)
                        Button("Add", action: addNote).disabled(newNote.isEmpty)
                    }
                } header: {
                    Text("Memory")
                }

                Section("Usage") {
                    let u = crew.usage(for: c)
                    LabeledContent("Requests", value: "\(u.requests)")
                    LabeledContent("Input tokens", value: u.input.formatted())
                    LabeledContent("Cached input tokens", value: u.cacheRead.formatted())
                    LabeledContent("Output tokens", value: u.output.formatted())
                    Button("Reset counters") { crew.resetUsage(c) }.disabled(u.requests == 0)
                }

                Section {
                    HStack {
                        Button("Save to Project", systemImage: "square.and.arrow.down") {
                            do {
                                let url = try crew.exportAgent(crew.cloudlings[i], to: engine.projectDirectory)
                                status = "Saved \(url.lastPathComponent) — commit it to share \(c.name) with your team."
                            } catch { status = error.localizedDescription }
                        }
                        Spacer()
                        Text(status).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                    }
                } header: {
                    Text("Share")
                } footer: {
                    Text("Agents saved to the project (agents/*.agent.json) carry role, instructions, permissions and memory — never API keys.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            .formStyle(.grouped)
            .onChange(of: crew.cloudlings[i]) { _, new in
                crew.applyDefinitionChanges(new)
                crew.save()
            }
        } else {
            ContentUnavailableView("Select a Cloudling", systemImage: "cloud")
        }
    }

    private func enabledToolCount(_ c: Cloudling) -> Int {
        crew.engineTools().filter { c.access(category: $0.category, readOnly: $0.readOnly) != .off }.count
    }

    private func addNote() {
        let note = newNote.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !note.isEmpty, let i = crew.cloudlings.firstIndex(where: { $0.id == cloudlingID }) else { return }
        crew.cloudlings[i].memory.append(note)
        newNote = ""
    }
}

/// Crew roster for Settings: add, remove, import from the project, and design each member.
struct CrewManager: View {
    @Environment(CrewStore.self) private var crew
    @Environment(EngineStore.self) private var engine
    @State var selection: UUID?

    var body: some View {
        HSplitView {
            List(selection: $selection) {
                Section("Crew") {
                    ForEach(crew.cloudlings) { c in
                        HStack {
                            CloudAvatar(color: c.color, face: c.face, size: 20, working: crew.isWorking(c))
                            VStack(alignment: .leading) {
                                Text(c.name)
                                Text(c.role.title).font(.caption).foregroundStyle(.secondary)
                            }
                        }
                        .tag(c.id)
                    }
                }
                let files = crew.agentFiles(in: engine.projectDirectory)
                if !files.isEmpty {
                    Section("In this project") {
                        ForEach(files, id: \.self) { url in
                            HStack {
                                Image(systemName: "doc.text").foregroundStyle(.secondary)
                                Text(url.lastPathComponent.replacingOccurrences(of: ".agent.json", with: ""))
                                Spacer()
                                Button("Load") {
                                    if let c = try? crew.importAgent(from: url) { selection = c.id }
                                }
                                .buttonStyle(.borderless).font(.caption)
                            }
                        }
                    }
                }
            }
            .frame(minWidth: 190, maxWidth: 240)
            .safeAreaInset(edge: .bottom) {
                HStack {
                    Menu {
                        ForEach(CrewRole.all) { role in
                            Button(role.title, systemImage: role.symbol) { add(role: role) }
                        }
                    } label: { Image(systemName: "plus") }
                    .menuStyle(.borderlessButton).fixedSize()
                    Button("Remove", systemImage: "minus") {
                        crew.cloudlings.removeAll { $0.id == selection }
                        selection = nil
                        crew.save()
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
                ContentUnavailableView("Select a Cloudling", systemImage: "cloud",
                                       description: Text("Design roles, instructions, permissions and memory."))
            }
        }
        .onAppear { selection = selection ?? crew.cloudlings.first?.id }
    }

    private func add(role: CrewRole) {
        let palette = ["#7fb3ff", "#8b73fa", "#54ccad", "#ffb873", "#ed6b7a", "#c4a1ff", "#6fd3e8"]
        let c = Cloudling(name: "Puff \(crew.cloudlings.count + 1)", roleID: role.id,
                          colorHex: palette[crew.cloudlings.count % palette.count],
                          face: CloudFace.allCases[crew.cloudlings.count % CloudFace.allCases.count],
                          providerID: crew.providers.first?.id)
        crew.cloudlings.append(c)
        selection = c.id
        crew.save()
    }
}
