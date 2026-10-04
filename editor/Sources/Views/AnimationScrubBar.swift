import SwiftUI

/// Timeline scrubber under the Animator / Sequencer sections of the Details panel.
/// Dragging previews live (transient, nothing saved); releasing stores the time as one
/// undoable edit. The button toggles live playback (animator) or the viewport preview
/// (sequencer). Everything goes through the same tools agents use.
struct AnimationScrubBar: View {
    @Environment(EngineStore.self) private var engine
    let entityID: UInt64
    let component: String  // "animator" | "sequencer"
    let values: JSON
    let onCommit: () -> Void

    @State private var length: Double = 1
    @State private var dragging: Double?

    private var isAnimator: Bool { component == "animator" }
    private var time: Double { dragging ?? (values["time"].number ?? 0) }
    private var active: Bool { isAnimator ? values["preview"].string == "play" : (values["preview"].bool ?? false) }
    private var id: JSON { .number(Double(entityID)) }

    var body: some View {
        HStack(spacing: 6) {
            Button(action: toggle) {
                Image(systemName: isAnimator ? (active ? "pause.fill" : "play.fill") : (active ? "eye.fill" : "eye.slash"))
                    .font(.system(size: 10))
                    .foregroundStyle(active ? Theme.accent : Theme.textDim)
                    .frame(width: 18, height: 18)
            }
            .buttonStyle(.borderless)
            .help(isAnimator ? "Animate live in the editor" : "Show the sequence at this time in the viewport")
            Slider(value: Binding(get: { min(time, length) }, set: scrub), in: 0...max(length, 0.01),
                   onEditingChanged: { editing in if !editing { commit() } })
                .controlSize(.mini)
            Text(String(format: "%.2f s", time))
                .font(Theme.monoSmall)
                .foregroundStyle(Theme.textDim)
                .frame(width: 52, alignment: .trailing)
            if !isAnimator {
                Button { engine.movieRequest = MovieRequest(sequence: entityID) } label: {
                    Image(systemName: "film").font(.system(size: 10)).foregroundStyle(Theme.textDim).frame(width: 18, height: 18)
                }
                .buttonStyle(.borderless)
                .disabled(engine.movie?.finished == false)
                .help("Render this sequence to a movie…")
            }
        }
        .padding(.horizontal, 10)
        .padding(.bottom, 6)
        .task(id: "\(entityID)|\(values["clip"].string ?? "")|\(values["controller"].string ?? "")|\(values["sequence"].string ?? "")") {
            loadLength()
        }
    }

    /// Clip length (animator: its clip, else the longest) or sequence length.
    private func loadLength() {
        if isAnimator {
            let r = engine.call("animation_list", ["entity": id], actor: "editor")
            let clips = r.structured["clips"].array
            let name = values["clip"].string ?? ""
            let chosen = clips.first { $0["name"].string == name }?["duration"].number
            length = max(chosen ?? clips.compactMap { $0["duration"].number }.max() ?? 1, 0.05)
        } else {
            let r = engine.call("sequence_get", ["sequence": id], actor: "editor")
            length = max(r.structured["length"].number ?? 1, 0.05)
        }
    }

    private func scrub(_ t: Double) {
        dragging = t
        if isAnimator {
            engine.call("animator_set", ["entity": id, "preview_time": .number(t)], actor: "editor")
        } else {
            engine.call("sequence_scrub", ["sequence": id, "time": .number(t), "transient": true], actor: "editor")
        }
    }

    private func commit() {
        guard let t = dragging else { return }
        if isAnimator {
            engine.call("animator_set", ["entity": id, "time": .number(t), "clear_preview": true])
        } else {
            engine.call("sequence_scrub", ["sequence": id, "clear": true], actor: "editor")
            engine.call("sequence_scrub", ["sequence": id, "time": .number(t), "persist": true])
        }
        dragging = nil
        onCommit()
    }

    private func toggle() {
        if isAnimator {
            engine.call("animator_set", ["entity": id, "preview": .string(active ? "pose" : "play")])
        } else {
            engine.call("entity_update", ["entity": id, "components": ["sequencer": ["preview": .bool(!active)]]])
        }
        onCommit()
    }
}
