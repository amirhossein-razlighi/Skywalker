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
    @State private var view: CardView = .code
    @State private var params: [JSON] = []

    enum CardView: String, CaseIterable, Identifiable {
        case code = "Code", graph = "Graph"
        var id: String { rawValue }
    }

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

            if !params.isEmpty {
                BehaviorParamsView(entityID: entityID, params: params)
            }

            HStack(spacing: 6) {
                Picker("", selection: $view) {
                    ForEach(CardView.allCases) { v in Text(v.rawValue).tag(v) }
                }
                .pickerStyle(.segmented).labelsHidden().controlSize(.small).fixedSize()
                .help("Edit the behavior as Wander code or as a node graph (both are the same behavior)")
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
            if view == .code {
                CodeEditor(text: $source, errorLines: errorLines)
                    .frame(minHeight: 120, idealHeight: 180, maxHeight: 320)
                    .background(Theme.field, in: RoundedRectangle(cornerRadius: 4))
                    .overlay(RoundedRectangle(cornerRadius: 4).stroke(Theme.border))
            } else {
                BehaviorGraphView(entityID: entityID, behaviorName: name, source: behavior["source"].string ?? "")
                    .frame(height: 340)
                    .clipShape(RoundedRectangle(cornerRadius: 4))
                    .overlay(RoundedRectangle(cornerRadius: 4).stroke(Theme.border))
                    .disabled(dirty)
                    .overlay {
                        if dirty {
                            Text("Save or revert the code to edit the graph").font(Theme.label).foregroundStyle(Theme.textDim)
                                .padding(6).background(Theme.panelRaised, in: RoundedRectangle(cornerRadius: 4))
                        }
                    }
            }

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
        .onAppear {
            syncFromEngine()
            loadParams()
        }
        .onChange(of: behavior) { _, _ in
            if !dirty { syncFromEngine() }
            loadParams()
        }
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

    private func loadParams() {
        let r = engine.call("behavior_spec", ["entity": .number(Double(entityID)), "name": .string(name)], actor: "editor")
        params = r.isError ? [] : r.structured["derived"]["params"].array
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
                Text("\(Text("Ln \(diagnostic["line"].int ?? 0), Col \(diagnostic["column"].int ?? 0)").foregroundStyle(Theme.textFaint))  \(diagnostic["message"].string ?? "")")
                if let hint = diagnostic["hint"].string { Text(hint).foregroundStyle(Theme.textDim) }
            }
        }
        .font(Theme.label)
        .textSelection(.enabled)
    }
}

/// Tunable `param`s of a behavior (declared in code with an optional range) as sliders.
/// Values are per entity: they are written to the entity's vars, which the behavior reads.
struct BehaviorParamsView: View {
    @Environment(EngineStore.self) private var engine
    let entityID: UInt64
    let params: [JSON]
    @State private var values: [String: Double] = [:]

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Label("Parameters", systemImage: "slider.horizontal.3").font(Theme.caps).foregroundStyle(Theme.textDim)
            ForEach(params.indices, id: \.self) { i in row(params[i]) }
        }
    }

    @ViewBuilder private func row(_ p: JSON) -> some View {
        let name = p["name"].string ?? ""
        HStack(spacing: 6) {
            Text(name).font(Theme.label).frame(width: 84, alignment: .leading).lineLimit(1)
                .help(p["doc"].string ?? name)
            if let current = p["value"].number ?? p["default"].number {
                let lo = p["min"].number ?? min(0, current)
                let hi = p["max"].number ?? max(1, current * 2)
                Slider(value: Binding(get: { values[name] ?? current },
                                      set: { values[name] = $0 }),
                       in: lo...max(hi, lo + 0.0001)) { editing in
                    if !editing, let v = values[name] { write(name, .number((v * 1000).rounded() / 1000)) }
                }
                .controlSize(.mini)
                Text(String(format: "%.3g", values[name] ?? current)).font(Theme.monoSmall).foregroundStyle(Theme.textDim)
                    .frame(width: 42, alignment: .trailing)
            } else {
                Text((p["value"].isNull ? p["default"] : p["value"]).serialized()).font(Theme.monoSmall)
                    .foregroundStyle(Theme.textDim).lineLimit(1)
                Spacer()
            }
        }
    }

    private func write(_ name: String, _ value: JSON) {
        engine.call("entity_update", ["entity": .number(Double(entityID)), "vars": .object([(name, value)])])
    }
}
