import SwiftUI

enum DockTab: String, CaseIterable, Identifiable {
    case assets = "Assets", console = "Console", activity = "Activity", agents = "Agents", studio = "Studio"
    var id: String { rawValue }
    var symbol: String {
        switch self {
        case .assets: "shippingbox"
        case .console: "terminal"
        case .activity: "clock.arrow.circlepath"
        case .agents: "cloud"
        case .studio: "person.3.sequence"
        }
    }
}

/// Window layout:  toolbar / [outliner | viewport + dock | details] / status bar.
/// Panels are custom (not NavigationSplitView) so the layout is dense, predictable and
/// resizable like a DCC tool's.
struct EditorView: View {
    @Environment(EngineStore.self) private var engine
    @Environment(CrewStore.self) private var crew
    @AppStorage("layout.left") private var leftWidth: Double = 250
    @AppStorage("layout.right") private var rightWidth: Double = 330
    @AppStorage("layout.bottom") private var bottomHeight: Double = 230
    @AppStorage("layout.showLeft") private var showLeft = true
    @AppStorage("layout.showRight") private var showRight = true
    @AppStorage("layout.showBottom") private var showBottom = true
    @AppStorage("layout.dockTab") private var dockTab: DockTab = .assets
    @State private var focusedAgent: String?

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 0) {
                if showLeft {
                    OutlinerPanel().frame(width: leftWidth)
                    SplitHandle(axis: .vertical, value: $leftWidth, range: 180...480, invert: false)
                }
                VStack(spacing: 0) {
                    ViewportPanel()
                    if showBottom {
                        SplitHandle(axis: .horizontal, value: $bottomHeight, range: 120...600, invert: true)
                        dock.frame(height: bottomHeight)
                    }
                }
                .frame(minWidth: 360)
                if showRight {
                    SplitHandle(axis: .vertical, value: $rightWidth, range: 260...560, invert: true)
                    DetailsPanel().frame(width: rightWidth)
                }
            }
            StatusBar()
        }
        .background(Theme.window)
        .foregroundStyle(Theme.text)
        .toolbar { toolbar }
        .navigationTitle(engine.sceneName)
        .navigationSubtitle(engine.playState == "editing" ? "Skywalker" : engine.playState.capitalized)
        .onChange(of: crew.workingCount) { old, new in
            if new > old, dockTab != .agents { focusedAgent = crew.cloudlings.first(where: crew.isWorking)?.id }
        }
    }

    private var dock: some View {
        VStack(spacing: 0) {
            PanelTabs(tabs: DockTab.allCases, selection: $dockTab, title: { $0.rawValue }, icon: { $0.symbol })
            switch dockTab {
            case .assets: AssetBrowser()
            case .console: ConsolePanel()
            case .activity: ActivityPanel()
            case .agents: AgentsPanel(focused: $focusedAgent)
            case .studio: StudioPanel(openAgent: { id in focusedAgent = id; dockTab = .agents })
            }
        }
        .background(Theme.panel)
    }

    @ToolbarContentBuilder
    private var toolbar: some ToolbarContent {
        ToolbarItemGroup(placement: .navigation) {
            SkywalkerMark(size: 14).padding(.horizontal, 4)
            ToolPicker()
        }
        ToolbarItemGroup(placement: .principal) {
            PlayControls()
        }
        ToolbarItemGroup(placement: .primaryAction) {
            CrewPresence { agent in
                focusedAgent = agent
                dockTab = .agents
                showBottom = true
            }
            Button("Undo", systemImage: "arrow.uturn.backward") { engine.call("history", ["action": "undo"]) }
                .help("Undo (⌘Z)")
            Button("Redo", systemImage: "arrow.uturn.forward") { engine.call("history", ["action": "redo"]) }
                .help("Redo (⇧⌘Z)")
            ControlGroup {
                Toggle("Outliner", systemImage: "sidebar.left", isOn: $showLeft)
                Toggle("Dock", systemImage: "rectangle.bottomthird.inset.filled", isOn: $showBottom)
                Toggle("Details", systemImage: "sidebar.right", isOn: $showRight)
            }
            .help("Show or hide panels")
        }
    }
}

/// Q/W/E/R transform tools + space + snapping.
struct ToolPicker: View {
    @Environment(EngineStore.self) private var engine

