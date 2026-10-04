import AppKit
import SwiftUI

// Visual node graph for a Wander behavior.
//
// The graph comes from the engine (`behavior_graph`): every handler, fn and test is a
// *body* with an entry node, statements chained by exec wires and expressions wired into
// data pins (literals sit inline on pins). Edits — moving nodes, editing pin values and
// node properties, adding nodes from the palette, connecting or removing wires — are sent
// back with `behavior_from_graph`, which regenerates the Wander code (and keeps the node
// positions), so the graph and the code are always the same behavior.

// MARK: - Model

struct GraphPin: Hashable {
    var name: String
    var kind: String  // "exec" | "data"
    var value: String?
    var isExec: Bool { kind == "exec" }
}

struct GraphNode: Identifiable, Hashable {
    let id: String
    var type: String
    var title: String
    var x: Double
    var y: Double
    var props: JSON
    var inputs: [GraphPin]
    var outputs: [GraphPin]
    var comment: [String]

    init(json: JSON) {
        id = json["id"].string ?? UUID().uuidString
        type = json["type"].string ?? "value"
        title = json["title"].string ?? ""
        x = json["x"].number ?? 0
        y = json["y"].number ?? 0
        props = json["props"]
        inputs = json["inputs"].array.map { GraphPin(name: $0["name"].string ?? "", kind: $0["kind"].string ?? "data", value: $0["value"].string) }
        outputs = json["outputs"].array.map { GraphPin(name: $0["name"].string ?? "", kind: $0["kind"].string ?? "data", value: nil) }
        comment = json["comment"].array.compactMap(\.string)
    }

    var json: JSON {
        var j: JSON = ["id": .string(id), "type": .string(type), "x": .number(x.rounded()), "y": .number(y.rounded())]
        if !title.isEmpty { j.set("title", .string(title)) }
        if !props.isNull { j.set("props", props) }
        j.set("inputs", .array(inputs.map { p in
            var o: JSON = ["name": .string(p.name), "kind": .string(p.kind)]
            if let v = p.value { o.set("value", .string(v)) }
            return o
        }))
        j.set("outputs", .array(outputs.map { ["name": .string($0.name), "kind": .string($0.kind)] }))
        if !comment.isEmpty { j.set("comment", .array(comment.map { .string($0) })) }
        return j
    }

    var group: String {
        switch type {
        case "entry": "entry"
        case "op", "not", "neg", "member", "index", "call", "method", "list", "map", "vector", "text", "value":
            type == "call" && inputs.first?.isExec == true ? "action" : "expression"
        case "if", "while", "for", "repeat", "every", "after", "wait", "break", "continue", "return", "stop", "go_to": "flow"
        case "let", "set": "variable"
        case "expect", "press", "hold", "release", "click": "test"
        default: "action"
        }
    }

    /// Short header text: the node type plus its key property.
    var header: String {
        if type == "entry" { return title }
        let p = props
        switch type {
        case "op": return p["op"].string ?? "op"
        case "let": return "let \(p["name"].string ?? "")"
        case "set": return "set \(p["target"].string ?? "")\(p["op"].string.map { " \($0)=" } ?? "")"
        case "member": return ".\(p["name"].string ?? "")"
        case "call": return inputs.first?.isExec == true ? "call" : "\(p["function"].string ?? "")()"
        case "method": return ".\(p["method"].string ?? "")()"
        case "go_to": return "go to \(p["state"].string ?? "")"
        case "emit": return "emit \"\(p["event"].string ?? "")\""
        case "for": return "for \(p["var"].string ?? "")"
        case "wait": return "wait (\(p["mode"].string ?? "seconds"))"
        case "value": return p["code"].string ?? "value"
        case "text": return "text"
        default: return type.replacingOccurrences(of: "_", with: " ")
        }
    }
}

struct GraphLink: Hashable {
    var from: String
    var out: String
    var to: String
    var input: String
    var json: JSON { ["from": .string(from), "out": .string(out), "to": .string(to), "in": .string(input)] }
}

