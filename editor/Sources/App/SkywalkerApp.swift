import AppKit
import SwiftUI

@main
struct SkywalkerApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @State private var engine: EngineStore
    @State private var studio: StudioStore
    @State private var crew: CrewStore

    init() {
        let engine = EngineStore(projectDirectory: Self.projectDirectory())
        let studio = StudioStore(engine: engine)
        _engine = State(initialValue: engine)
        _studio = State(initialValue: studio)
        _crew = State(initialValue: CrewStore(engine: engine, studio: studio))
    }

    var body: some Scene {
        Window("Skywalker", id: "main") {
            EditorView()
                .environment(engine)
                .environment(studio)
                .environment(crew)
                .frame(minWidth: 1100, minHeight: 680)
                .preferredColorScheme(.dark)
                .onReceive(NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)) { _ in
                    crew.save()
                    engine.shutdown()
                }
        }
        .windowToolbarStyle(.unified(showsTitle: true))
        .defaultSize(width: 1440, height: 880)
        .commands {
            CommandGroup(replacing: .undoRedo) {
                // Text fields keep their own undo; everywhere else ⌘Z undoes scene edits.
                Button("Undo") {
                    if let text = NSApp.keyWindow?.firstResponder as? NSTextView, let um = text.undoManager, um.canUndo {
                        um.undo()
                    } else {
                        engine.call("history", ["action": "undo"])
                    }
                }
                .keyboardShortcut("z")
                Button("Redo") {
                    if let text = NSApp.keyWindow?.firstResponder as? NSTextView, let um = text.undoManager, um.canRedo {
                        um.redo()
                    } else {
                        engine.call("history", ["action": "redo"])
                    }
                }
                .keyboardShortcut("z", modifiers: [.command, .shift])
            }
            CommandGroup(replacing: .newItem) {
                Button("Open Project…") { Self.chooseProject() }.keyboardShortcut("o")
            }
            CommandGroup(replacing: .saveItem) {
                Button("Save Scene") { engine.call("scene_save", ["path": "scenes/main.sky.json"]) }.keyboardShortcut("s")
            }
            CommandMenu("Game") {
                Button("Play") { engine.call("sim_control", ["action": "play"]) }.keyboardShortcut("p")
                Button("Pause") { engine.call("sim_control", ["action": "pause"]) }.keyboardShortcut("p", modifiers: [.command, .shift])
                Button("Stop") { engine.call("sim_control", ["action": "stop"]) }.keyboardShortcut(".")
                Divider()
                Button("Frame All") { engine.call("camera_set", ["frame": "all"]) }.keyboardShortcut("0")
            }
        }

        Settings {
            SettingsView()
                .environment(engine)
                .environment(studio)
                .environment(crew)
                .preferredColorScheme(.dark)
        }
    }

    static let lastProjectKey = "lastProjectDirectory"

    /// The project folder: `--project DIR`, $SKY_PROJECT, the last opened project, or
    /// ~/Documents/Skywalker/Hello Sky (created from the bundled template on first launch).
    static func projectDirectory() -> URL {
        let args = ProcessInfo.processInfo.arguments
        if let i = args.firstIndex(of: "--project"), i + 1 < args.count {
            return remember(URL(filePath: args[i + 1], directoryHint: .isDirectory))
        }
        if let env = ProcessInfo.processInfo.environment["SKY_PROJECT"], !env.isEmpty {
            return URL(filePath: env, directoryHint: .isDirectory)
        }
        if let last = UserDefaults.standard.string(forKey: lastProjectKey),
           FileManager.default.fileExists(atPath: last) {
            return URL(filePath: last, directoryHint: .isDirectory)
        }
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let project = docs.appending(path: "Skywalker/Hello Sky", directoryHint: .isDirectory)
        if !FileManager.default.fileExists(atPath: project.path),
           let template = Bundle.main.url(forResource: "hello_sky", withExtension: nil, subdirectory: "Templates") {
            try? FileManager.default.createDirectory(at: project.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? FileManager.default.copyItem(at: template, to: project)
        }
        try? FileManager.default.createDirectory(at: project, withIntermediateDirectories: true)
        return project
    }

    private static func remember(_ url: URL) -> URL {
        UserDefaults.standard.set(url.path, forKey: lastProjectKey)
        return url
    }

    /// Picks a project folder and relaunches the editor on it (each window owns one project).
    static func chooseProject() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = "Open"
        panel.message = "Choose a Skywalker project folder"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        _ = remember(url)
        let config = NSWorkspace.OpenConfiguration()
        config.createsNewApplicationInstance = true
        config.arguments = ["--project", url.path]
        NSWorkspace.shared.openApplication(at: Bundle.main.bundleURL, configuration: config) { _, _ in
            DispatchQueue.main.async { NSApp.terminate(nil) }
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        // Allows running the bare executable (e.g. from the build directory) as a proper GUI app.
        NSApp.setActivationPolicy(.regular)
        NSApp.activate()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
