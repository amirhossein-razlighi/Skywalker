import SwiftUI

/// Game ▸ Render Movie… (and the Sequencer's film button): a compact render-queue form that calls
/// `movie_render` in the background. Progress shows in the viewport (MovieProgressHUD).
struct MovieRenderSheet: View {
    @Environment(EngineStore.self) private var engine
    @Environment(\.dismiss) private var dismiss
    let request: MovieRequest

    @AppStorage("movie.format") private var format = "h264"
    @AppStorage("movie.resolution") private var resolution = "1080p"
    @AppStorage("movie.fps") private var fps = 24
    @AppStorage("movie.samples") private var samples = 8
    @AppStorage("movie.motionBlur") private var motionBlur = true
    @AppStorage("movie.shutter") private var shutter = 0.5
    @AppStorage("movie.simulate") private var simulate = false
    @AppStorage("movie.look") private var look = "final"
    @State private var source: UInt64 = 0  // 0 = scene camera
    @State private var duration = 5.0
    @State private var output = ""
    @State private var error: String?

    private static let formats: [(String, String)] = [
        ("h264", "H.264 · .mp4"), ("hevc", "HEVC 10-bit · .mp4"), ("prores", "ProRes 422 HQ · .mov"), ("png", "PNG sequence"),
    ]

    private var sequencers: [EntitySummary] { engine.entities.filter { $0.components.contains("sequencer") } }
    private var sourceName: String { sequencers.first { $0.id == source }?.name ?? "movie" }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            PanelHeader(title: "Render Movie", icon: "film")
            Grid(alignment: .leading, horizontalSpacing: 10, verticalSpacing: 7) {
                row("Source") {
                    Picker("", selection: $source) {
                        Text("Scene camera").tag(UInt64(0))
                        ForEach(sequencers) { Text($0.name).tag($0.id) }
                    }
                }
                if source == 0 {
                    row("Duration") {
                        HStack {
                            TextField("", value: $duration, format: .number).frame(width: 60)
                            Text("s").foregroundStyle(Theme.textDim)
                        }
                    }
                }
                row("Format") {
                    Picker("", selection: $format) { ForEach(Self.formats, id: \.0) { Text($0.1).tag($0.0) } }
                }
                row("Output") { TextField("", text: $output).font(Theme.mono) }
                row("Size") {
                    HStack(spacing: 8) {
                        Picker("", selection: $resolution) {
                            ForEach(["720p", "1080p", "1440p", "4k"], id: \.self) { Text($0 == "4k" ? "4K" : $0).tag($0) }
                        }
                        .frame(width: 90)
                        Picker("", selection: $fps) { ForEach([24, 25, 30, 60], id: \.self) { Text("\($0) fps").tag($0) } }
                            .frame(width: 90)
                    }
                }
                row("Samples") {
                    Picker("", selection: $samples) {
                        ForEach([1, 4, 8, 16, 32], id: \.self) { Text($0 == 1 ? "1 · draft" : "\($0)").tag($0) }
                    }
                    .frame(width: 110)
                }
                row("Motion blur") {
                    HStack(spacing: 8) {
                        Toggle("", isOn: $motionBlur).toggleStyle(.switch).controlSize(.mini)
                        Slider(value: $shutter, in: 0.1...1).controlSize(.mini).disabled(!motionBlur)
                        Text("\(Int((shutter * 360).rounded()))°").font(Theme.monoSmall).foregroundStyle(Theme.textDim).frame(width: 34)
                    }
                }
                row("Look") {
                    Picker("", selection: $look) {
                        Text("Final").tag("final")
                        Text("Clay").tag("clay")
                        Text("Sketch").tag("sketch")
                    }
                    .pickerStyle(.segmented)
                    .frame(width: 200)
                }
                row("Simulate") {
                    Toggle("Run the game (scripts, physics)", isOn: $simulate).toggleStyle(.checkbox)
                }
            }
            .font(Theme.label)
            .labelsHidden()
            .padding(12)
            if let error {
                Text(error).font(Theme.label).foregroundStyle(Theme.error).padding(.horizontal, 12).padding(.bottom, 8)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Divider().overlay(Theme.border)
            HStack {
                Text("Renders from the scene state; the scene is restored afterwards.")
                    .font(.system(size: 10)).foregroundStyle(Theme.textFaint)
                Spacer()
                Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Render") { render() }.keyboardShortcut(.defaultAction).disabled(engine.movie?.finished == false)
            }
            .controlSize(.small)
            .padding(10)
        }
        .frame(width: 440)
        .background(Theme.panel)
        .foregroundStyle(Theme.text)
        .onAppear {
            source = request.sequence ?? sequencers.first?.id ?? 0
            output = defaultOutput()
        }
        .onChange(of: format) { output = defaultOutput() }
        .onChange(of: source) { output = defaultOutput() }
        .onChange(of: look) { output = defaultOutput() }
    }

    private func row<Content: View>(_ title: String, @ViewBuilder _ content: () -> Content) -> some View {
        GridRow {
            Text(title).foregroundStyle(Theme.textDim).gridColumnAlignment(.trailing)
            content()
        }
    }

    private func defaultOutput() -> String {
        let base = sourceName.lowercased().map { $0.isLetter || $0.isNumber ? $0 : "_" }
        let name = String(base) + (look == "final" ? "" : "_\(look)")
        switch format {
        case "png": return "renders/\(name)/frame_####.png"
        case "prores": return "renders/\(name).mov"
        default: return "renders/\(name).mp4"
        }
    }

    private func render() {
        var args: JSON = [
            "output": .string(output), "codec": .string(format), "resolution": .string(resolution),
            "fps": .number(Double(fps)), "samples": .number(Double(samples)),
            "shutter": .number(motionBlur ? shutter : 0), "simulate": .bool(simulate),
        ]
        if source != 0 {
            args.set("sequence", .number(Double(source)))
        } else {
            args.set("duration", .number(max(duration, 0.1)))
        }
        if look == "clay" { args.set("clay", true) }
        if look == "sketch" { args.set("debug_view", "sketch") }
        let r = engine.startMovie(args)
        if r.isError {
            error = r.text
        } else {
            dismiss()
        }
    }
}