    var body: some View {
        @Bindable var engine = engine
        HStack(spacing: 6) {
            Picker("Tool", selection: $engine.gizmoMode) {
                ForEach(GizmoMode.allCases) { mode in
                    Image(systemName: mode.symbol).help(mode.title).tag(mode)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .fixedSize()
            Picker("Space", selection: $engine.gizmoLocal) {
                Text("World").tag(false)
                Text("Local").tag(true)
            }
            .pickerStyle(.menu)
            .labelsHidden()
            .fixedSize()
            .help("Gizmo space")
            Toggle(isOn: $engine.snapping) { Image(systemName: "squareshape.split.3x3") }
                .toggleStyle(.button)
                .help("Snap (\(engine.snapStep, specifier: "%g") m / 15°). Hold ⌃ to snap temporarily.")
        }
    }
}

struct PlayControls: View {
    @Environment(EngineStore.self) private var engine

    var body: some View {
        let state = engine.playState
        HStack(spacing: 2) {
            Button { engine.call("sim_control", ["action": state == "playing" ? "pause" : "play"]) } label: {
                Image(systemName: state == "playing" ? "pause.fill" : "play.fill")
                    .foregroundStyle(state == "editing" ? Theme.text : Theme.success)
            }
            .help(state == "playing" ? "Pause (⇧⌘P)" : "Play (⌘P)")
            Button { engine.call("sim_control", ["action": "step", "ticks": 1]) } label: {
                Image(systemName: "forward.frame.fill")
            }
            .help("Step one tick (1/60 s)")
            Button { engine.call("sim_control", ["action": "stop"]) } label: {
                Image(systemName: "stop.fill").foregroundStyle(state == "editing" ? Theme.textFaint : Theme.error)
            }
            .disabled(state == "editing")
            .help("Stop and restore the scene (⌘.)")
        }
    }
}

/// Collaborator-style presence: the crew as small overlapping avatars; working agents
/// carry a pulsing dot. Click one to open its chat.
struct CrewPresence: View {
    @Environment(CrewStore.self) private var crew
    @Environment(EngineStore.self) private var engine
    let open: (String) -> Void

    var body: some View {
        HStack(spacing: -7) {
            ForEach(crew.cloudlings) { c in
                Button { open(c.id) } label: {
                    CloudAvatar(color: c.color, face: c.face, size: 15, working: crew.isWorking(c),
                                dimmed: !crew.isWorking(c) && crew.workingCount > 0)
                        .padding(2)
                        .background(Circle().fill(Theme.panelRaised).frame(width: 26, height: 26))
                }
                .buttonStyle(.plain)
                .help("\(c.name) — \(c.role.title)\(crew.isWorking(c) ? " · working" : "")")
            }
        }
        .padding(.horizontal, 4)
        .overlay(alignment: .topTrailing) {
            if !crew.pendingApprovals.isEmpty {
                Text("\(crew.pendingApprovals.count)")
                    .font(.system(size: 9, weight: .bold))
                    .padding(.horizontal, 4)
                    .background(Capsule().fill(Theme.warning))
                    .foregroundStyle(.black)
                    .offset(x: 4, y: -4)
                    .help("Agent actions waiting for your approval")
            }
        }
    }
}

/// Draggable divider between panes.
struct SplitHandle: View {
    enum Axis { case vertical, horizontal }
    let axis: Axis
    @Binding var value: Double
    let range: ClosedRange<Double>
    /// true when dragging toward the origin grows the pane (right / bottom panes).
    let invert: Bool
    @State private var start: Double?
    @State private var hovering = false

    var body: some View {
        Rectangle()
            .fill(hovering || start != nil ? Theme.accent.opacity(0.6) : Theme.border)
            .frame(width: axis == .vertical ? 1 : nil, height: axis == .horizontal ? 1 : nil)
            .padding(axis == .vertical ? .horizontal : .vertical, 2)
            .contentShape(Rectangle())
            .onHover { inside in
                hovering = inside
                if inside { (axis == .vertical ? NSCursor.resizeLeftRight : NSCursor.resizeUpDown).push() } else { NSCursor.pop() }
            }
            .gesture(DragGesture(minimumDistance: 1, coordinateSpace: .global)
                .onChanged { g in
                    let s = start ?? value
                    start = s
                    let delta = axis == .vertical ? g.translation.width : g.translation.height
                    value = min(max(s + (invert ? -delta : delta), range.lowerBound), range.upperBound)
                }
                .onEnded { _ in start = nil })
            .padding(axis == .vertical ? .horizontal : .vertical, -2)
    }
}

/// The viewport with its overlay controls.
struct ViewportPanel: View {
    @Environment(EngineStore.self) private var engine
    @AppStorage("viewport.stats") private var showStats = true

    var body: some View {
        @Bindable var engine = engine
        ZStack {
            ViewportView(engine: engine)
            VStack {
                HStack(alignment: .top) {
                    HStack(spacing: 2) {
                        Menu {
                            Picker("Camera", selection: $engine.viewSceneCamera) {
                                Label("Editor Camera", systemImage: "camera.metering.center.weighted").tag(false)
                                Label("Game Camera", systemImage: "video").tag(true)
                            }
                            .pickerStyle(.inline)
                            Divider()
                            Button("Frame Selection  F") {
                                if let id = engine.selection.first { engine.call("camera_set", ["frame": .number(Double(id))]) }
                            }
                            Button("Frame All") { engine.call("camera_set", ["frame": "all"]) }
                        } label: {
                            Label(engine.viewSceneCamera ? "Game Camera" : "Perspective", systemImage: "camera")
                                .font(Theme.label)
                        }
                        .menuStyle(.borderlessButton)
                        .fixedSize()
                        .padding(.horizontal, 6)
                        Divider().frame(height: 14)
                        IconButton(symbol: "number", help: "Toggle grid", active: gridOn) {
                            engine.call("environment_update", ["showGrid": .bool(!gridOn)])
                        }
                        IconButton(symbol: "chart.bar.xaxis", help: "Toggle stats", active: showStats) { showStats.toggle() }
                    }
                    .padding(3)
                    .background(Theme.panel.opacity(0.88), in: RoundedRectangle(cornerRadius: 6))
                    .overlay(RoundedRectangle(cornerRadius: 6).stroke(Theme.border))
                    .environment(\.colorScheme, .dark)
                    Spacer()
                    if showStats { StatsOverlay() }
                }
                Spacer()
                HStack(alignment: .bottom) {
                    AxisIndicator(yaw: engine.cameraYaw, pitch: engine.cameraPitch)
                    Spacer()
                    ActivityToast()
                }
            }
            .padding(8)
            .allowsHitTesting(true)
        }
        .overlay {
            if engine.playState != "editing" {
                RoundedRectangle(cornerRadius: 0).strokeBorder(Theme.success.opacity(0.7), lineWidth: 2).allowsHitTesting(false)
            }
        }
        .clipped()
    }

    private var gridOn: Bool { engine.environment["showGrid"].bool ?? true }
}

struct StatsOverlay: View {
    @Environment(EngineStore.self) private var engine
    var body: some View {
        let s = engine.stats
        VStack(alignment: .trailing, spacing: 1) {
            Text("\(Int(s.fps.rounded())) fps · \(s.cpuMs, specifier: "%.1f") ms")
            Text("\(s.draws) draws · \(s.entities) entities")
            Text(s.renderer).foregroundStyle(Theme.textFaint)
        }
        .font(Theme.monoSmall)
        .foregroundStyle(Theme.text)
        .padding(.horizontal, 7).padding(.vertical, 4)
        .background(Theme.panel.opacity(0.82), in: RoundedRectangle(cornerRadius: 6))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(Theme.border))
        .allowsHitTesting(false)
    }
}

/// Small XYZ axis indicator reflecting the editor camera orientation.
struct AxisIndicator: View {
    let yaw: Float
    let pitch: Float

