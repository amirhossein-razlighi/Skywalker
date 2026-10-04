import SwiftUI

enum RightTab: String, CaseIterable, Identifiable {
    case details = "Details", world = "World"
    var id: String { rawValue }
    var symbol: String { self == .details ? "slider.horizontal.3" : "globe.americas" }
}

struct DetailsPanel: View {
    @State private var tab: RightTab = .details
    var body: some View {
        VStack(spacing: 0) {
            PanelTabs(tabs: RightTab.allCases, selection: $tab, title: { $0.rawValue }, icon: { $0.symbol })
            ScrollView {
                switch tab {
                case .details: EntityDetails()
                case .world: WorldSettings()
                }
            }
            .scrollContentBackground(.hidden)
        }
        .background(Theme.panel)
    }
}

/// Reflection-driven entity editor. Field editors come from the engine's JSON schema —
/// the same schema agents receive — so humans and agents edit exactly the same data.
struct EntityDetails: View {
    @Environment(EngineStore.self) private var engine
    @State private var doc: JSON = .null
    @State private var schemas: JSON = .null

    private var selectedID: UInt64? { engine.selection.count == 1 ? engine.selection.first : nil }
    private static let componentOrder = ["transform", "mesh", "light", "reflection_probe", "camera", "particles", "groom",
                                         "water", "terrain", "foliage", "audio", "listener", "body", "collider", "character",
                                         "joint", "vehicle", "chase_camera", "nav_agent", "navmesh", "physics_world", "animator", "characterIk", "attach", "ik", "sequencer",
                                         "sprite", "sprite_anim", "tilemap", "light2d", "parallax", "camera2d", "text", "ui_canvas",
                                         "ui", "dialogue", "process", "particles2d", "persist", "carry"]
    /// Components offered by Add Component (transform is always present).
    private static let addable = ["mesh", "light", "reflection_probe", "camera", "particles", "groom", "water", "terrain",
                                  "foliage", "audio", "listener", "body", "collider", "character", "joint", "vehicle", "chase_camera",
                                  "nav_agent", "navmesh",
                                  "physics_world", "animator", "characterIk", "attach", "ik", "sequencer", "sprite", "sprite_anim",
                                  "tilemap",
                                  "light2d", "parallax", "camera2d", "text", "ui_canvas", "ui", "dialogue", "process", "particles2d",
                                  "persist", "carry"]

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            if let id = selectedID, !doc.isNull {
                header(id: id)
                if !doc["prefab"].isNull {
                    PrefabSection(entityID: id, prefab: doc["prefab"])
                }
                ForEach(Self.componentOrder.filter { !doc["components"][$0].isNull }, id: \.self) { comp in
                    PropertySection(title: title(comp), icon: icon(comp), removable: comp != "transform",
                                    onRemove: { update(id, ["components": .object([(comp, .null)])]) }) {
                        PropertyGrid(schema: schemas[comp], values: doc["components"][comp]) { field, value in
                            update(id, ["components": .object([(comp, .object([(field, value)]))])])
                        }
                        if comp == "animator" || comp == "sequencer" {
                            AnimationScrubBar(entityID: id, component: comp, values: doc["components"][comp]) { reload() }
                        }
                    }
                }
                addComponent(id: id)
                BehaviorsSection(entityID: id, entityName: doc["name"].string ?? "", behaviors: doc["behaviors"].array)
                    .id(id)
            } else {
                emptyState
            }
        }
        .task { schemas = engine.call("component_schema", [:], actor: "editor").structured }
        .task(id: "\(selectedID ?? 0)-\(engine.revision)") { reload() }
    }

    private var emptyState: some View {
        VStack(spacing: 6) {
            Image(systemName: engine.selection.count > 1 ? "square.stack.3d.up" : "cube.transparent")
                .font(.system(size: 22)).foregroundStyle(Theme.textFaint)
            Text(engine.selection.count > 1 ? "\(engine.selection.count) entities selected" : "Nothing selected")
                .font(Theme.body).foregroundStyle(Theme.textDim)
            Text(engine.selection.count > 1 ? "Multi-editing arrives in a later version — or ask an agent."
                                            : "Select an entity in the viewport or outliner.")
                .font(Theme.label).foregroundStyle(Theme.textFaint).multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity)
        .padding(.top, 60)
    }

    private func icon(_ comp: String) -> String {
        switch comp {
        case "transform": "move.3d"
        case "mesh": "cube"
        case "light": "lightbulb"
        case "reflection_probe": "circle.lefthalf.filled"
        case "camera": "video"
        case "particles": "flame"
        case "groom": "comb"
        case "water": "water.waves"
        case "audio": "speaker.wave.2"
        case "listener": "ear"
        case "body": "scalemass"
        case "collider": "square.dashed"
        case "character": "figure.walk"
        case "joint": "link"
        case "vehicle": "car"
        case "chase_camera": "camera.aperture"
        case "nav_agent": "arrow.triangle.turn.up.right.diamond"
        case "navmesh": "map"
        case "physics_world": "globe"
        case "terrain": "mountain.2"
        case "foliage": "leaf"
        case "sprite": "photo"
        case "sprite_anim": "film.stack"
        case "tilemap": "square.grid.3x3"
        case "light2d": "sun.max"
        case "parallax": "square.3.layers.3d"
        case "camera2d": "camera.viewfinder"
        case "text": "textformat"
        case "ui_canvas": "rectangle.on.rectangle"
        case "ui": "rectangle.and.hand.point.up.left"
        case "dialogue": "bubble.left.and.bubble.right"
        case "animator": "figure.run"
        case "attach": "paperclip"
        case "characterIk": "figure.stairs"
        case "ik": "hand.point.up.left"
        case "sequencer": "film.stack"
        case "process": "pause.circle"
        case "particles2d": "cloud.snow"
        case "persist": "square.and.arrow.down.on.square"
        case "carry": "suitcase"
        default: "puzzlepiece"
        }
    }

    private func title(_ comp: String) -> String {
        switch comp {
        case "body": "Rigid Body"
        case "nav_agent": "Nav Agent"
        case "navmesh": "NavMesh"
        case "physics_world": "Physics Settings"
        case "chase_camera": "Chase Camera"
        case "sprite_anim": "Sprite Animator"
        case "light2d": "Light 2D"
        case "camera2d": "Camera 2D"
        case "ui_canvas": "UI Canvas"
        case "ui": "UI Element"
        case "attach": "Bone Attachment"
        case "characterIk": "Character IK"
        case "ik": "IK Target"
        case "process": "Process (Pause / Time)"
        case "reflection_probe": "Reflection Probe"
        case "particles2d": "Particles 2D"
        case "persist": "Persist (Save Games)"
        case "carry": "Carry (Scene Changes)"
        default: comp.capitalized
        }
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
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Toggle("", isOn: Binding(get: { doc["enabled"].bool ?? true }, set: { update(id, ["enabled": .bool($0)]) }))
                    .toggleStyle(.checkbox).labelsHidden().help("Enabled")
                CommitField(text: doc["name"].string ?? "", font: .system(size: 13, weight: .semibold)) {
                    update(id, ["name": .string($0)])
                }
                Toggle("%", isOn: Binding(get: { doc["unique"].bool ?? false }, set: { update(id, ["unique": .bool($0)]) }))
                    .toggleStyle(.button).controlSize(.mini)
                    .help("Unique name: Wander find(\"%\(doc["name"].string ?? "Name")\") finds it inside its prefab instance")
                Text("#\(id)").font(Theme.monoSmall).foregroundStyle(Theme.textFaint)
            }
            HStack(spacing: 6) {
                Text("Tags").font(Theme.label).foregroundStyle(Theme.textDim).frame(width: PropertyGrid.labelWidth, alignment: .leading)
                CommitField(text: doc["tags"].array.compactMap(\.string).joined(separator: ", "), placeholder: "comma separated") { text in
                    let tags = text.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
                    update(id, ["tags": .array(tags.map(JSON.string))])
                }
            }
        }
        .padding(10)
        .overlay(alignment: .bottom) { Rectangle().fill(Theme.border).frame(height: 1) }
    }

    private var missingComponents: [String] { Self.addable.filter { doc["components"][$0].isNull } }

    /// `entity_update` arguments that add `component` with its defaults.
    private static func addPatch(_ component: String) -> JSON {
        let defaults: JSON = [:]
        return ["components": JSON.object([(component, defaults)])]
    }

    private func addComponent(id: UInt64) -> some View {
        Menu {
            ForEach(missingComponents, id: \.self) { comp in
                Button { update(id, Self.addPatch(comp)) } label: { Label(title(comp), systemImage: icon(comp)) }
            }
        } label: {
            Label("Add Component", systemImage: "plus").font(Theme.label)
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
        .padding(10)
    }
}

