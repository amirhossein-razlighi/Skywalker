import SwiftUI

struct SettingsView: View {
    var body: some View {
        TabView {
            Tab("Crew", systemImage: "cloud.fill") { CrewManager() }
            Tab("Providers", systemImage: "brain") { ProviderSettings() }
            Tab("External Agents", systemImage: "antenna.radiowaves.left.and.right") { ExternalAgentSettings() }
            Tab("Design Apps", systemImage: "wand.and.stars") { DesignAppSettings() }
        }
        .frame(width: 820, height: 620)
    }
}

private struct ProviderSettings: View {
    @Environment(CrewStore.self) private var crew
    @State private var selection: UUID?
    @State private var apiKey = ""

    var body: some View {
        @Bindable var crew = crew
        HSplitView {
            List(selection: $selection) {
                ForEach(crew.providers) { Text($0.name).tag($0.id) }
            }
            .frame(minWidth: 180, maxWidth: 220)
            .safeAreaInset(edge: .bottom) {
                Menu("Add Provider") {
                    ForEach(ProviderConfig.presets, id: \.name) { preset in
                        Button(preset.name) {
                            var p = preset
                            p.id = UUID()
                            crew.providers.append(p)
                            selection = p.id
                            crew.save()
                        }
                    }
                }
                .padding(6)
            }
            if let i = crew.providers.firstIndex(where: { $0.id == selection }) {
                Form {
                    TextField("Name", text: $crew.providers[i].name)
                    Picker("Protocol", selection: $crew.providers[i].kind) {
                        ForEach(ProviderKind.allCases) { Text($0.label).tag($0) }
                    }
                    TextField("Base URL", text: $crew.providers[i].baseURL)
                    TextField("Default model", text: $crew.providers[i].defaultModel)
                    Toggle("Model can see images (viewport screenshots)", isOn: $crew.providers[i].supportsVision)
                    SecureField("API key (stored in Keychain)", text: $apiKey)
                        .onSubmit { Keychain.set(apiKey, account: crew.providers[i].keychainAccount) }
                    Button("Save Key") { Keychain.set(apiKey, account: crew.providers[i].keychainAccount) }
                    Text("Local servers (Ollama, LM Studio, vLLM, llama.cpp) usually need no key.")
                        .font(.caption).foregroundStyle(.secondary)
                    Button("Remove Provider", role: .destructive) {
                        crew.providers.remove(at: i)
                        selection = nil
                        crew.save()
                    }
                }
                .formStyle(.grouped)
                .onAppear { apiKey = Keychain.get(account: crew.providers[i].keychainAccount) ?? "" }
                .onChange(of: selection) { _, _ in
                    if let j = crew.providers.firstIndex(where: { $0.id == selection }) {
                        apiKey = Keychain.get(account: crew.providers[j].keychainAccount) ?? ""
                    }
                }
                .onChange(of: crew.providers) { _, _ in crew.save() }
            } else {
                ContentUnavailableView("Select a provider", systemImage: "brain")
            }
        }
    }
}

private struct ExternalAgentSettings: View {
    @Environment(EngineStore.self) private var engine

    private var cliPath: String {
        Bundle.main.url(forAuxiliaryExecutable: "skywalker-cli")?.path ?? "/path/to/skywalker"
    }

