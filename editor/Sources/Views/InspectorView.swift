import SwiftUI

/// Inspector for the selected entity (or the environment when nothing is selected).
/// Field editors are generated from the engine's reflected JSON schema — the very same
/// schema agents receive — so humans and agents always edit the same things.
struct InspectorView: View {
    @Environment(EngineStore.self) private var engine
    @State private var doc: JSON = .null
    @State private var schemas: JSON = .null

    private var selectedID: UInt64? { engine.selection.count == 1 ? engine.selection.first : nil }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                if let id = selectedID, !doc.isNull {
                    header(id: id)
                    ForEach(componentNames, id: \.self) { comp in
                        ComponentSection(name: comp, schema: schemas[comp], values: doc["components"][comp]) { patch in
                            update(id, ["components": [(comp, patch)].reduce(into: JSON.object([])) { $0.set($1.0, $1.1) }])
                        } onRemove: {
                            update(id, ["components": .object([(comp, .null)])])
                        }
                    }
                    addComponentMenu(id: id)
                    BehaviorsSection(entityID: id, entityName: doc["name"].string ?? "", behaviors: doc["behaviors"].array)
                } else if engine.selection.count > 1 {
                    ContentUnavailableView("\(engine.selection.count) entities selected", systemImage: "square.stack.3d.up",
                                           description: Text("Ask a Cloudling to edit them together."))
                } else {
                    EnvironmentSection(schema: schemas["environment"])
                }
            }
            .padding(12)
        }
        .task { schemas = engine.call("component_schema", [:], actor: "editor").structured }
        .task(id: "\(selectedID ?? 0)-\(engine.revision)") { reload() }
    }

    private var componentNames: [String] {
        ["transform", "mesh", "light", "camera"].filter { !doc["components"][$0].isNull }
    }

    private func reload() {
        guard let id = selectedID else { doc = .null; return }
        doc = engine.entity(id)
    }

    private func update(_ id: UInt64, _ patch: JSON) {
        var args = patch
        args.set("entity", .number(Double(id)))
        engine.call("entity_update", args)
        reload()
    }

    @ViewBuilder
    private func header(id: UInt64) -> some View {
        HStack {
            TextField("Name", text: Binding(get: { doc["name"].string ?? "" },
                                            set: { update(id, ["name": .string($0)]) }))
                .font(.title3.weight(.semibold))
                .textFieldStyle(.plain)
            Spacer()
            Text("#\(id)").font(Theme.monoSmall).foregroundStyle(.secondary)
            Toggle("", isOn: Binding(get: { doc["enabled"].bool ?? true },
                                     set: { update(id, ["enabled": .bool($0)]) }))
                .toggleStyle(.switch).controlSize(.mini).labelsHidden()
                .help("Enabled")
        }
        TextField("tags (comma separated)", text: Binding(
            get: { doc["tags"].array.compactMap(\.string).joined(separator: ", ") },
            set: { text in
                let tags = text.split(separator: ",").map { JSON.string($0.trimmingCharacters(in: .whitespaces)) }
                update(id, ["tags": .array(tags.filter { $0.string?.isEmpty == false })])
            }))
            .font(.caption)
    }

    private func addComponentMenu(id: UInt64) -> some View {
        Menu("Add Component") {
            ForEach(["mesh", "light", "camera"].filter { doc["components"][$0].isNull }, id: \.self) { comp in
                Button(comp.capitalized) { update(id, ["components": .object([(comp, [:])])]) }
            }
        }
        .controlSize(.small)
        .fixedSize()
    }
}

/// A reflected component editor.
struct ComponentSection: View {
    let name: String
    let schema: JSON
    let values: JSON
    let onChange: (JSON) -> Void
    var onRemove: (() -> Void)? = nil

    var body: some View {
        DisclosureGroup {
            VStack(alignment: .leading, spacing: 6) {
                ForEach(schema["properties"].members.map(\.0), id: \.self) { field in
                    FieldEditor(field: field, schema: schema["properties"][field], value: values[field]) { newValue in
                        onChange(.object([(field, newValue)]))
                    }
                }
            }
            .padding(.top, 4)
        } label: {
            HStack {
                Text(name.capitalized).font(.headline)
                Spacer()
                if let onRemove, name != "transform" {
                    Button(role: .destructive, action: onRemove) { Image(systemName: "minus.circle") }
                        .buttonStyle(.borderless).help("Remove \(name)")
                }
            }
        }
        .card()
    }
}