/// Collapsible section with a header bar (expanded by default).
struct PropertySection<Content: View>: View {
    let title: String
    let icon: String
    var removable = false
    var onRemove: () -> Void = {}
    @ViewBuilder var content: Content
    @State private var expanded = true

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 6) {
                Image(systemName: "chevron.right").font(.system(size: 8.5, weight: .bold))
                    .rotationEffect(.degrees(expanded ? 90 : 0)).foregroundStyle(Theme.textFaint).frame(width: 10)
                Image(systemName: icon).font(.system(size: 10.5)).foregroundStyle(Theme.textDim)
                Text(title).font(Theme.sectionTitle)
                Spacer()
                if removable {
                    Menu {
                        Button("Remove \(title)", role: .destructive, action: onRemove)
                    } label: { Image(systemName: "ellipsis").font(.system(size: 10)) }
                    .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
                }
            }
            .padding(.horizontal, 10)
            .frame(height: 26)
            .background(Theme.header)
            .contentShape(Rectangle())
            .onTapGesture { withAnimation(.easeOut(duration: 0.12)) { expanded.toggle() } }
            if expanded {
                content.padding(.horizontal, 10).padding(.vertical, 6)
            }
            Rectangle().fill(Theme.border).frame(height: 1)
        }
    }
}

/// Label/value rows generated from a component's JSON schema.
struct PropertyGrid: View {
    static let labelWidth: CGFloat = 86
    let schema: JSON
    let values: JSON
    let onChange: (String, JSON) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            ForEach(schema["properties"].members.map(\.0), id: \.self) { field in
                let fieldSchema = schema["properties"][field]
                HStack(alignment: .center, spacing: 6) {
                    Text(Self.prettify(field)).font(Theme.label).foregroundStyle(Theme.textDim)
                        .frame(width: Self.labelWidth, alignment: .leading)
                        .lineLimit(1)
                        .help(fieldSchema["description"].string ?? field)
                    if fieldSchema["x-sky-entity"].bool == true {
                        EntityField(value: values[field]) { onChange(field, $0) }
                    } else if fieldSchema["x-sky-entity-list"].bool == true {
                        EntityListField(value: values[field]) { onChange(field, $0) }
                    } else if let kind = AssetField.kind(for: field, schema: fieldSchema) {
                        AssetField(kind: kind, value: values[field].string ?? "") { onChange(field, .string($0)) }
                    } else {
                        FieldEditor(schema: fieldSchema, value: values[field]) { onChange(field, $0) }
                    }
                }
                .frame(minHeight: 20)
            }
        }
    }

    static func prettify(_ name: String) -> String {
        var out = ""
        for (i, ch) in name.enumerated() {
            if ch.isUppercase && i > 0 { out.append(" ") }
            out.append(i == 0 ? Character(ch.uppercased()) : ch)
        }
        return out
    }
}