/// Where a body lives in the graph JSON, e.g. ["behaviors", 0, "states", 1, "handlers", 0].
enum GraphPathKey: Hashable {
    case key(String)
    case index(Int)
}

struct GraphBody: Identifiable, Hashable {
    var id: String
    var title: String
    var path: [GraphPathKey]
}

extension JSON {
    func at(_ path: [GraphPathKey]) -> JSON {
        var cur = self
        for k in path {
            switch k {
            case let .key(s): cur = cur[s]
            case let .index(i): cur = cur[i]
            }
        }
        return cur
    }

    mutating func update(_ path: [GraphPathKey], _ value: JSON) {
        guard let first = path.first else { self = value; return }
        let rest = Array(path.dropFirst())
        switch first {
        case let .key(s):
            var child = self[s]
            child.update(rest, value)
            set(s, child)
        case let .index(i):
            guard case var .array(items) = self, items.indices.contains(i) else { return }
            items[i].update(rest, value)
            self = .array(items)
        }
    }
}

// MARK: - Layout constants

private enum GL {
    static let nodeWidth: CGFloat = 196
    static let header: CGFloat = 22
    static let row: CGFloat = 20
    static let pin: CGFloat = 9

    static func height(_ n: GraphNode) -> CGFloat {
        header + CGFloat(max(max(n.inputs.count, n.outputs.count), 1)) * row + 6
    }
}

private func groupColor(_ group: String) -> Color {
    switch group {
    case "entry": Color(hex: "#8a3a3a")
    case "flow": Color(hex: "#3f5683")
    case "variable": Color(hex: "#5b4a86")
    case "test": Color(hex: "#7a6a2c")
    case "expression": Color(hex: "#3e4149")
    default: Color(hex: "#2f6655")
    }
}

// MARK: - The editor

struct BehaviorGraphView: View {
    @Environment(EngineStore.self) private var engine
    let entityID: UInt64
    let behaviorName: String
    /// The behavior's saved source: the graph reloads when it changes.
    let source: String
    var compact = true

    @State private var graph: JSON = .null
    @State private var palette: [JSON] = []
    @State private var bodies: [GraphBody] = []
    @State private var bodyID: String = ""
    @State private var nodes: [GraphNode] = []
    @State private var links: [GraphLink] = []
    @State private var offset: CGSize = .zero
    @State private var zoom: CGFloat = 1
    @State private var panStart: CGSize?
    @State private var zoomStart: CGFloat?
    @State private var dragNode: String?
    @State private var dragOrigin: CGPoint = .zero
    @State private var wire: (node: String, pin: String, isExec: Bool, point: CGPoint)?
    @State private var editing: EditTarget?
    @State private var editText = ""
    @State private var selected: String?
    @State private var status: String?
    @State private var statusIsError = false
    @State private var loadedSource: String?
    @State private var showLarge = false

