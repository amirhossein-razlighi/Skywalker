import SwiftUI

struct SettingsView: View {
    var body: some View {
        TabView {
            Tab("Crew", systemImage: "cloud.fill") { CrewManager() }
            Tab("Providers", systemImage: "brain") { ProviderSettings() }
            Tab("External Agents", systemImage: "antenna.radiowaves.left.and.right") { ExternalAgentSettings() }
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