struct FieldEditor: View {
    let schema: JSON
    let value: JSON
    let onChange: (JSON) -> Void

    var body: some View {
        if schema["x-sky-json"].bool == true {
            // Structured data (animation clips, tile layers, style overrides): edited as compact JSON.
            CommitField(text: value.serialized(), font: Theme.monoSmall) { text in
                if let parsed = JSON.parse(text) { onChange(parsed) }
            }
        } else if !schema["enum"].isNull {
            Picker("", selection: Binding(get: { value.string ?? "" }, set: { onChange(.string($0)) })) {
                ForEach(schema["enum"].array.compactMap(\.string), id: \.self) { Text($0.capitalized).tag($0) }
            }
            .labelsHidden().pickerStyle(.menu).controlSize(.small)
        } else if schema["type"].string == "boolean" {
            HStack {
                Toggle("", isOn: Binding(get: { value.bool ?? false }, set: { onChange(.bool($0)) }))
                    .toggleStyle(.checkbox).labelsHidden()
                Spacer()
            }
        } else if schema["type"].string == "number" {
            ScrubField(value: value.number ?? 0, step: step, range: range) { onChange(.number($0)) }
        } else if schema["type"].string == "array" {
            // vec2 / vec3 / vec4 (maxItems tells which)
            let count = max(2, min(4, Int(schema["maxItems"].number ?? 3)))
            HStack(spacing: 3) {
                ForEach(0..<count, id: \.self) { i in
                    ScrubField(value: value[i].number ?? 0, step: step, axis: i < 3 ? i : nil) { v in
                        var arr = value.array
                        while arr.count < count { arr.append(0) }
                        arr[i] = .number(v)
                        onChange(.array(arr))
                    }
                }
            }
        } else if schema["type"].isNull {  // color
            HStack(spacing: 6) {
                ColorPicker("", selection: Binding(get: { Color(hex: value.string ?? "#ffffff") }, set: { onChange(.string($0.hexString)) }),
                            supportsOpacity: false)
                    .labelsHidden().controlSize(.small)
                CommitField(text: value.string ?? "", font: Theme.monoSmall) { onChange(.string($0)) }
            }
        } else {
            CommitField(text: value.string ?? "") { onChange(.string($0)) }
        }
    }