    enum EditTarget: Hashable, Identifiable {
        case pin(node: String, pin: String)
        case prop(node: String, key: String)
        var id: String {
            switch self {
            case let .pin(n, p): "pin:\(n):\(p)"
            case let .prop(n, k): "prop:\(n):\(k)"
            }
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            toolbar
            GeometryReader { geo in
                ZStack(alignment: .topLeading) {
                    Theme.field
                    Canvas { ctx, size in drawGrid(ctx, size); drawWires(ctx) }
                        .contentShape(Rectangle())
                        .gesture(panGesture)
                        .onTapGesture { selected = nil }
                    ForEach(nodes) { node in
                        nodeView(node)
                            .frame(width: GL.nodeWidth, height: GL.height(node), alignment: .topLeading)
                            .scaleEffect(zoom, anchor: .topLeading)
                            .offset(x: screenX(node.x), y: screenY(node.y))
                    }
                }
                .coordinateSpace(name: "graph")
                .clipped()
                .gesture(MagnifyGesture()
                    .onChanged { v in
                        if zoomStart == nil { zoomStart = zoom }
                        zoom = min(2.5, max(0.3, (zoomStart ?? 1) * v.magnification))
                    }
                    .onEnded { _ in zoomStart = nil })
                .onAppear { if bodies.isEmpty { load() } else { fit(geo.size) } }
                .onChange(of: source) { _, _ in if loadedSource != source { load() } }
                .onChange(of: bodyID) { _, _ in selectBody(); fit(geo.size) }
            }
            if let status {
                Text(status).font(Theme.label)
                    .foregroundStyle(statusIsError ? Theme.error : Theme.textDim)
                    .lineLimit(3).frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 8).padding(.vertical, 4)
                    .background(Theme.panel)
            }
        }
        .sheet(item: $editing) { target in editSheet(target) }
        .sheet(isPresented: $showLarge) {
            VStack(spacing: 0) {
                BehaviorGraphView(entityID: entityID, behaviorName: behaviorName, source: source, compact: false)
                HStack {
                    Spacer()
                    Button("Done") { showLarge = false }.keyboardShortcut(.defaultAction)
                }.padding(8).background(Theme.panel)
            }
            .frame(minWidth: 1000, idealWidth: 1280, minHeight: 640, idealHeight: 820)
        }
    }

    // MARK: Toolbar

    private var toolbar: some View {
        HStack(spacing: 6) {
            Picker("", selection: $bodyID) {
                ForEach(bodies) { b in Text(b.title).tag(b.id) }
            }
            .labelsHidden().controlSize(.small).frame(maxWidth: compact ? 170 : 280)
            Menu {
                ForEach(paletteGroups, id: \.0) { group, entries in
                    Section(group.capitalized) {
                        ForEach(entries.indices, id: \.self) { i in
                            Button(entries[i]["title"].string ?? "") { addNode(entries[i]) }
                        }
                    }
                }
            } label: { Label("Add", systemImage: "plus") }
                .menuStyle(.borderlessButton).fixedSize().font(Theme.label)
                .help("Add a node from the palette")
            Spacer()
            Button { zoom = max(0.3, zoom / 1.2) } label: { Image(systemName: "minus.magnifyingglass") }
                .buttonStyle(.borderless).help("Zoom out")
            Text("\(Int(zoom * 100))%").font(Theme.monoSmall).foregroundStyle(Theme.textFaint).frame(width: 38)
            Button { zoom = min(2.5, zoom * 1.2) } label: { Image(systemName: "plus.magnifyingglass") }
                .buttonStyle(.borderless).help("Zoom in")
            Button { load() } label: { Image(systemName: "arrow.clockwise") }
                .buttonStyle(.borderless).help("Reload from code")
            if compact {
                Button { showLarge = true } label: { Image(systemName: "arrow.up.left.and.arrow.down.right") }
                    .buttonStyle(.borderless).help("Open the graph in a large editor")
            }
        }
        .font(.system(size: 11))
        .padding(.horizontal, 6)
        .frame(height: 26)
        .background(Theme.header)
    }

    private var paletteGroups: [(String, [JSON])] {
        var groups: [String: [JSON]] = [:]
        var order: [String] = []
        for p in palette {
            let g = p["group"].string ?? "other"
            if groups[g] == nil { order.append(g) }
            groups[g, default: []].append(p)
        }
        return order.map { ($0, groups[$0] ?? []) }
    }

    // MARK: Coordinates

    private func screenX(_ x: Double) -> CGFloat { CGFloat(x) * zoom + offset.width }
    private func screenY(_ y: Double) -> CGFloat { CGFloat(y) * zoom + offset.height }

    private func pinPoint(_ node: GraphNode, pin: String, output: Bool) -> CGPoint {
        let pins = output ? node.outputs : node.inputs
        let i = pins.firstIndex(where: { $0.name == pin }) ?? 0
        let x = output ? node.x + Double(GL.nodeWidth) : node.x
        let y = node.y + Double(GL.header) + (Double(i) + 0.5) * Double(GL.row)
        return CGPoint(x: screenX(x), y: screenY(y))
    }

    private func fit(_ size: CGSize) {
        guard !nodes.isEmpty, size.width > 0 else { return }
        let minX = nodes.map(\.x).min() ?? 0
        let minY = nodes.map(\.y).min() ?? 0
        let maxX = nodes.map { $0.x + Double(GL.nodeWidth) }.max() ?? 1
        let maxY = nodes.map { $0.y + Double(GL.height($0)) }.max() ?? 1
        let z = min(1.2, max(0.3, min(size.width / CGFloat(maxX - minX + 60), size.height / CGFloat(maxY - minY + 60))))
        zoom = z
        offset = CGSize(width: -CGFloat(minX) * z + 30, height: -CGFloat(minY) * z + 30)
    }

    // MARK: Drawing

    private func drawGrid(_ ctx: GraphicsContext, _ size: CGSize) {
        let step = 24 * zoom
        guard step > 6 else { return }
        var path = Path()
        var x = offset.width.truncatingRemainder(dividingBy: step)
        while x < size.width {
            path.move(to: CGPoint(x: x, y: 0))
            path.addLine(to: CGPoint(x: x, y: size.height))
            x += step
        }
        var y = offset.height.truncatingRemainder(dividingBy: step)
        while y < size.height {
            path.move(to: CGPoint(x: 0, y: y))
            path.addLine(to: CGPoint(x: size.width, y: y))
            y += step
        }
        ctx.stroke(path, with: .color(Color.white.opacity(0.035)), lineWidth: 1)
    }

    private func wirePath(_ a: CGPoint, _ b: CGPoint) -> Path {
        var p = Path()
        p.move(to: a)
        let dx = max(40, abs(b.x - a.x) * 0.5)
        p.addCurve(to: b, control1: CGPoint(x: a.x + dx, y: a.y), control2: CGPoint(x: b.x - dx, y: b.y))
        return p
    }

    private func drawWires(_ ctx: GraphicsContext) {
        let byID = Dictionary(uniqueKeysWithValues: nodes.map { ($0.id, $0) })
        for l in links {
            guard let a = byID[l.from], let b = byID[l.to] else { continue }
            let exec = a.outputs.first(where: { $0.name == l.out })?.isExec ?? false
            ctx.stroke(wirePath(pinPoint(a, pin: l.out, output: true), pinPoint(b, pin: l.input, output: false)),
                       with: .color(exec ? Color.white.opacity(0.85) : Theme.accent.opacity(0.9)), lineWidth: exec ? 2 : 1.5)
        }
        if let w = wire, let a = byID[w.node] {
            ctx.stroke(wirePath(pinPoint(a, pin: w.pin, output: true), w.point),
                       with: .color(Theme.ai), style: StrokeStyle(lineWidth: 1.5, dash: [5, 3]))
        }
    }

    // MARK: Nodes

    private func nodeView(_ node: GraphNode) -> some View {
        let linkedInputs = Set(links.filter { $0.to == node.id }.map(\.input))
        return VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 4) {
                Text(node.header).font(.system(size: 10.5, weight: .semibold)).lineLimit(1)
                Spacer(minLength: 0)
                if !node.comment.isEmpty {
                    Image(systemName: "text.bubble").font(.system(size: 8.5)).foregroundStyle(Theme.textDim)
                        .help(node.comment.joined(separator: "\n"))
                }
            }
            .padding(.horizontal, 6)
            .frame(height: GL.header)
            .background(groupColor(node.group))
            .contentShape(Rectangle())
            .gesture(nodeDrag(node))
            .contextMenu { nodeMenu(node) }
            HStack(alignment: .top, spacing: 0) {
                VStack(alignment: .leading, spacing: 0) {
                    ForEach(node.inputs, id: \.name) { pin in
                        inputRow(node, pin, linked: linkedInputs.contains(pin.name))
                    }
                }
                Spacer(minLength: 4)
                VStack(alignment: .trailing, spacing: 0) {
                    ForEach(node.outputs, id: \.name) { pin in outputRow(node, pin) }
                }
            }
            .padding(.vertical, 3)
        }
        .background(Theme.panelRaised.opacity(0.97), in: RoundedRectangle(cornerRadius: 4))
        .overlay(RoundedRectangle(cornerRadius: 4).stroke(selected == node.id ? Theme.selection : Theme.border,
                                                         lineWidth: selected == node.id ? 1.5 : 1))
        .onTapGesture { selected = node.id }
    }

    private func pinShape(_ pin: GraphPin, filled: Bool) -> some View {
        Group {
            if pin.isExec {
                Image(systemName: filled ? "play.fill" : "play").font(.system(size: 8)).foregroundStyle(Color.white.opacity(0.9))
            } else {
                Circle().strokeBorder(Theme.accent, lineWidth: 1.5)
                    .background(Circle().fill(filled ? Theme.accent : Color.clear))
            }
        }
        .frame(width: GL.pin, height: GL.pin)
    }

    private func inputRow(_ node: GraphNode, _ pin: GraphPin, linked: Bool) -> some View {
        HStack(spacing: 4) {
            pinShape(pin, filled: linked)
            if !pin.isExec || pin.name != "in" {
                Text(pin.name).font(.system(size: 9.5)).foregroundStyle(Theme.textDim).lineLimit(1)
            }
            if !pin.isExec && !linked {
                Button {
                    editText = pin.value ?? ""
                    editing = .pin(node: node.id, pin: pin.name)
                } label: {
                    Text(pin.value.map { $0.isEmpty ? "…" : $0 } ?? "…")
                        .font(.system(size: 9.5, design: .monospaced)).lineLimit(1)
                        .padding(.horizontal, 3).frame(maxWidth: 104, alignment: .leading)
                        .background(Theme.field, in: RoundedRectangle(cornerRadius: 2))
                }
                .buttonStyle(.plain)
                .help("Edit the value (any Wander expression)")
            }
        }
        .frame(height: GL.row)
        .padding(.leading, 4)
        .contextMenu {
            if linked { Button("Disconnect") { disconnect(node.id, input: pin.name) } }
        }
    }

    private func outputRow(_ node: GraphNode, _ pin: GraphPin) -> some View {
        HStack(spacing: 4) {
            if !(pin.isExec && (pin.name == "next" || pin.name == "then") && node.outputs.count == 1) || node.type == "entry" {
                Text(pin.name).font(.system(size: 9.5)).foregroundStyle(Theme.textDim).lineLimit(1)
            }
            pinShape(pin, filled: links.contains { $0.from == node.id && $0.out == pin.name })
                .gesture(DragGesture(coordinateSpace: .named("graph"))
                    .onChanged { v in
                        wire = (node.id, pin.name, pin.isExec, v.location)
                    }
                    .onEnded { v in
                        connect(from: node.id, pin: pin.name, isExec: pin.isExec, at: v.location)
                        wire = nil
                    })
        }
        .frame(height: GL.row)
        .padding(.trailing, 4)
    }

    @ViewBuilder private func nodeMenu(_ node: GraphNode) -> some View {
        let editable = node.props.members.filter { k, v in v.string != nil || v.number != nil }
        ForEach(editable.indices, id: \.self) { i in
            let key = editable[i].0
            Button("Edit \(key)…") {
                editText = editable[i].1.string ?? editable[i].1.number.map { String($0) } ?? ""
                editing = .prop(node: node.id, key: key)
            }
        }
        if node.type != "entry" {
            Divider()
            Button("Delete Node", role: .destructive) { deleteNode(node.id) }
        }
    }

    private func editSheet(_ target: EditTarget) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            switch target {
            case let .pin(_, pin): Text("Value of \(pin)").font(Theme.sectionTitle)
            case let .prop(_, key): Text("Property \(key)").font(Theme.sectionTitle)
            }
            TextField("", text: $editText).font(Theme.mono).textFieldStyle(.roundedBorder).frame(minWidth: 320)
                .onSubmit { commitEdit(target) }
            Text("Any Wander expression: 3, \"text\", #ff8800, (0, 1, 0), self.position, speed * dt")
                .font(Theme.label).foregroundStyle(Theme.textFaint)
            HStack {
                Spacer()
                Button("Cancel") { editing = nil }.keyboardShortcut(.cancelAction)
                Button("Apply") { commitEdit(target) }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(14)
    }

    // MARK: Gestures

    private var panGesture: some Gesture {
        DragGesture(minimumDistance: 2)
            .onChanged { v in
                if panStart == nil { panStart = offset }
                offset = CGSize(width: (panStart?.width ?? 0) + v.translation.width,
                                height: (panStart?.height ?? 0) + v.translation.height)
            }
            .onEnded { _ in panStart = nil }
    }

    private func nodeDrag(_ node: GraphNode) -> some Gesture {
        DragGesture(minimumDistance: 1)
            .onChanged { v in
                guard let i = nodes.firstIndex(where: { $0.id == node.id }) else { return }
                if dragNode != node.id {
                    dragNode = node.id
                    dragOrigin = CGPoint(x: nodes[i].x, y: nodes[i].y)
                    selected = node.id
                }
                nodes[i].x = Double(dragOrigin.x + v.translation.width)
                nodes[i].y = Double(dragOrigin.y + v.translation.height)
            }
            .onEnded { _ in
                dragNode = nil
                commit(reason: "moved")
            }
    }

    // MARK: Editing

    private func connect(from: String, pin: String, isExec: Bool, at point: CGPoint) {
        // Nearest compatible input pin under the drop point.
        var best: (String, String, CGFloat)?
        for n in nodes where n.id != from {
            for p in n.inputs where p.isExec == isExec {
                let pp = pinPoint(n, pin: p.name, output: false)
                let d = hypot(pp.x - point.x, pp.y - point.y)
                if d < 18 * max(zoom, 0.6), d < (best?.2 ?? .infinity) { best = (n.id, p.name, d) }
            }
        }
        guard let (to, input, _) = best else { return }
        if isExec {
            links.removeAll { $0.from == from && $0.out == pin }  // an exec output continues in one place
        }
        links.removeAll { $0.to == to && $0.input == input && !isExec }  // a data input has one source
        links.append(GraphLink(from: from, out: pin, to: to, input: input))
        commit(reason: "connected")
    }

    private func disconnect(_ node: String, input: String) {
        links.removeAll { $0.to == node && $0.input == input }
        commit(reason: "disconnected")
    }

    private func deleteNode(_ id: String) {
        nodes.removeAll { $0.id == id }
        links.removeAll { $0.from == id || $0.to == id }
        commit(reason: "deleted")
    }

    private func addNode(_ entry: JSON) {
        let count = nodes.count
        let id = "\(bodyID)/n\(Int(Date().timeIntervalSince1970 * 1000) % 100_000_000)"
        var n = GraphNode(json: [
            "id": .string(id), "type": entry["type"], "props": entry["props"],
            "inputs": entry["inputs"], "outputs": entry["outputs"],
        ])
        n.x = Double((-offset.width + 80) / zoom) + Double(count % 4) * 24
        n.y = Double((-offset.height + 60) / zoom) + Double(count % 4) * 24
        nodes.append(n)
        selected = id
        commit(reason: "added \(entry["title"].string ?? "node")")
    }

    private func commitEdit(_ target: EditTarget) {
        switch target {
        case let .pin(node, pin):
            if let i = nodes.firstIndex(where: { $0.id == node }), let k = nodes[i].inputs.firstIndex(where: { $0.name == pin }) {
                nodes[i].inputs[k].value = editText
            }
        case let .prop(node, key):
            if let i = nodes.firstIndex(where: { $0.id == node }) {
                let old = nodes[i].props[key]
                let value: JSON = old.number != nil ? (Double(editText).map { .number($0) } ?? .string(editText)) : .string(editText)
                nodes[i].props.set(key, value)
            }
        }
        editing = nil
        commit(reason: "edited")
    }

    // MARK: Engine round trip

    private func load() {
        let r = engine.call("behavior_graph", ["entity": .number(Double(entityID)), "name": .string(behaviorName), "palette": true],
                            actor: "editor")
        loadedSource = source
        if r.isError {
            status = r.text.isEmpty ? "The behavior does not parse; fix the code to see its graph." : r.text
            statusIsError = true
            return
        }
        var g = r.structured
        palette = g["palette"].array
        g.set("palette", .null)
        graph = g
        bodies = collectBodies(g)
        if !bodies.contains(where: { $0.id == bodyID }) { bodyID = bodies.first?.id ?? "" }
        selectBody()
        status = nil
    }

    private func collectBodies(_ g: JSON) -> [GraphBody] {
        var out: [GraphBody] = []
        func add(_ body: JSON, _ path: [GraphPathKey], _ prefix: String) {
            out.append(GraphBody(id: body["id"].string ?? UUID().uuidString, title: prefix + (body["title"].string ?? "?"), path: path))
        }
        for (bi, b) in g["behaviors"].array.enumerated() {
            let name = (b["implicit"].bool ?? false) ? "" : "\(b["name"].string ?? ""): "
            for (i, h) in b["handlers"].array.enumerated() { add(h, [.key("behaviors"), .index(bi), .key("handlers"), .index(i)], name) }
            for (si, s) in b["states"].array.enumerated() {
                for (i, h) in s["handlers"].array.enumerated() {
                    add(h, [.key("behaviors"), .index(bi), .key("states"), .index(si), .key("handlers"), .index(i)],
                        name + "\(s["name"].string ?? "state") ▸ ")
                }
            }
            for (i, f) in b["functions"].array.enumerated() { add(f, [.key("behaviors"), .index(bi), .key("functions"), .index(i)], name) }
            for (i, t) in b["tests"].array.enumerated() { add(t, [.key("behaviors"), .index(bi), .key("tests"), .index(i)], name) }
        }
        for (i, f) in g["functions"].array.enumerated() { add(f, [.key("functions"), .index(i)], "") }
        for (i, t) in g["tests"].array.enumerated() { add(t, [.key("tests"), .index(i)], "") }
        return out
    }

    private func selectBody() {
        guard let b = bodies.first(where: { $0.id == bodyID }) else {
            nodes = []
            links = []
            return
        }
        let body = graph.at(b.path)
        nodes = body["nodes"].array.map(GraphNode.init(json:))
        links = body["links"].array.map {
            GraphLink(from: $0["from"].string ?? "", out: $0["out"].string ?? "", to: $0["to"].string ?? "", input: $0["in"].string ?? "")
        }
    }

    /// Writes the edited body into the graph and regenerates the behavior's code.
    private func commit(reason: String) {
        guard let b = bodies.first(where: { $0.id == bodyID }) else { return }
        var body = graph.at(b.path)
        body.set("nodes", .array(nodes.map(\.json)))
        body.set("links", .array(links.map(\.json)))
        graph.update(b.path, body)
        let r = engine.call("behavior_from_graph", ["graph": graph, "entity": .number(Double(entityID)), "name": .string(behaviorName)],
                            actor: "editor")
        if r.isError {
            let diags = r.structured["diagnostics"].array.filter { $0["severity"].string == "error" }
            if let d = diags.first {
                status = "Not saved: line \(d["line"].int ?? 0): \(d["message"].string ?? "") — fix the graph or the code."
            } else {
                status = "Not saved: " + r.text
            }
            statusIsError = true
            return
        }
        loadedSource = r.structured["source"].string
        status = "Saved (\(reason)); the code was regenerated."
        statusIsError = false
    }
}
