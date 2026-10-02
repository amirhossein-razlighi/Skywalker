import SwiftUI

enum LeftTab: String, CaseIterable, Identifiable {
    case outliner = "Outliner", assets = "Assets"
    var id: String { rawValue }
    var symbol: String { self == .outliner ? "list.bullet.indent" : "folder" }
}

struct OutlinerPanel: View {
    @State private var tab: LeftTab = .outliner

    var body: some View {
        VStack(spacing: 0) {
            PanelTabs(tabs: LeftTab.allCases, selection: $tab, title: { $0.rawValue }, icon: { $0.symbol })
            switch tab {
            case .outliner: OutlinerView()
            case .assets: AssetBrowser()
            }
        }
        .background(Theme.panel)
    }
}

struct OutlinerNode: Identifiable, Hashable {
    let entity: EntitySummary
    var children: [OutlinerNode]?
    var id: UInt64 { entity.id }
}

/// Scene tree: type icons, visibility toggles, search, add menu, context actions.
/// A custom tree (rather than List + OutlineGroup) so selection is owned by the engine:
/// rebuilding rows after scene edits never drops or rewrites the selection.
struct OutlinerView: View {
    @Environment(EngineStore.self) private var engine
    @State private var filter = ""
    @State private var collapsed: Set<UInt64> = []
    @State private var anchor: UInt64?

    private struct Row: Identifiable {
        let entity: EntitySummary
        let depth: Int
        let hasChildren: Bool
        var id: UInt64 { entity.id }
    }

    private var rows: [Row] {
        let all = engine.entities
        if !filter.isEmpty {
            return all.filter { $0.name.localizedCaseInsensitiveContains(filter) }
                .map { Row(entity: $0, depth: 0, hasChildren: false) }
        }
        let byParent = Dictionary(grouping: all, by: \.parent)
        var out: [Row] = []
        func walk(_ parent: UInt64, _ depth: Int) {
            for e in byParent[parent] ?? [] {
                let kids = !(byParent[e.id] ?? []).isEmpty
                out.append(Row(entity: e, depth: depth, hasChildren: kids))
                if kids && !collapsed.contains(e.id) { walk(e.id, depth + 1) }
            }
        }
        walk(0, 0)
        return out
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 6) {
                HStack(spacing: 4) {
                    Image(systemName: "magnifyingglass").font(.system(size: 10)).foregroundStyle(Theme.textFaint)
                    TextField("Search", text: $filter).textFieldStyle(.plain).font(Theme.label)
                }
                .padding(.horizontal, 6)
                .frame(height: 22)
                .background(Theme.field, in: RoundedRectangle(cornerRadius: 4))
                AddEntityMenu()
            }
            .padding(6)
            ScrollView {
                LazyVStack(spacing: 0) {
                    let visible = rows
                    ForEach(visible) { row in
                        OutlinerRow(entity: row.entity, depth: row.depth, hasChildren: row.hasChildren,
                                    expanded: !collapsed.contains(row.id), selected: engine.selection.contains(row.id),
                                    toggleExpanded: {
                                        if collapsed.contains(row.id) { collapsed.remove(row.id) } else { collapsed.insert(row.id) }
                                    })
                            .contentShape(Rectangle())
                            .onTapGesture { select(row.id, in: visible) }
                            .contextMenu { contextMenu(for: row.entity) }
                    }
                }
                .padding(.bottom, 8)
            }
            .contentShape(Rectangle())
            .onTapGesture { engine.selection = [] }
            HStack {
                Text("\(engine.entities.count) entities").font(.system(size: 10)).foregroundStyle(Theme.textFaint)
                Spacer()
            }
            .padding(.horizontal, 8)
            .frame(height: 20)
        }
    }

    private func select(_ id: UInt64, in visible: [Row]) {
        let mods = NSEvent.modifierFlags
        if mods.contains(.command) {
            if engine.selection.contains(id) { engine.selection.remove(id) } else { engine.selection.insert(id) }
            anchor = id
        } else if mods.contains(.shift), let anchor, let a = visible.firstIndex(where: { $0.id == anchor }),
                  let b = visible.firstIndex(where: { $0.id == id }) {
            engine.selection = Set(visible[min(a, b)...max(a, b)].map(\.id))
        } else {
            engine.selection = [id]
            anchor = id
        }
    }

    @ViewBuilder
    private func contextMenu(for e: EntitySummary) -> some View {
        let id = JSON.number(Double(e.id))
        Button("Frame") { engine.call("camera_set", ["frame": id]) }
        Button("Duplicate") { engine.call("entity_duplicate", ["entity": id, "offset": .vec3(1, 0, 0)]) }
        Button(e.enabled ? "Disable" : "Enable") { engine.call("entity_update", ["entity": id, "enabled": .bool(!e.enabled)]) }
        Button("Unparent") { engine.call("entity_update", ["entity": id, "parent": 0]) }.disabled(e.parent == 0)
        Divider()
        Button("Delete", role: .destructive) { engine.call("entity_delete", ["entity": id]) }
    }
}