    private var step: Double {
        let lo = schema["minimum"].number ?? -1e30, hi = schema["maximum"].number ?? 1e30
        return (hi - lo) <= 1.01 ? 0.005 : 0.05
    }
    private var range: ClosedRange<Double>? {
        guard let lo = schema["minimum"].number, let hi = schema["maximum"].number else { return nil }
        return lo...hi
    }
}

/// Numeric field: type a value, or drag horizontally on the label badge to scrub
/// (⇧ = ×10, ⌥ = ×0.1). Commits one undo step per edit/drag.
struct ScrubField: View {
    let value: Double
    var step: Double = 0.05
    var range: ClosedRange<Double>? = nil
    var axis: Int? = nil
    let onCommit: (Double) -> Void

    @State private var text = ""
    @State private var dragStart: Double?
    @State private var preview: Double?
    @FocusState private var focused: Bool

    private var axisColor: Color { [Theme.axisX, Theme.axisY, Theme.axisZ][axis ?? 0] }

    var body: some View {
        HStack(spacing: 0) {
            Rectangle()
                .fill(axis != nil ? axisColor : Theme.textFaint.opacity(0.5))
                .frame(width: axis != nil ? 3 : 2)
                .overlay {
                    Color.clear.frame(width: 12).contentShape(Rectangle())
                        .onHover { inside in if inside { NSCursor.resizeLeftRight.push() } else { NSCursor.pop() } }
                        .gesture(scrub)
                }
            TextField("", text: $text)
                .textFieldStyle(.plain)
                .font(Theme.monoSmall)
                .multilineTextAlignment(.trailing)
                .padding(.horizontal, 5)
                .focused($focused)
                .onSubmit(commitText)
                .onChange(of: focused) { _, f in if !f { commitText() } }
        }
        .frame(height: 20)
        .background(Theme.field, in: RoundedRectangle(cornerRadius: 3))
        .overlay(RoundedRectangle(cornerRadius: 3).stroke(focused ? Theme.accent : Theme.border, lineWidth: 1))
        .clipShape(RoundedRectangle(cornerRadius: 3))
        .onAppear { text = Self.format(value) }
        .onChange(of: value) { _, v in if !focused && preview == nil { text = Self.format(v) } }
    }

    private var scrub: some Gesture {
        DragGesture(minimumDistance: 2)
            .onChanged { g in
                let start = dragStart ?? value
                dragStart = start
                let mods = NSEvent.modifierFlags
                let mult = mods.contains(.shift) ? 10.0 : (mods.contains(.option) ? 0.1 : 1.0)
                var v = start + Double(g.translation.width) * step * mult
                if let range { v = min(max(v, range.lowerBound), range.upperBound) }
                preview = v
                text = Self.format(v)
            }
            .onEnded { _ in
                if let v = preview { onCommit(v) }
                dragStart = nil
                preview = nil
            }
    }

    private func commitText() {
        if let v = Double(text.replacingOccurrences(of: ",", with: ".")), v != value {
            onCommit(range.map { min(max(v, $0.lowerBound), $0.upperBound) } ?? v)
        } else {
            text = Self.format(value)
        }
    }

    static func format(_ v: Double) -> String {
        if v.rounded() == v && abs(v) < 1e7 { return String(Int(v)) }
        return String(format: "%.3f", v).replacingOccurrences(of: #"0+$"#, with: "", options: .regularExpression)
    }
}

/// A text field that commits on Return / focus loss.
struct CommitField: View {
    let text: String
    var placeholder = ""
    var font: Font = Theme.body
    let onCommit: (String) -> Void
    @State private var draft = ""
    @FocusState private var focused: Bool

