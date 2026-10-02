import SwiftUI

struct HierarchyNode: Identifiable, Hashable {
    let entity: EntitySummary
    var children: [HierarchyNode]?
    var id: UInt64 { entity.id }
}

/// Scene outline with search, built from the engine's `scene_overview`.
struct HierarchyView: View {
    @Environment(EngineStore.self) private var engine
    @State private var filter = ""

    private var nodes: [HierarchyNode] {
        let all = engine.entities
        if !filter.isEmpty {
            return all.filter { $0.name.localizedCaseInsensitiveContains(filter) }.map { HierarchyNode(entity: $0, children: nil) }
        }
        let byParent = Dictionary(grouping: all, by: \.parent)
        func build(_ parent: UInt64) -> [HierarchyNode] {
            (byParent[parent] ?? []).map { e in
                let kids = build(e.id)
                return HierarchyNode(entity: e, children: kids.isEmpty ? nil : kids)
            }
        }
        return build(0)
    }

    var body: some View {
        @Bindable var engine = engine
        List(selection: $engine.selection) {
            Section(engine.sceneName) {
                OutlineGroup(nodes, children: \.children) { node in
                    Label {
                        Text(node.entity.name).lineLimit(1)
                    } icon: {
                        Image(systemName: icon(for: node.entity)).foregroundStyle(tint(for: node.entity))
                    }
                    .tag(node.id)
                    .contextMenu { contextMenu(for: node.entity) }
                }
            }
        }
        .searchable(text: $filter, placement: .sidebar, prompt: "Find entities")
        .safeAreaInset(edge: .bottom) { AddEntityBar().padding(8) }
        .navigationTitle("Scene")
    }

    @ViewBuilder
    private func contextMenu(for e: EntitySummary) -> some View {
        Button("Frame") { engine.call("camera_set", ["frame": .number(Double(e.id))]) }
        Button("Duplicate") { engine.call("entity_duplicate", ["entity": .number(Double(e.id)), "offset": .vec3(1, 0, 0)]) }
        Divider()
        Button("Delete", role: .destructive) { engine.call("entity_delete", ["entity": .number(Double(e.id))]) }
    }

    private func icon(for e: EntitySummary) -> String {
        if e.components.contains("camera") { return "video.fill" }
        if e.components.contains("light") { return "lightbulb.fill" }
        if e.components.contains("behaviors") { return "wand.and.stars" }
        if e.components.contains("mesh") { return "cube.fill" }
        return "circle.dashed"
    }

    private func tint(for e: EntitySummary) -> Color {
        if e.components.contains("camera") { return .secondary }
        if e.components.contains("light") { return Theme.dawn }
        if e.components.contains("behaviors") { return Theme.violet }
        return Theme.sky
    }
}

/// Quick-add menu for primitives, lights and cameras.
struct AddEntityBar: View {
    @Environment(EngineStore.self) private var engine

    var body: some View {
        Menu {
            Section("Shapes") {
                ForEach(["cube", "sphere", "plane", "cylinder", "cone", "capsule", "torus", "quad"], id: \.self) { mesh in
                    Button(mesh.capitalized) { add(["name": .string(mesh.capitalized), "mesh": .string(mesh), "position": .vec3(0, 0.5, 0)]) }
                }
            }
            Section("Lights & cameras") {
                Button("Point Light") { add(["name": "Point Light", "position": .vec3(0, 2, 0), "components": ["light": ["kind": "point", "intensity": 4]]]) }
                Button("Spot Light") { add(["name": "Spot Light", "position": .vec3(0, 3, 0), "rotation": .vec3(-90, 0, 0), "components": ["light": ["kind": "spot", "intensity": 8]]]) }
                Button("Camera") { add(["name": "Camera", "position": .vec3(0, 2, 6), "components": ["camera": ["fov": 55]]]) }
            }
            Button("Empty") { add(["name": "Empty"]) }
        } label: {
            Label("Add", systemImage: "plus.circle.fill").frame(maxWidth: .infinity)
        }
        .menuStyle(.borderlessButton)
        .controlSize(.large)
    }

    private func add(_ args: JSON) {
        let r = engine.call("entity_create", args)
        if let id = r.structured["id"].number { engine.selection = [UInt64(id)] }
    }
}