struct OutlinerRow: View {
    @Environment(EngineStore.self) private var engine
    let entity: EntitySummary
    var depth = 0
    var hasChildren = false
    var expanded = true
    var selected = false
    var toggleExpanded: () -> Void = {}
    @State private var hovering = false

    var body: some View {
        HStack(spacing: 6) {
            Button(action: toggleExpanded) {
                Image(systemName: "chevron.right").font(.system(size: 8, weight: .bold))
                    .rotationEffect(.degrees(expanded ? 90 : 0))
                    .foregroundStyle(Theme.textFaint)
                    .frame(width: 10, height: 16)
                    .opacity(hasChildren ? 1 : 0)
            }
            .buttonStyle(.plain)
            .disabled(!hasChildren)
            Image(systemName: icon).font(.system(size: 10.5)).foregroundStyle(tint).frame(width: 14)
            Text(entity.name).font(Theme.body).lineLimit(1).foregroundStyle(entity.enabled ? Theme.text : Theme.textFaint)
            if entity.components.contains("behaviors") {
                Image(systemName: "sparkles").font(.system(size: 8.5)).foregroundStyle(Theme.ai).help("Has behaviors")
            }
            Spacer(minLength: 4)
            Button {
                engine.call("entity_update", ["entity": .number(Double(entity.id)), "enabled": .bool(!entity.enabled)])
            } label: {
                Image(systemName: entity.enabled ? "eye" : "eye.slash").font(.system(size: 10))
                    .foregroundStyle(entity.enabled ? Theme.textFaint : Theme.textDim)
            }
            .buttonStyle(.plain)
            .help(entity.enabled ? "Disable" : "Enable")
            .opacity(hovering || !entity.enabled ? 1 : 0.55)
        }
        .padding(.leading, 6 + CGFloat(depth) * 14)
        .padding(.trailing, 8)
        .frame(height: 22)
        .background(selected ? Theme.accent.opacity(0.28) : (hovering ? Theme.hover : .clear))
        .overlay(alignment: .leading) { if selected { Rectangle().fill(Theme.accent).frame(width: 2) } }
        .onHover { hovering = $0 }
    }

    private var icon: String {
        let c = entity.components
        if c.contains("camera") { return "video" }
        if c.contains("light") { return "lightbulb" }
        if c.contains("mesh") { return "cube" }
        return "circle.dotted"
    }

    private var tint: Color {
        let c = entity.components
        if c.contains("camera") { return Theme.textDim }
        if c.contains("light") { return Theme.warning }
        if c.contains("mesh") { return Theme.accent }
        return Theme.textFaint
    }
}

struct AddEntityMenu: View {
    @Environment(EngineStore.self) private var engine