    var body: some View {
        TextField(placeholder, text: $draft)
            .textFieldStyle(.plain)
            .font(font)
            .padding(.horizontal, 5)
            .frame(height: 20)
            .background(Theme.field, in: RoundedRectangle(cornerRadius: 3))
            .overlay(RoundedRectangle(cornerRadius: 3).stroke(focused ? Theme.accent : Theme.border, lineWidth: 1))
            .focused($focused)
            .onAppear { draft = text }
            .onChange(of: text) { _, t in if !focused { draft = t } }
            .onSubmit { if draft != text { onCommit(draft) } }
            .onChange(of: focused) { _, f in if !f && draft != text { onCommit(draft) } }
    }
}

/// World Settings: lighting, sky, fog, exposure (+ presets).
struct WorldSettings: View {
    @Environment(EngineStore.self) private var engine
    @State private var schema: JSON = .null

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            PropertySection(title: "Presets", icon: "wand.and.stars") {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 80), spacing: 6)], spacing: 6) {
                    ForEach(["noon", "sunset", "night", "overcast", "studio"], id: \.self) { preset in
                        Button(preset.capitalized) { engine.call("environment_update", ["preset": .string(preset)]) }
                            .controlSize(.small)
                    }
                }
            }
            if !schema.isNull {
                PropertySection(title: "Environment", icon: "sun.horizon") {
                    PropertyGrid(schema: schema, values: engine.environment) { field, value in
                        engine.call("environment_update", .object([(field, value)]))
                    }
                }
            }
        }
        .task { schema = engine.call("component_schema", ["component": "environment"], actor: "editor").structured["environment"] }
    }
}

/// A string field that references a project asset: type a value, pick from a menu of matching
/// assets, or drop an asset from the browser onto it.
struct AssetField: View {
    @Environment(EngineStore.self) private var engine
    let kind: String  // material | texture | mesh
    let value: String
    let onChange: (String) -> Void
    @State private var targeted = false
    @State private var options: [String] = []

    static func kind(for field: String, schema: JSON) -> String? {
        guard schema["type"].string == "string" else { return nil }
        return ["material": "material", "texture": "texture", "mesh": "mesh", "normalMap": "texture",
                "ormMap": "texture", "emissiveMap": "texture"][field]
    }

    var body: some View {
        HStack(spacing: 4) {
            CommitField(text: value, font: Theme.monoSmall) { onChange($0) }
            Menu {
                if kind == "mesh" {
                    Section("Primitives") {
                        ForEach(["cube", "sphere", "plane", "cylinder", "cone", "quad", "capsule", "torus"], id: \.self) { p in
                            Button(p) { onChange(p) }
                        }
                    }
                }
                Section("Project") {
                    if options.isEmpty { Text("No \(kind) assets") }
                    ForEach(options, id: \.self) { path in Button(path) { onChange(kind == "mesh" ? "asset:" + path : path) } }
                }
                if !value.isEmpty && kind != "mesh" {
                    Divider()
                    Button("None") { onChange("") }
                }
            } label: { Image(systemName: "shippingbox") }
            .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
            .help("Choose a \(kind) asset")
        }
        .overlay(RoundedRectangle(cornerRadius: 4).stroke(Theme.accent, lineWidth: targeted ? 1.5 : 0))
        .dropDestination(for: String.self) { items, _ in
            guard let s = items.first, s.hasPrefix(EngineStore.assetDragPrefix) else { return false }
            let path = String(s.dropFirst(EngineStore.assetDragPrefix.count))
            onChange(kind == "mesh" ? "asset:" + path : path)
            return true
        } isTargeted: { targeted = $0 }
        .task(id: engine.revision) {
            options = engine.call("asset_list", ["type": .string(kind), "limit": 200], actor: "editor")
                .structured["assets"].array.compactMap { $0["path"].string }
        }
    }
}

/// An entity-link field (joint target, camera follow, look-at...): type a name, pick from the scene, or drop an
/// entity from the outliner. Links store the entity id, so they keep working when the target is renamed.
struct EntityField: View {
    @Environment(EngineStore.self) private var engine
    let value: JSON  // {"id", "name"} or null
    let onChange: (JSON) -> Void
    @State private var targeted = false

    private var label: String { value["name"].string ?? value["id"].number.map { "#\(Int($0))" } ?? "" }
    private var dangling: Bool {
        guard let id = value["id"].number else { return false }
        return !engine.entities.contains { $0.id == UInt64(id) }
    }