/// Edits one reflected field based on its JSON-schema type.
struct FieldEditor: View {
    let field: String
    let schema: JSON
    let value: JSON
    let onChange: (JSON) -> Void

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(field).font(.caption).foregroundStyle(.secondary).frame(width: 92, alignment: .leading)
                .help(schema["description"].string ?? "")
            editor
        }
    }

    @ViewBuilder
    private var editor: some View {
        if !schema["enum"].isNull {
            Picker("", selection: Binding(get: { value.string ?? "" }, set: { onChange(.string($0)) })) {
                ForEach(schema["enum"].array.compactMap(\.string), id: \.self) { Text($0).tag($0) }
            }
            .labelsHidden()
        } else if schema["type"].string == "boolean" {
            Toggle("", isOn: Binding(get: { value.bool ?? false }, set: { onChange(.bool($0)) })).labelsHidden()
        } else if schema["type"].string == "number" {
            NumberField(value: value.number ?? 0, onCommit: { onChange(.number($0)) })
        } else if schema["type"].string == "array" {
            HStack(spacing: 4) {
                ForEach(0..<3, id: \.self) { i in
                    NumberField(value: value[i].number ?? 0, onCommit: { v in
                        var arr = value.array
                        while arr.count < 3 { arr.append(0) }
                        arr[i] = .number(v)
                        onChange(.array(arr))
                    })
                }
            }
        } else if schema["type"].isNull {  // color
            ColorPicker("", selection: Binding(
                get: { Color(hex: value.string ?? "#ffffff") },
                set: { onChange(.string($0.hexString)) }))
                .labelsHidden()
            Text(value.string ?? "").font(Theme.monoSmall).foregroundStyle(.secondary)
        } else {
            CommitTextField(text: value.string ?? "", onCommit: { onChange(.string($0)) })
        }
    }
}

/// A number field that commits on Return / focus loss (one undo step per edit).
struct NumberField: View {
    let value: Double
    let onCommit: (Double) -> Void
    @State private var text = ""
    @FocusState private var focused: Bool

    var body: some View {
        TextField("", text: $text)
            .font(Theme.monoSmall)
            .multilineTextAlignment(.trailing)
            .focused($focused)
            .onAppear { text = Self.format(value) }
            .onChange(of: value) { _, v in if !focused { text = Self.format(v) } }
            .onSubmit(commit)
            .onChange(of: focused) { _, f in if !f { commit() } }
    }

    private func commit() {
        if let v = Double(text), v != value { onCommit(v) } else { text = Self.format(value) }
    }

    static func format(_ v: Double) -> String {
        v.rounded() == v ? String(Int(v)) : String(format: "%.3g", v)
    }
}

struct CommitTextField: View {
    let text: String
    let onCommit: (String) -> Void
    @State private var draft = ""

    var body: some View {
        TextField("", text: $draft)
            .onAppear { draft = text }
            .onChange(of: text) { _, t in draft = t }
            .onSubmit { if draft != text { onCommit(draft) } }
    }
}

/// Environment / lighting editor shown when nothing is selected.
struct EnvironmentSection: View {
    @Environment(EngineStore.self) private var engine
    let schema: JSON
    @State private var env: JSON = .null

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Label("Environment & Lighting", systemImage: "sun.horizon.fill").font(.headline)
            HStack {
                ForEach(["noon", "sunset", "night", "overcast", "studio"], id: \.self) { preset in
                    Button(preset.capitalized) { apply(["preset": .string(preset)]) }
                        .controlSize(.small)
                }
            }
            if !schema.isNull, !env.isNull {
                ComponentSection(name: "environment", schema: schema, values: env) { patch in apply(patch) }
            }
            Text("Select an entity to edit it, or ask your crew.").font(.caption).foregroundStyle(.secondary)
        }
        .task(id: engine.revision) { env = engine.call("environment_update", [:], actor: "editor").structured }
    }

    private func apply(_ patch: JSON) {
        env = engine.call("environment_update", patch).structured
    }
}

extension Color {
    var hexString: String {
        let c = NSColor(self).usingColorSpace(.sRGB) ?? .white
        return String(format: "#%02x%02x%02x", Int(c.redComponent * 255), Int(c.greenComponent * 255), Int(c.blueComponent * 255))
    }
}
