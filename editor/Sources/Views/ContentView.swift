import SwiftUI

enum BottomTab: String, CaseIterable, Identifiable {
    case crew = "Crew", plans = "Flight Plans", activity = "Activity", console = "Console"
    var id: String { rawValue }
    var symbol: String {
        switch self {
        case .crew: "cloud.fill"
        case .plans: "point.3.connected.trianglepath.dotted"
        case .activity: "list.bullet.rectangle"
        case .console: "terminal"
        }
    }
}

struct ContentView: View {
    @Environment(EngineStore.self) private var engine
    @Environment(CrewStore.self) private var crew
    @State private var showInspector = true
    @State private var tab: BottomTab = .crew
    @State private var focusedCloudling: UUID?
    @State private var bottomHeight: CGFloat = 280

    var body: some View {
        NavigationSplitView {
            HierarchyView()
                .navigationSplitViewColumnWidth(min: 200, ideal: 240)
        } detail: {
            VStack(spacing: 0) {
                CrewStrip(focused: Binding(get: { focusedCloudling }, set: { focusedCloudling = $0; tab = .crew }))
                Divider()
                ResizableSplit(bottomHeight: $bottomHeight) {
                    ZStack(alignment: .bottomLeading) {
                        ViewportView(engine: engine)
                            .frame(minHeight: 240)
                        ActivityToast()
                            .padding(10)
                        if engine.playState != "editing" {
                            Text(engine.playState == "playing" ? "▶︎ PLAYING" : "❚❚ PAUSED")
                                .font(.caption.weight(.bold)).padding(6)
                                .background(.ultraThinMaterial, in: Capsule())
                                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
                                .padding(10)
                        }
                    }
                } bottom: {
                    bottomPanel
                }
            }
            .inspector(isPresented: $showInspector) {
                InspectorView()
                    .inspectorColumnWidth(min: 260, ideal: 320, max: 480)
            }
        }
        .toolbar { toolbarContent }
        .navigationTitle(engine.sceneName)
        .onAppear { focusedCloudling = focusedCloudling ?? crew.cloudlings.first?.id }
    }

    private var bottomPanel: some View {
        VStack(spacing: 0) {
            Picker("Panel", selection: $tab) {
                ForEach(BottomTab.allCases) { t in Label(t.rawValue, systemImage: t.symbol).tag(t) }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .padding(6)
            Divider()
            switch tab {
            case .crew:
                if let c = crew.cloudlings.first(where: { $0.id == focusedCloudling }) ?? crew.cloudlings.first {
                    CrewChatView(cloudling: c).id(c.id)
                } else {
                    ContentUnavailableView("No crew yet", systemImage: "cloud", description: Text("Add Cloudlings in Settings."))
                }
            case .plans: FlightPlanView()
            case .activity: ActivityView()
            case .console: ConsoleView()
            }
        }
    }

    @ToolbarContentBuilder
    private var toolbarContent: some ToolbarContent {
        ToolbarItem(placement: .navigation) { SkywalkerLogo(size: 16) }
        ToolbarItemGroup(placement: .principal) {
            Button("Play", systemImage: "play.fill") { engine.call("sim_control", ["action": "play"]) }
                .disabled(engine.playState == "playing")
            Button("Pause", systemImage: "pause.fill") { engine.call("sim_control", ["action": "pause"]) }
                .disabled(engine.playState != "playing")
            Button("Step", systemImage: "forward.frame.fill") { engine.call("sim_control", ["action": "step", "ticks": 1]) }
            Button("Stop", systemImage: "stop.fill") { engine.call("sim_control", ["action": "stop"]) }
                .disabled(engine.playState == "editing")
        }
        ToolbarItemGroup(placement: .primaryAction) {
            Button("Undo", systemImage: "arrow.uturn.backward") { engine.call("history", ["action": "undo"]) }
            Button("Redo", systemImage: "arrow.uturn.forward") { engine.call("history", ["action": "redo"]) }
            Button("Agents", systemImage: engine.agentServerRunning ? "antenna.radiowaves.left.and.right" : "antenna.radiowaves.left.and.right.slash") {
                engine.setAgentServer(enabled: !engine.agentServerRunning)
            }
            .help(engine.agentServerRunning ? "External agents can connect (MCP). Click to stop." : "Allow external agents (Claude Code, Codex, …) to connect")
            Button("Inspector", systemImage: "sidebar.trailing") { showInspector.toggle() }
        }
    }
}

/// A transient bubble over the viewport showing the latest agent action — so you can
/// watch the crew work without opening the activity panel.
struct ActivityToast: View {
    @Environment(EngineStore.self) private var engine
    @Environment(CrewStore.self) private var crew

    var body: some View {
        TimelineView(.periodic(from: .now, by: 0.5)) { context in
            if let item = engine.activity.last(where: { $0.actor != "user" }),
               context.date.timeIntervalSince(item.date) < 4 {
                HStack(spacing: 8) {
                    if let c = crew.cloudling(actor: item.actor) {
                        CloudAvatar(color: c.color, face: c.face, size: 20, working: true)
                        Text("**\(c.name)** \(item.kind == "tool" ? "is using \(item.text)" : item.text)")
                    } else {
                        CloudAvatar(color: .gray, face: .curious, size: 20, working: true)
                        Text("**\(item.actor)** \(item.text)")
                    }
                }
                .font(.caption)
                .padding(.horizontal, 10).padding(.vertical, 6)
                .background(.ultraThinMaterial, in: Capsule())
                .transition(.opacity.combined(with: .move(edge: .bottom)))
            }
        }
        .animation(.easeOut, value: engine.activity.count)
    }
}

/// A vertical split with a draggable divider, in pure SwiftUI. (AppKit-backed VSplitView
/// nests hosting views whose safe-area updates can feed back into layout indefinitely.)
struct ResizableSplit<Top: View, Bottom: View>: View {
    @Binding var bottomHeight: CGFloat
    @ViewBuilder var top: Top
    @ViewBuilder var bottom: Bottom
    @State private var dragStart: CGFloat?

    var body: some View {
        GeometryReader { geo in
            VStack(spacing: 0) {
                top.frame(maxWidth: .infinity, maxHeight: .infinity)
                Rectangle()
                    .fill(.separator)
                    .frame(height: 1)
                    .padding(.vertical, 3)
                    .contentShape(Rectangle())
                    .onHover { inside in if inside { NSCursor.resizeUpDown.push() } else { NSCursor.pop() } }
                    .gesture(DragGesture(minimumDistance: 1)
                        .onChanged { value in
                            let start = dragStart ?? bottomHeight
                            dragStart = start
                            bottomHeight = min(max(start - value.translation.height, 120), geo.size.height - 200)
                        }
                        .onEnded { _ in dragStart = nil })
                bottom.frame(height: bottomHeight)
            }
        }
    }
}