    var body: some View {
        Canvas { ctx, size in
            let c = CGPoint(x: size.width / 2, y: size.height / 2)
            let r = min(size.width, size.height) * 0.38
            let y = Double(yaw) * .pi / 180, p = Double(pitch) * .pi / 180
            // camera basis from orbit angles (eye = target + (cos p sin y, sin p, cos p cos y))
            func project(_ v: SIMD3<Double>) -> (CGPoint, Double) {
                let right = SIMD3(cos(y), 0, -sin(y))
                let fwd = -SIMD3(cos(p) * sin(y), sin(p), cos(p) * cos(y))
                let up = SIMD3(right.y * fwd.z - right.z * fwd.y, right.z * fwd.x - right.x * fwd.z, right.x * fwd.y - right.y * fwd.x)
                let sx = (v * right).sum(), sy = (v * up).sum(), depth = (v * fwd).sum()
                return (CGPoint(x: c.x + sx * r, y: c.y - sy * r), depth)
            }
            let axes: [(SIMD3<Double>, Color, String)] = [
                (SIMD3(1, 0, 0), Theme.axisX, "X"), (SIMD3(0, 1, 0), Theme.axisY, "Y"), (SIMD3(0, 0, 1), Theme.axisZ, "Z"),
            ]
            for (v, color, name) in axes.sorted(by: { project($0.0).1 > project($1.0).1 }) {
                let (pt, _) = project(v)
                var path = Path()
                path.move(to: c)
                path.addLine(to: pt)
                ctx.stroke(path, with: .color(color), lineWidth: 1.6)
                ctx.fill(Path(ellipseIn: CGRect(x: pt.x - 5.5, y: pt.y - 5.5, width: 11, height: 11)), with: .color(color))
                ctx.draw(Text(name).font(.system(size: 7.5, weight: .bold)).foregroundStyle(.black), at: pt)
            }
        }
        .frame(width: 54, height: 54)
        .allowsHitTesting(false)
    }
}

/// Transient bubble showing the latest agent action over the viewport.
struct ActivityToast: View {
    @Environment(EngineStore.self) private var engine
    @Environment(CrewStore.self) private var crew