/// Over the viewport while a movie renders (the viewport shows the frames as they come).
struct MovieProgressHUD: View {
    @Environment(EngineStore.self) private var engine

    var body: some View {
        if let m = engine.movie {
            HStack(spacing: 8) {
                Image(systemName: m.finished ? (m.state == "done" ? "checkmark.circle.fill" : "exclamationmark.triangle.fill") : "film")
                    .foregroundStyle(m.state == "done" ? Theme.success : m.state == "rendering" ? Theme.accent : Theme.warning)
                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 6) {
                        Text(m.finished ? "Movie \(m.state)" : "Rendering \(m.name)").fontWeight(.semibold)
                        Text(detail(m)).foregroundStyle(Theme.textDim).font(Theme.monoSmall).lineLimit(1)
                    }
                    ProgressView(value: m.fraction).progressViewStyle(.linear).tint(Theme.accent).frame(width: 240)
                }
                if m.finished {
                    if let path = m.outputs.first, m.state != "failed" {
                        IconButton(symbol: "folder", help: "Show in Finder") {
                            NSWorkspace.shared.activateFileViewerSelecting([URL(filePath: path.replacingOccurrences(of: "####", with: "0000"))])
                        }
                    }
                    IconButton(symbol: "xmark", help: "Dismiss") { engine.dismissMovie() }
                } else {
                    Button("Cancel") { engine.cancelMovie() }.controlSize(.small)
                }
            }
            .font(Theme.label)
            .foregroundStyle(Theme.text)
            .padding(.horizontal, 10).padding(.vertical, 6)
            .background(Theme.panel.opacity(0.92), in: RoundedRectangle(cornerRadius: 6))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(Theme.border))
            .help(m.error ?? m.outputs.joined(separator: "\n"))
        }
    }

    private func detail(_ m: MovieProgress) -> String {
        if m.state == "failed" { return m.error ?? "" }
        var s = "\(m.frame)/\(m.frames)"
        if m.msPerFrame > 0 { s += String(format: " · %.2f s/frame", m.msPerFrame / 1000) }
        if !m.finished && m.eta > 0 { s += String(format: " · %d:%02d left", Int(m.eta) / 60, Int(m.eta) % 60) }
        return s
    }
}
