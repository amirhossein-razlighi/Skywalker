import AppKit
import SwiftUI

/// One row of `asset_list`.
struct AssetItem: Identifiable, Hashable, Sendable {
    var id: String { path }
    let path: String
    let type: String
    let tags: [String]
    let description: String
    let rev: Double
    let generated: Bool

    var name: String {
        let file = (path as NSString).lastPathComponent
        for ext in [".mat.json", ".prefab.json", ".sky.json", ".agent.json"] where file.hasSuffix(ext) {
            return String(file.dropLast(ext.count))
        }
        return (file as NSString).deletingPathExtension
    }
    var previewable: Bool { ["mesh", "material", "texture", "prefab"].contains(type) }
    var symbol: String {
        switch type {
        case "mesh": "cube"
        case "texture": "photo"
        case "material": "circle.lefthalf.filled"
        case "prefab": "shippingbox"
        case "scene": "mountain.2"
        case "audio": "waveform"
        case "video": "film"
        case "script": "chevron.left.forwardslash.chevron.right"
        case "agent": "cloud"
        default: "doc"
        }
    }

    init(_ j: JSON) {
        path = j["path"].string ?? ""
        type = j["type"].string ?? "unknown"
        tags = j["tags"].array.compactMap(\.string)
        description = j["description"].string ?? ""
        rev = j["rev"].number ?? 0
        generated = !j["source"]["prompt"].isNull
    }
}

/// Lazily rendered, cached asset thumbnails (rendered by the engine's `asset_preview`).
@MainActor
@Observable
final class ThumbnailCache {
    private struct Entry {
        var rev: Double
        var image: NSImage?
    }
    private var entries: [String: Entry] = [:]
    @ObservationIgnored private var pending: Set<String> = []

    func image(for item: AssetItem) -> NSImage? {
        guard let e = entries[item.path], e.rev == item.rev else { return nil }
        return e.image
    }

    func load(_ item: AssetItem, engine: EngineStore) async {
        guard item.previewable, entries[item.path]?.rev != item.rev, !pending.contains(item.path) else { return }
        pending.insert(item.path)
        defer { pending.remove(item.path) }
        await Task.yield()  // keep scrolling smooth: one preview per runloop turn
        let r = engine.call("asset_preview", ["asset": .string(item.path), "size": 160], actor: "editor")
        let image = r.imagesBase64.first.flatMap { Data(base64Encoded: $0) }.flatMap(NSImage.init(data:))
        entries[item.path] = Entry(rev: item.rev, image: image)
    }
}

enum AssetFilter: String, CaseIterable, Identifiable {
    case all = "All", mesh = "Meshes", material = "Materials", prefab = "Prefabs", texture = "Textures",
         scene = "Scenes", audio = "Audio", agent = "Agents"
    var id: String { rawValue }
    var type: String? {
        switch self {
        case .all: nil
        case .mesh: "mesh"
        case .material: "material"
        case .prefab: "prefab"
        case .texture: "texture"
        case .scene: "scene"
        case .audio: "audio"
        case .agent: "agent"
        }
    }
}

/// Project asset browser: filter, search, preview, and drag into the viewport or onto fields.
struct AssetBrowser: View {
    @Environment(EngineStore.self) private var engine
    @State private var items: [AssetItem] = []
    @State private var filter: AssetFilter = .all
    @State private var query = ""
    @State private var selected: String?
    @State private var thumbs = ThumbnailCache()
    @State private var info: JSON = .null