    var body: some View {
        HStack(spacing: 4) {
            CommitField(text: label, placeholder: "None", font: Theme.monoSmall) { text in
                onChange(text.isEmpty ? .null : .string(text))
            }
            .overlay(RoundedRectangle(cornerRadius: 3).stroke(Theme.warning, lineWidth: dangling ? 1 : 0))
            .help(dangling ? "This link points at an entity that no longer exists (entity_refs lists broken links)" : "")
            Menu {
                ForEach(engine.entities.prefix(400)) { e in
                    Button("\(e.name)  #\(e.id)") { onChange(.number(Double(e.id))) }
                }
                if !value.isNull {
                    Divider()
                    Button("None") { onChange(.null) }
                }
            } label: { Image(systemName: "scope") }
            .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
            .help("Choose an entity (or drag one from the outliner)")
        }
        .overlay(RoundedRectangle(cornerRadius: 4).stroke(Theme.accent, lineWidth: targeted ? 1.5 : 0))
        .dropDestination(for: String.self) { items, _ in
            guard let s = items.first, s.hasPrefix(EngineStore.entityDragPrefix),
                  let id = Double(s.dropFirst(EngineStore.entityDragPrefix.count)) else { return false }
            onChange(.number(id))
            return true
        } isTargeted: { targeted = $0 }
    }
}

/// A list of entity links (collider sets): comma-separated names; dropping an entity adds it.
struct EntityListField: View {
    let value: JSON  // [{"id", "name"}, ...]
    let onChange: (JSON) -> Void
    @State private var targeted = false

    private var text: String {
        value.array.compactMap { $0["name"].string ?? $0["id"].number.map { "#\(Int($0))" } }.joined(separator: ", ")
    }

    var body: some View {
        CommitField(text: text, placeholder: "None", font: Theme.monoSmall) { onChange(.string($0)) }
            .overlay(RoundedRectangle(cornerRadius: 4).stroke(Theme.accent, lineWidth: targeted ? 1.5 : 0))
            .dropDestination(for: String.self) { items, _ in
                guard let s = items.first, s.hasPrefix(EngineStore.entityDragPrefix),
                      let id = Double(s.dropFirst(EngineStore.entityDragPrefix.count)) else { return false }
                onChange(.array(value.array + [.number(id)]))
                return true
            } isTargeted: { targeted = $0 }
    }
}

/// Linked prefab instance: where it comes from, how many overrides it has, and apply / revert / unpack.
struct PrefabSection: View {
    @Environment(EngineStore.self) private var engine
    let entityID: UInt64
    let prefab: JSON  // {"instance", "pid", "source"?}
    @State private var info: JSON = .null

    var body: some View {
        PropertySection(title: "Prefab", icon: "shippingbox") {
            VStack(alignment: .leading, spacing: 5) {
                HStack(spacing: 6) {
                    Text(info["source"].string ?? prefab["source"].string ?? "").font(Theme.monoSmall).lineLimit(1)
                        .truncationMode(.middle)
                    Spacer()
                    Text(overrideText).font(Theme.label).foregroundStyle(Theme.textDim)
                }
                ForEach(Array(info["overrides"].array.prefix(8).enumerated()), id: \.offset) { _, o in
                    Text("\(o["node"].string ?? "") · \(o["property"].string ?? "")").font(Theme.label)
                        .foregroundStyle(Theme.accent).lineLimit(1)
                }
                HStack(spacing: 6) {
                    Button("Apply") { run("prefab_apply", ["entity": .number(Double(entityID))]) }
                        .help("Push this instance's overrides into the prefab file (every instance updates)")
                    Button("Revert") { run("prefab_revert", ["entity": .number(Double(entityID)), "all": true]) }
                        .help("Make the whole instance match its prefab again")
                    Button("Unpack") { run("prefab_unpack", ["entity": .number(Double(entityID))]) }
                        .help("Stop linking to the prefab; keep the entities as they are")
                }
                .controlSize(.small)
            }
        }
        .task(id: "\(entityID)-\(engine.revision)") {
            info = engine.call("prefab_overrides", ["entity": .number(Double(entityID))], actor: "editor").structured
        }
    }

    private var overrideText: String {
        let n = Int(info["override_count"].number ?? 0)
        return n == 0 ? "matches prefab" : "\(n) override\(n == 1 ? "" : "s")"
    }

    private func run(_ tool: String, _ args: JSON) {
        _ = engine.call(tool, args)
    }
}
