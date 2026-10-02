import AppKit
import SwiftUI

@main
struct SkywalkerApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @State private var engine: EngineStore
    @State private var crew: CrewStore

    init() {
        let engine = EngineStore(projectDirectory: Self.projectDirectory())
        _engine = State(initialValue: engine)
        _crew = State(initialValue: CrewStore(engine: engine))
    }

    var body: some Scene {
        Window("Skywalker", id: "main") {
            ContentView()
                .environment(engine)
                .environment(crew)
                .frame(minWidth: 1000, minHeight: 640)
                .onReceive(NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)) { _ in
                    crew.save()
                    engine.shutdown()
                }
        }
        .commands {
            CommandGroup(replacing: .undoRedo) {
                Button("Undo") { engine.call("history", ["action": "undo"]) }.keyboardShortcut("z")
                Button("Redo") { engine.call("history", ["action": "redo"]) }.keyboardShortcut("z", modifiers: [.command, .shift])
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
                .environment(crew)
        }
    }

    /// The project folder: $SKY_PROJECT, or ~/Documents/Skywalker/Hello Sky (created from the
    /// bundled template on first launch).
    static func projectDirectory() -> URL {
        if let env = ProcessInfo.processInfo.environment["SKY_PROJECT"], !env.isEmpty {
            return URL(filePath: env, directoryHint: .isDirectory)
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
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        // Allows running the bare executable (e.g. from the build directory) as a proper GUI app.
        NSApp.setActivationPolicy(.regular)
        NSApp.activate()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