    var body: some View {
        HStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 1) {
                ForEach(AssetFilter.allCases) { f in
                    Button { filter = f } label: {
                        HStack {
                            Text(f.rawValue).font(Theme.label)
                            Spacer()
                            Text("\(count(f))").font(Theme.monoSmall).foregroundStyle(Theme.textFaint)
                        }
                        .padding(.horizontal, 8).frame(height: 20)
                        .background(filter == f ? Theme.accent.opacity(0.18) : .clear, in: RoundedRectangle(cornerRadius: 4))
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                }
                Spacer()
            }
            .padding(6)
            .frame(width: 130)
            .background(Theme.panel)
            Rectangle().fill(Theme.border).frame(width: 1)

            VStack(spacing: 0) {
                HStack(spacing: 6) {
                    Image(systemName: "magnifyingglass").font(.system(size: 10)).foregroundStyle(Theme.textFaint)
                    TextField("Search assets, tags, descriptions", text: $query).textFieldStyle(.plain).font(Theme.label)
                    IconButton(symbol: "arrow.clockwise", help: "Rescan the project folder") { reload(rescan: true) }
                    IconButton(symbol: "folder", help: "Show the project in Finder") {
                        NSWorkspace.shared.open(engine.projectDirectory)
                    }
                }
                .padding(.horizontal, 8).frame(height: 24)
                .overlay(alignment: .bottom) { Rectangle().fill(Theme.border).frame(height: 1) }

                ScrollView {
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 92, maximum: 120), spacing: 8)], spacing: 8) {
                        ForEach(visible) { item in
                            AssetCell(item: item, image: thumbs.image(for: item), selected: selected == item.path)
                                .onTapGesture { select(item) }
                                .onTapGesture(count: 2) { place(item) }
                                .draggable(EngineStore.assetDragPrefix + item.path)
                                .task(id: item.rev) { await thumbs.load(item, engine: engine) }
                                .contextMenu { menu(for: item) }
                        }
                    }
                    .padding(8)
                }
                .overlay {
                    if visible.isEmpty {
                        Text(items.isEmpty
                             ? "No assets yet. Drop .glb/.obj/.png files into the project folder, or ask an agent to make materials and prefabs."
                             : "Nothing matches.")
                            .font(Theme.label).foregroundStyle(Theme.textFaint).multilineTextAlignment(.center).padding()
                    }
                }
            }

            if let item = items.first(where: { $0.path == selected }) {
                Rectangle().fill(Theme.border).frame(width: 1)
                AssetInspector(item: item, image: thumbs.image(for: item), info: info, onPlace: { place(item) })
                    .frame(width: 230)
            }
        }
        .task { reload(rescan: false) }
        .onChange(of: engine.revision) { _, _ in reload(rescan: false) }
        .task {
            // The engine rescans the folder every few seconds; pick up new files.
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(3))
                reload(rescan: false)
            }
        }
    }

    private var visible: [AssetItem] {
        items.filter { item in
            (filter.type == nil || item.type == filter.type)
                && (query.isEmpty || item.path.localizedCaseInsensitiveContains(query)
                    || item.description.localizedCaseInsensitiveContains(query)
                    || item.tags.contains { $0.localizedCaseInsensitiveContains(query) })
        }
    }

    private func count(_ f: AssetFilter) -> Int { f.type == nil ? items.count : items.filter { $0.type == f.type }.count }

    private func reload(rescan: Bool) {
        if rescan { engine.call("asset_refresh", [:], actor: "editor") }
        let r = engine.call("asset_list", ["limit": 5000], actor: "editor")
        let fresh = r.structured["assets"].array.map(AssetItem.init).sorted { $0.path < $1.path }
        if fresh != items { items = fresh }
        if let selected, !items.contains(where: { $0.path == selected }) { self.selected = nil }
    }

    private func select(_ item: AssetItem) {
        selected = item.path
        info = engine.call("asset_info", ["asset": .string(item.path)], actor: "editor").structured
    }

    private func place(_ item: AssetItem) {
        engine.placeAsset(item.path, type: item.type, at: nil)
    }

    @ViewBuilder
    private func menu(for item: AssetItem) -> some View {
        if ["mesh", "prefab"].contains(item.type) { Button("Place in Scene") { place(item) } }
        if ["material", "texture", "mesh"].contains(item.type) {
            Button("Assign to Selection") { engine.assignAsset(item.path, type: item.type, to: Array(engine.selection)) }
                .disabled(engine.selection.isEmpty)
        }
        if item.type == "scene" { Button("Open Scene") { engine.call("scene_load", ["path": .string(item.path)]) } }
        Divider()
        Button("Copy Path") {
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(item.path, forType: .string)
        }
        Button("Reveal in Finder") {
            NSWorkspace.shared.activateFileViewerSelecting([engine.projectDirectory.appending(path: item.path)])
        }
    }
}