    var body: some View {
        Form {
            Toggle("Allow external agents to connect to this editor", isOn: Binding(
                get: { engine.agentServerRunning }, set: { engine.setAgentServer(enabled: $0) }))
            LabeledContent("Socket", value: EngineStore.socketPath)
            Section("Claude Code") {
                CopyableCommand(command: "claude mcp add skywalker -- \"\(cliPath)\" mcp --attach")
            }
            Section("Any MCP client (Codex, Cursor, Gemini CLI, …)") {
                CopyableCommand(command: "{\"mcpServers\": {\"skywalker\": {\"command\": \"\(cliPath)\", \"args\": [\"mcp\", \"--attach\"]}}}")
                Text("Use `mcp` without `--attach` to run a separate headless engine instead of driving this editor.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }
}

/// Design apps agents can drive (Blender, Maya, Houdini, 3ds Max): what was detected on this
/// computer, and the live Blender bridge.
private struct DesignAppSettings: View {
    @Environment(EngineStore.self) private var engine
    @State private var apps: [JSON] = []
    @State private var session: JSON = .null
    @State private var busy = false
    @State private var message = ""

    var body: some View {
        Form {
            Section("Detected on this computer") {
                if apps.isEmpty {
                    Text(busy ? "Looking…" : "No design apps found. Install Blender (free, blender.org), or set SKY_BLENDER to its executable.")
                        .foregroundStyle(.secondary)
                }
                ForEach(Array(apps.enumerated()), id: \.offset) { _, app in
                    VStack(alignment: .leading, spacing: 2) {
                        HStack {
                            Text("\(app["name"].string ?? "") \(app["version"].string ?? "")").font(.headline)
                            if app["tested"].bool == false {
                                Text("untested adapter").font(.caption).foregroundStyle(Theme.warning)
                            }
                            Spacer()
                            Text((app["found_via"].string ?? "")).font(.caption).foregroundStyle(.secondary)
                        }
                        Text(app["executable"].string ?? "").font(Theme.monoSmall).foregroundStyle(.secondary).textSelection(.enabled)
                        if let note = app["note"].string { Text(note).font(.caption).foregroundStyle(Theme.warning) }
                    }
                }
                HStack {
                    Button("Rescan") { refresh(rescan: true) }.disabled(busy)
                    if busy { ProgressView().controlSize(.small) }
                    Spacer()
                }
                Text("Override a path with SKY_BLENDER, SKY_MAYAPY, SKY_HOUDINI_HYTHON or SKY_3DSMAX_BATCH, or in ~/.skywalker/dcc/paths.json.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Live Blender session") {
                if let version = session["version"].string, session["connected"].bool == true {
                    LabeledContent("Connected", value: "Blender \(version) (\(session["mode"].string ?? ""))")
                    Button("Stop Bridge") { run("dcc_session_stop") }.disabled(busy)
                } else {
                    Text("Not connected. Install the add-on, then click Start Bridge in Blender's Skywalker tab (press N in the 3D viewport) " +
                         "— or right-click a model in the Asset Browser and choose Open in Blender.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Button("Install Blender Add-on") { run("dcc_install_addon") }
                    .disabled(busy || !apps.contains { $0["app"].string == "blender" })
                if !message.isEmpty { Text(message).font(.caption).foregroundStyle(.secondary).textSelection(.enabled) }
            }
        }
        .formStyle(.grouped)
        .onAppear { refresh(rescan: false) }
    }

    /// The engine runs design apps synchronously for in-editor callers, so give the UI a frame to
    /// show the spinner before the call.
    private func run(_ tool: String) {
        busy = true
        DispatchQueue.main.async {
            let r = engine.call(tool, [:], actor: "editor")
            message = r.text.components(separatedBy: "\n").first ?? ""
            busy = false
            refresh(rescan: false)
        }
    }

    private func refresh(rescan: Bool) {
        busy = true
        DispatchQueue.main.async {
            let list = engine.call("dcc_list", rescan ? ["refresh": true] : [:], actor: "editor")
            apps = list.structured["apps"].array
            session = engine.call("dcc_session_status", [:], actor: "editor").structured
            busy = false
        }
    }
}

private struct CopyableCommand: View {
    let command: String
    var body: some View {
        HStack(alignment: .top) {
            Text(command).font(Theme.monoSmall).textSelection(.enabled)
            Spacer()
            Button("Copy", systemImage: "doc.on.doc") {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(command, forType: .string)
            }
            .labelStyle(.iconOnly).buttonStyle(.borderless)
        }
    }
}