    var body: some View {
        Menu {
            Section("Shapes") {
                ForEach(["cube", "sphere", "plane", "cylinder", "cone", "capsule", "torus", "quad"], id: \.self) { mesh in
                    Button(mesh.capitalized) {
                        add(["name": .string(mesh.capitalized), "mesh": .string(mesh), "position": .vec3(0, mesh == "plane" ? 0 : 0.5, 0)])
                    }
                }
            }
            Section("Lights") {
                Button("Point Light") { add(["name": "Point Light", "position": .vec3(0, 2, 0), "components": ["light": ["kind": "point", "intensity": 4]]]) }
                Button("Spot Light") { add(["name": "Spot Light", "position": .vec3(0, 3, 0), "rotation": .vec3(-90, 0, 0), "components": ["light": ["kind": "spot", "intensity": 8]]]) }
                Button("Directional Light") { add(["name": "Directional Light", "position": .vec3(0, 4, 0), "rotation": .vec3(-50, 30, 0), "components": ["light": ["kind": "directional", "intensity": 1]]]) }
            }
            Button("Camera") { add(["name": "Camera", "position": .vec3(0, 2, 6), "components": ["camera": ["fov": 55]]]) }
            Button("Empty") { add(["name": "Empty"]) }
        } label: {
            Image(systemName: "plus")
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
        .help("Add entity")
    }

    private func add(_ args: JSON) {
        let r = engine.call("entity_create", args)
        if let id = r.structured["id"].number { engine.selection = [UInt64(id)] }
    }
}

/// Project files: scenes, meshes (OBJ, e.g. from 3D-generation models), images.
struct AssetBrowser: View {
    @Environment(EngineStore.self) private var engine
    @State private var files: [URL] = []
    @State private var filter = ""

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 6) {
                TextField("Filter", text: $filter).textFieldStyle(.plain).font(Theme.label)
                    .padding(.horizontal, 6).frame(height: 22)
                    .background(Theme.field, in: RoundedRectangle(cornerRadius: 4))
                IconButton(symbol: "arrow.clockwise", help: "Rescan") { scan() }
                IconButton(symbol: "folder", help: "Reveal project in Finder") {
                    NSWorkspace.shared.activateFileViewerSelecting([engine.projectDirectory])
                }
            }
            .padding(6)
            List {
                ForEach(grouped, id: \.0) { group, urls in
                    Section(group) {
                        ForEach(urls, id: \.self) { url in
                            AssetRow(url: url, relative: relative(url)).listRowSeparator(.hidden)
                        }
                    }
                }
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .overlay {
                if files.isEmpty {
                    Text("Drop .obj meshes, images and scenes into\n\(engine.projectDirectory.lastPathComponent)/")
                        .font(Theme.label).foregroundStyle(Theme.textFaint).multilineTextAlignment(.center)
                }
            }
        }
        .onAppear(perform: scan)
        .onChange(of: engine.sceneName) { _, _ in scan() }
    }

    private var grouped: [(String, [URL])] {
        let visible = files.filter { filter.isEmpty || $0.lastPathComponent.localizedCaseInsensitiveContains(filter) }
        let groups: [(String, (URL) -> Bool)] = [
            ("Scenes", { $0.lastPathComponent.hasSuffix(".sky.json") }),
            ("Meshes", { $0.pathExtension.lowercased() == "obj" }),
            ("Images", { ["png", "jpg", "jpeg"].contains($0.pathExtension.lowercased()) }),
            ("Audio", { ["wav", "mp3", "ogg", "m4a"].contains($0.pathExtension.lowercased()) }),
        ]
        return groups.compactMap { name, test in
            let items = visible.filter(test)
            return items.isEmpty ? nil : (name, items)
        }
    }

    private func relative(_ url: URL) -> String {
        let base = engine.projectDirectory.standardizedFileURL.path
        let p = url.standardizedFileURL.path
        return p.hasPrefix(base) ? String(p.dropFirst(base.count + 1)) : p
    }

    private func scan() {
        let root = engine.projectDirectory
        guard let e = FileManager.default.enumerator(at: root, includingPropertiesForKeys: nil,
                                                     options: [.skipsHiddenFiles, .skipsPackageDescendants]) else { return }
        var out: [URL] = []
        for case let url as URL in e where out.count < 2000 {
            let ext = url.pathExtension.lowercased()
            if url.lastPathComponent.hasSuffix(".sky.json") || ["obj", "png", "jpg", "jpeg", "wav", "mp3", "ogg", "m4a"].contains(ext) {
                out.append(url)
            }
        }
        files = out.sorted { $0.lastPathComponent < $1.lastPathComponent }
    }
}

private struct AssetRow: View {
    @Environment(EngineStore.self) private var engine
    let url: URL
    let relative: String

    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: icon).font(.system(size: 10.5)).foregroundStyle(Theme.textDim).frame(width: 14)
            Text(url.lastPathComponent).font(Theme.body).lineLimit(1)
        }
        .help(relative)
        .contextMenu { actions }
        .onTapGesture(count: 2) { primary() }
    }

    private var icon: String {
        if url.lastPathComponent.hasSuffix(".sky.json") { return "globe" }
        switch url.pathExtension.lowercased() {
        case "obj": return "cube.transparent"
        case "png", "jpg", "jpeg": return "photo"
        default: return "waveform"
        }
    }

    @ViewBuilder private var actions: some View {
        if url.lastPathComponent.hasSuffix(".sky.json") {
            Button("Open Scene") { engine.call("scene_load", ["path": .string(relative)]) }
        } else if url.pathExtension.lowercased() == "obj" {
            Button("Add to Scene") { primary() }
            Button("Assign to Selection") {
                if let id = engine.selection.first {
                    engine.call("asset_import_mesh", ["path": .string(relative), "entity": .number(Double(id))])
                }
            }
            .disabled(engine.selection.count != 1)
        } else if ["png", "jpg", "jpeg"].contains(url.pathExtension.lowercased()) {
            Button("Apply as Texture to Selection") { primary() }.disabled(engine.selection.isEmpty)
        }
        Button("Reveal in Finder") { NSWorkspace.shared.activateFileViewerSelecting([url]) }
    }

    private func primary() {
        if url.lastPathComponent.hasSuffix(".sky.json") {
            engine.call("scene_load", ["path": .string(relative)])
        } else if url.pathExtension.lowercased() == "obj" {
            let name = url.deletingPathExtension().lastPathComponent
            let created = engine.call("entity_create", ["name": .string(name), "position": .vec3(0, 0.5, 0), "mesh": "cube"])
            if let id = created.structured["id"].number {
                engine.call("asset_import_mesh", ["path": .string(relative), "entity": .number(id)])
                engine.selection = [UInt64(id)]
            }
        } else if ["png", "jpg", "jpeg"].contains(url.pathExtension.lowercased()) {
            let ops: [JSON] = engine.selection.map {
                ["tool": "entity_update", "args": ["entity": .number(Double($0)), "components": ["mesh": ["texture": .string(relative)]]]]
            }
            if !ops.isEmpty { engine.call("batch", ["operations": .array(ops), "label": "Apply texture"]) }
        }
    }
}
