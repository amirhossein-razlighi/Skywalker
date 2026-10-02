import SwiftUI

/// The ECPS editor: each behavior is an *intent* (plain language, for people and agents)
/// woven into *Wander* code (deterministic, for the engine). Edit either side; "Weave"
/// asks the crew's gameplay programmer to (re)write the code from the intent.
struct BehaviorsSection: View {
    @Environment(EngineStore.self) private var engine
    let entityID: UInt64
    let entityName: String
    let behaviors: [JSON]

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Label("Behaviors", systemImage: "wand.and.stars").font(.headline)
                Spacer()
                Button {
                    let name = "Behavior \(behaviors.count + 1)"
                    engine.call("behavior_set", ["entity": .number(Double(entityID)), "name": .string(name),
                                                 "intent": "", "source": ""])
                } label: { Image(systemName: "plus") }
                .buttonStyle(.borderless)
                .help("Add behavior")
            }
            if behaviors.isEmpty {
                Text("Describe what this entity should do — your crew can weave it into Wander.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            ForEach(behaviors.indices, id: \.self) { i in
                BehaviorCard(entityID: entityID, entityName: entityName, behavior: behaviors[i])
            }
        }
        .card()
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
    @State private var showCode = true

    private var name: String { behavior["name"].string ?? "Behavior" }
    private var dirty: Bool { intent != (behavior["intent"].string ?? "") || source != (behavior["source"].string ?? "") }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(name).font(.subheadline.weight(.semibold))
                Spacer()
                Toggle("", isOn: Binding(get: { behavior["enabled"].bool ?? true }, set: { save(enabled: $0) }))
                    .toggleStyle(.switch).controlSize(.mini).labelsHidden()
                Button(role: .destructive) {
                    engine.call("behavior_remove", ["entity": .number(Double(entityID)), "name": .string(name)])
                } label: { Image(systemName: "trash") }
                .buttonStyle(.borderless)
            }

            Text("Intent").font(.caption).foregroundStyle(.secondary)
            TextEditor(text: $intent)
                .font(.callout)
                .frame(minHeight: 44)
                .scrollContentBackground(.hidden)
                .padding(6)
                .background(Theme.sky.opacity(0.08), in: RoundedRectangle(cornerRadius: 8))

            HStack {
                Button {
                    weaving = true
                    Task {
                        if dirty { save() }
                        await crew.weave(entity: entityID, entityName: entityName, behavior: name, intent: intent)
                        weaving = false
                    }
                } label: {
                    Label(weaving ? "Weaving…" : "Weave with crew", systemImage: "sparkles")
                }
                .disabled(weaving || intent.trimmingCharacters(in: .whitespaces).isEmpty)
                .buttonStyle(.borderedProminent)
                .tint(Theme.violet)
                .controlSize(.small)
                Spacer()
                Button(showCode ? "Hide Wander" : "Show Wander") { showCode.toggle() }
                    .buttonStyle(.borderless).controlSize(.small)
            }

            if showCode {
                TextEditor(text: $source)
                    .font(Theme.monoSmall)
                    .frame(minHeight: 120)
                    .scrollContentBackground(.hidden)
                    .padding(6)
                    .background(.black.opacity(0.06), in: RoundedRectangle(cornerRadius: 8))
                ForEach(diagnostics.indices, id: \.self) { i in
                    let d = diagnostics[i]
                    Label {
                        VStack(alignment: .leading, spacing: 2) {
                            Text("\(d["line"].int ?? 0):\(d["column"].int ?? 0)  \(d["message"].string ?? "")")
                            if let hint = d["hint"].string { Text(hint).foregroundStyle(.secondary) }
                        }
                    } icon: {
                        Image(systemName: d["severity"].string == "error" ? "xmark.octagon.fill" : "exclamationmark.triangle.fill")
                            .foregroundStyle(d["severity"].string == "error" ? Theme.rose : Theme.dawn)
                    }
                    .font(.caption)
                }
            }

            if dirty {
                HStack {
                    Spacer()
                    Button("Revert") { syncFromEngine() }.controlSize(.small)
                    Button("Save") { save() }.controlSize(.small).keyboardShortcut(.return, modifiers: .command)
                }
            }
        }
        .padding(8)
        .background(.background, in: RoundedRectangle(cornerRadius: 8))
        .onAppear(perform: syncFromEngine)
        .onChange(of: behavior) { _, _ in syncFromEngine() }
        .task(id: source) {
            try? await Task.sleep(for: .milliseconds(350))  // debounce live checking
            guard !Task.isCancelled else { return }
            diagnostics = engine.call("wander_check", ["source": .string(source)], actor: "editor").structured["diagnostics"].array
        }
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