    var body: some View {
        TimelineView(.periodic(from: .now, by: 0.5)) { context in
            if let item = engine.activity.last(where: { $0.actor != "user" && $0.actor != "editor" }),
               context.date.timeIntervalSince(item.date) < 4 {
                HStack(spacing: 6) {
                    if let c = crew.cloudling(actor: item.actor) {
                        CloudAvatar(color: c.color, face: c.face, size: 13, working: true)
                        Text(c.name).fontWeight(.semibold)
                    } else {
                        Image(systemName: "link")
                        Text(item.actor).fontWeight(.semibold)
                    }
                    Text(item.kind == "tool" ? item.text : item.text).foregroundStyle(Theme.textDim).lineLimit(1)
                }
                .font(Theme.label)
                .foregroundStyle(Theme.text)
                .padding(.horizontal, 8).padding(.vertical, 4)
                .background(Theme.panel.opacity(0.88), in: Capsule())
                .overlay(Capsule().stroke(Theme.border))
            }
        }
        .allowsHitTesting(false)
    }
}

struct StatusBar: View {
    @Environment(EngineStore.self) private var engine
    @Environment(CrewStore.self) private var crew

    var body: some View {
        HStack(spacing: 14) {
            HStack(spacing: 5) {
                Circle().fill(stateColor).frame(width: 6, height: 6)
                Text(engine.playState.capitalized)
            }
            Text(selectionText).foregroundStyle(Theme.textDim)
            Spacer()
            if let last = engine.activity.last {
                Text("\(displayActor(last.actor)): \(last.text)").foregroundStyle(Theme.textFaint).lineLimit(1)
            }
            Spacer()
            if crew.workingCount > 0 {
                Text("\(crew.workingCount) agent\(crew.workingCount == 1 ? "" : "s") working").foregroundStyle(Theme.ai)
            }
            Button {
                engine.setAgentServer(enabled: !engine.agentServerRunning)
            } label: {
                HStack(spacing: 4) {
                    Circle().fill(engine.agentServerRunning ? Theme.success : Theme.textFaint).frame(width: 6, height: 6)
                    Text(engine.agentServerRunning ? "MCP listening" : "MCP off")
                }
            }
            .buttonStyle(.plain)
            .help(engine.agentServerRunning ? "External agents can connect via \(EngineStore.socketPath). Click to stop."
                                            : "Click to let external agents (Claude Code, Codex, …) connect")
            Text(engine.stats.renderer).foregroundStyle(Theme.textFaint)
        }
        .font(.system(size: 10.5))
        .padding(.horizontal, 10)
        .frame(height: 22)
        .background(Theme.header)
        .overlay(alignment: .top) { Rectangle().fill(Theme.border).frame(height: 1) }
    }

    private var stateColor: Color {
        switch engine.playState {
        case "playing": Theme.success
        case "paused": Theme.warning
        default: Theme.textFaint
        }
    }

    private var selectionText: String {
        switch engine.selection.count {
        case 0: return "No selection"
        case 1:
            let id = engine.selection.first!
            return "Selected: \(engine.entities.first { $0.id == id }?.name ?? "#\(id)")"
        default: return "\(engine.selection.count) selected"
        }
    }

    private func displayActor(_ actor: String) -> String {
        if actor.hasPrefix("agent:") { return String(actor.dropFirst(6)) }
        if actor.hasPrefix("mcp:") { return String(actor.dropFirst(4)) }
        return actor == "user" ? "You" : actor
    }
}
