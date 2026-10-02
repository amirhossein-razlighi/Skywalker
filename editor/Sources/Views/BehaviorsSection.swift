import SwiftUI

/// ECPS (Entity-Component-Prompt System) editor: each behavior is an *intent* in plain
/// language plus its *Wander* code. Edit either side; "Weave" asks the crew's gameplay
/// programmer to write or update the code from the intent and verify it in simulation.
struct BehaviorsSection: View {
    @Environment(EngineStore.self) private var engine
    let entityID: UInt64
    let entityName: String
    let behaviors: [JSON]

    var body: some View {
        PropertySection(title: "Behaviors", icon: "sparkles") {
            VStack(alignment: .leading, spacing: 8) {
                if behaviors.isEmpty {
                    Text("Describe what this entity should do. Your crew weaves the intent into deterministic Wander code.")
                        .font(Theme.label).foregroundStyle(Theme.textFaint)
                }
                ForEach(behaviors.indices, id: \.self) { i in
                    BehaviorCard(entityID: entityID, entityName: entityName, behavior: behaviors[i])
                }
                Button {
                    var n = behaviors.count + 1
                    let names = Set(behaviors.compactMap { $0["name"].string })
                    while names.contains("Behavior \(n)") { n += 1 }
                    engine.call("behavior_set", ["entity": .number(Double(entityID)), "name": .string("Behavior \(n)"),
                                                 "intent": "", "source": ""])
                } label: {
                    Label("Add Behavior", systemImage: "plus").font(Theme.label)
                }
                .buttonStyle(.borderless)
            }
        }
    }
}

struct BehaviorCard: View {
    @Environment(EngineStore.self) private var engine
    @Environment(CrewStore.self) private var crew
    let entityID: UInt64
    let entityName: String
    let behavior: JSON

    @State private var intent = ""
    @State private var source = ""
    @State private var diagnostics: [JSON] = []
    @State private var weaving = false

    private var name: String { behavior["name"].string ?? "Behavior" }
    private var dirty: Bool { intent != (behavior["intent"].string ?? "") || source != (behavior["source"].string ?? "") }
    private var errorLines: Set<Int> {
        Set(diagnostics.filter { $0["severity"].string == "error" }.compactMap { $0["line"].int })
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 6) {
                Toggle("", isOn: Binding(get: { behavior["enabled"].bool ?? true }, set: { save(enabled: $0) }))
                    .toggleStyle(.checkbox).labelsHidden()
                Text(name).font(Theme.sectionTitle)
                statusBadge
                Spacer()
                Menu {
                    Button("Remove Behavior", role: .destructive) {
                        engine.call("behavior_remove", ["entity": .number(Double(entityID)), "name": .string(name)])
                    }
                } label: { Image(systemName: "ellipsis").font(.system(size: 10)) }
                .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
            }

            Label("Intent", systemImage: "text.bubble").font(Theme.caps).foregroundStyle(Theme.textDim)
            TextEditor(text: $intent)
                .font(Theme.body)
                .scrollContentBackground(.hidden)
                .frame(minHeight: 38, maxHeight: 80)
                .padding(4)
                .background(Theme.field, in: RoundedRectangle(cornerRadius: 4))
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(Theme.border))

            HStack(spacing: 6) {
                Label("Wander", systemImage: "chevron.left.forwardslash.chevron.right").font(Theme.caps).foregroundStyle(Theme.textDim)
                Spacer()
                Button {
                    weaving = true
                    Task {
                        if dirty { save() }
                        await crew.weave(entity: entityID, entityName: entityName, behavior: name, intent: intent)
                        weaving = false
                    }
                } label: {
                    Label(weaving ? "Weaving…" : "Weave", systemImage: "sparkles").font(Theme.label)
                }
                .buttonStyle(.bordered)
                .tint(Theme.ai)
                .controlSize(.small)
                .disabled(weaving || intent.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                .help("Ask the crew's gameplay programmer to write this behavior from the intent")
            }
            CodeEditor(text: $source, errorLines: errorLines)
                .frame(minHeight: 120, idealHeight: 180, maxHeight: 320)
                .background(Theme.field, in: RoundedRectangle(cornerRadius: 4))
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(Theme.border))

            ForEach(diagnostics.indices, id: \.self) { i in
                DiagnosticRow(diagnostic: diagnostics[i])
            }

            if dirty {
                HStack {
                    Text("Unsaved changes").font(Theme.label).foregroundStyle(Theme.warning)
                    Spacer()
                    Button("Revert") { syncFromEngine() }.controlSize(.small)
                    Button("Save") { save() }.controlSize(.small).buttonStyle(.borderedProminent)
                        .keyboardShortcut(.return, modifiers: .command)
                }
            }
        }
        .padding(8)
        .background(Theme.panelRaised, in: RoundedRectangle(cornerRadius: 6))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(Theme.border))
        .onAppear(perform: syncFromEngine)
        .onChange(of: behavior) { _, _ in if !dirty { syncFromEngine() } }
        .task(id: source) {
            try? await Task.sleep(for: .milliseconds(300))  // debounce live checking
            guard !Task.isCancelled else { return }
            diagnostics = engine.call("wander_check", ["source": .string(source)], actor: "editor").structured["diagnostics"].array
        }
    }

    @ViewBuilder private var statusBadge: some View {
        let errors = diagnostics.filter { $0["severity"].string == "error" }.count
        if source.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            badge("empty", Theme.textFaint)
        } else if errors > 0 {
            badge("\(errors) error\(errors == 1 ? "" : "s")", Theme.error)
        } else {
            badge("compiled", Theme.success)
        }
    }

    private func badge(_ text: String, _ color: Color) -> some View {
        Text(text).font(.system(size: 9.5, weight: .medium)).padding(.horizontal, 5).padding(.vertical, 1)
            .foregroundStyle(color).background(color.opacity(0.14), in: Capsule())
    }

    private func syncFromEngine() {
        intent = behavior["intent"].string ?? ""
        source = behavior["source"].string ?? ""
    }

    private func save(enabled: Bool? = nil) {
        var args: JSON = ["entity": .number(Double(entityID)), "name": .string(name),
                          "intent": .string(intent), "source": .string(source), "allow_errors": true]
        if let enabled { args.set("enabled", .bool(enabled)) }
        engine.call("behavior_set", args)
    }
}

struct DiagnosticRow: View {
    let diagnostic: JSON
    var body: some View {
        let isError = diagnostic["severity"].string == "error"
        HStack(alignment: .top, spacing: 6) {
            Image(systemName: isError ? "xmark.octagon.fill" : "exclamationmark.triangle.fill")
                .foregroundStyle(isError ? Theme.error : Theme.warning).font(.system(size: 10))
            VStack(alignment: .leading, spacing: 1) {
                Text("Ln \(diagnostic["line"].int ?? 0), Col \(diagnostic["column"].int ?? 0)  ").foregroundStyle(Theme.textFaint)
                    + Text(diagnostic["message"].string ?? "")
                if let hint = diagnostic["hint"].string { Text(hint).foregroundStyle(Theme.textDim) }
            }
        }
        .font(Theme.label)
        .textSelection(.enabled)
    }
}