private struct AssetCell: View {
    let item: AssetItem
    let image: NSImage?
    let selected: Bool

    var body: some View {
        VStack(spacing: 4) {
            ZStack {
                RoundedRectangle(cornerRadius: 6).fill(Theme.field)
                if let image {
                    Image(nsImage: image).resizable().scaledToFill()
                } else {
                    Image(systemName: item.symbol).font(.system(size: 22, weight: .light)).foregroundStyle(Theme.textFaint)
                }
            }
            .frame(maxWidth: .infinity)
            .frame(height: 84)
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .overlay(alignment: .topTrailing) {
                if item.generated {
                    Image(systemName: "sparkles").font(.system(size: 9, weight: .semibold)).foregroundStyle(.white)
                        .padding(3).background(Theme.accent, in: Circle()).padding(4)
                        .help("Generated")
                }
            }
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(selected ? Theme.accent : Theme.border, lineWidth: selected ? 2 : 1))
            Text(item.name).font(Theme.label).lineLimit(1).truncationMode(.middle)
            Text(item.type).font(.system(size: 9)).foregroundStyle(Theme.textFaint)
        }
        .contentShape(Rectangle())
        .help(item.description.isEmpty ? item.path : "\(item.path)\n\(item.description)")
    }
}

private struct AssetInspector: View {
    @Environment(EngineStore.self) private var engine
    let item: AssetItem
    let image: NSImage?
    let info: JSON
    let onPlace: () -> Void

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                if let image {
                    Image(nsImage: image).resizable().scaledToFit().clipShape(RoundedRectangle(cornerRadius: 6))
                }
                Text(item.name).font(Theme.sectionTitle)
                Text(item.path).font(Theme.monoSmall).foregroundStyle(Theme.textFaint).textSelection(.enabled)
                if !item.description.isEmpty { Text(item.description).font(Theme.label).foregroundStyle(Theme.textDim) }
                if !item.tags.isEmpty {
                    Text(item.tags.map { "#\($0)" }.joined(separator: " ")).font(Theme.label).foregroundStyle(Theme.accent)
                }
                if let prompt = info["source"]["prompt"].string {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Generated from").font(.system(size: 10)).foregroundStyle(Theme.textFaint)
                        Text("“\(prompt)”").font(Theme.label).foregroundStyle(Theme.textDim)
                    }
                }
                let users = info["usedBy"].array
                Text(users.isEmpty ? "Not used in this scene" : "Used by \(users.count) entit\(users.count == 1 ? "y" : "ies")")
                    .font(Theme.label).foregroundStyle(Theme.textFaint)
                if !info["material"].isNull {
                    Text(info["material"].serialized(pretty: true)).font(Theme.monoSmall).foregroundStyle(Theme.textDim)
                }
                HStack {
                    if ["mesh", "prefab"].contains(item.type) {
                        Button("Place", systemImage: "plus.square.on.square", action: onPlace)
                    }
                    if ["material", "texture", "mesh"].contains(item.type) {
                        Button("Assign") { engine.assignAsset(item.path, type: item.type, to: Array(engine.selection)) }
                            .disabled(engine.selection.isEmpty)
                    }
                }
                .controlSize(.small)
                Text("Drag onto the viewport to place, or onto an object to apply.")
                    .font(.system(size: 10)).foregroundStyle(Theme.textFaint)
            }
            .padding(10)
        }
    }
}
