import SwiftUI

// The Studio's Tools tab: tools the agents defined for themselves (custom Wander and composite tools)
// and tools hosted by connected clients. The human approves or rejects the ones that need it,
// disables or enables them, and reads their code, capabilities and recent calls (docs/CUSTOM_TOOLS.md).

struct CustomToolInfo: Identifiable, Hashable, Sendable {
    let id: String  // the tool name
    var title: String
    var kind: String
    var status: String
    var reason: String
    var category: String
    var version: Int
    var author: String
    var host: String
    var description: String
    var mutates: Bool
    var privileged: Bool
    var calls: Int
    var failures: Int

    init(json j: JSON) {
        id = j["name"].string ?? ""
        title = j["title"].string ?? id
        kind = j["kind"].string ?? ""
        status = j["status"].string ?? ""
        reason = j["reason"].string ?? ""
        category = j["category"].string ?? ""
        version = j["version"].int ?? 1
        author = j["author"].string ?? ""
        host = j["host"].string ?? ""
        description = j["description"].string ?? ""
        mutates = j["mutates"].bool ?? false
        privileged = j["privileged"].bool ?? false
        calls = j["calls"].int ?? 0
        failures = j["failures"].int ?? 0
    }

    var needsHuman: Bool { status == "pending_approval" }
}

struct CustomToolsView: View {
    @Environment(StudioStore.self) private var studio

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                Text("Approval policy").font(Theme.label).foregroundStyle(Theme.textDim)
                Picker("Policy", selection: Binding(get: { studio.customToolPolicy }, set: { studio.setCustomToolPolicy($0) })) {
                    Text("Off").tag("off")
                    Text("Ask for every tool").tag("ask")
                    Text("Ask when it can change things").tag("auto")
                    Text("Trust").tag("trust")
                }
                .labelsHidden().fixedSize().controlSize(.small)
                .help("Who approves the tools agents define (game.json customTools.policy)")
                Spacer()
                Text("\(studio.customTools.count) tool\(studio.customTools.count == 1 ? "" : "s")").font(Theme.label).foregroundStyle(Theme.textFaint)
                Button { studio.reloadCustomTools() } label: { Image(systemName: "arrow.clockwise") }
                    .buttonStyle(.borderless).help("Rescan tools/ for definitions edited by hand")
            }
            .padding(.horizontal, 8).padding(.vertical, 4)
            List(studio.customTools) { tool in
                CustomToolRow(tool: tool).listRowSeparator(.visible).listRowBackground(Theme.panel)
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .overlay {
                if studio.customTools.isEmpty {
                    Text("No custom tools yet. Agents define them with tool_define when no engine tool does what they need.")
                        .font(Theme.label).foregroundStyle(Theme.textFaint).multilineTextAlignment(.center).padding()
                }
            }
        }
    }
}

private func toolStatusColor(_ s: String) -> Color {
    switch s {
    case "active": Theme.success
    case "pending_approval": Theme.warning
    case "rejected", "invalid": Theme.error
    default: Theme.textFaint
    }
}

private struct CustomToolRow: View {
    @Environment(StudioStore.self) private var studio
    let tool: CustomToolInfo
    @State private var inspecting = false

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 6) {
                Image(systemName: tool.kind == "external" ? "point.3.connected.trianglepath.dotted" : tool.kind == "composite" ? "list.bullet.rectangle" : "function")
                    .font(.system(size: 10)).foregroundStyle(Theme.textDim).frame(width: 12)
                Text(tool.id).font(Theme.mono).lineLimit(1)
                Text("v\(tool.version)").font(Theme.monoSmall).foregroundStyle(Theme.textFaint)
                if tool.mutates {
                    Image(systemName: "pencil").font(.system(size: 9)).foregroundStyle(Theme.warning).help("Can change the project")
                }
                Spacer()
                Text(tool.status.replacingOccurrences(of: "_", with: " ")).font(Theme.caps)
                    .padding(.horizontal, 5).padding(.vertical, 1)
                    .foregroundStyle(toolStatusColor(tool.status))
                    .background(toolStatusColor(tool.status).opacity(0.14), in: Capsule())
                if tool.needsHuman || tool.status == "rejected" {
                    Button("Approve") { studio.approveCustomTool(tool.id, approve: true) }.controlSize(.small)
                        .help("Allow this exact definition to run (changing it later needs approval again)")
                }
                if tool.needsHuman {
                    Button("Reject") { studio.approveCustomTool(tool.id, approve: false) }.controlSize(.small)
                }
                if tool.status == "active" || tool.status == "disabled" {
                    Toggle("", isOn: Binding(get: { tool.status == "active" }, set: { studio.enableCustomTool(tool.id, enabled: $0) }))
                        .toggleStyle(.switch).controlSize(.mini).labelsHidden().help("Enable or disable")
                }
                Button { inspecting = true } label: { Image(systemName: "doc.text.magnifyingglass") }
                    .buttonStyle(.borderless).help("Code, capabilities, approval and recent calls")
                    .popover(isPresented: $inspecting, arrowEdge: .leading) { CustomToolInspector(name: tool.id) }
            }
            Text(tool.description).font(Theme.label).foregroundStyle(Theme.textDim).lineLimit(2).padding(.leading, 18)
            HStack(spacing: 6) {
                Text("by \(tool.host.isEmpty ? tool.author : tool.host)")
                Text("· \(tool.kind)")
                if tool.calls > 0 { Text("· \(tool.calls) call\(tool.calls == 1 ? "" : "s")") }
                if tool.failures > 0 { Text("· \(tool.failures) failed").foregroundStyle(Theme.error) }
                if !tool.reason.isEmpty && tool.status != "active" { Text("· \(tool.reason)").lineLimit(1) }
            }
            .font(Theme.label).foregroundStyle(Theme.textFaint).padding(.leading, 18)
        }
        .padding(.vertical, 2)
    }
}

private struct CustomToolInspector: View {
    @Environment(StudioStore.self) private var studio
    let name: String

    var body: some View {
        let info = studio.call("tool_inspect", ["name": .string(name)])
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                Text(name).font(Theme.sectionTitle)
                Text(info["description"].string ?? "").font(Theme.body).foregroundStyle(Theme.textDim)
                section("Capabilities", info["definition"]["capabilities"].serialized())
                section("Limits", info["definition"]["limits"].serialized())
                if let code = info["code"].string {
                    section("Code", code)
                } else if info["definition"]["steps"].array.count > 0 {
                    section("Steps", info["definition"]["steps"].serialized())
                }
                if info["approval"]["by"].string != nil {
                    section("Approval", "\(info["approval"]["verdict"].string ?? "") by \(info["approval"]["by"].string ?? "") at \(info["approval"]["at"].string ?? "")"
                            + ((info["approval"]["current"].bool ?? false) ? "" : " (for an earlier version)"))
                }
                let errors = info["stats"]["recent_errors"].array
                if !errors.isEmpty {
                    section("Recent errors", errors.map { "\($0["at"].string ?? "") \($0["actor"].string ?? ""): \($0["error"].string ?? "")" }.joined(separator: "\n"))
                }
            }
            .padding(12)
        }
        .frame(width: 460, height: 420)
    }

    private func section(_ title: String, _ text: String) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title).font(Theme.caps).foregroundStyle(Theme.textFaint)
            Text(text).font(Theme.monoSmall).textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(6).background(Theme.field, in: RoundedRectangle(cornerRadius: Theme.radius))
        }
    }
}
