import SwiftUI

/// Skywalker editor theme: a dense, graphite "pro tool" palette (in the family of Unreal,
/// Blender and Xcode) with one calm sky-blue accent, an orange selection color that
/// matches the viewport outline, and a soft violet reserved for AI/agent affordances.
enum Theme {
    // Surfaces
    static let window = Color(hex: "#151619")
    static let panel = Color(hex: "#1c1d21")
    static let panelRaised = Color(hex: "#23252a")
    static let header = Color(hex: "#202226")
    static let field = Color(hex: "#121316")
    static let border = Color(hex: "#2c2e34")
    static let hover = Color.white.opacity(0.05)

    // Text
    static let text = Color(hex: "#e3e4e8")
    static let textDim = Color(hex: "#9b9da6")
    static let textFaint = Color(hex: "#6b6d76")

    // Accents
    static let accent = Color(hex: "#4f8dff")
    static let selection = Color(hex: "#ff7a1a")
    static let ai = Color(hex: "#9a86ff")
    static let success = Color(hex: "#4fc38a")
    static let warning = Color(hex: "#f2b44c")
    static let error = Color(hex: "#f0616d")
    static let axisX = Color(hex: "#ef4d55")
    static let axisY = Color(hex: "#73c94d")
    static let axisZ = Color(hex: "#4a8cfa")

    // Legacy names used by crew colors
    static let sky = accent
    static let violet = ai
    static let rose = error
    static let dawn = warning
    static let mint = success

    static let radius: CGFloat = 5
    static let cornerRadius: CGFloat = 5
    static let rowHeight: CGFloat = 22

    static let label = Font.system(size: 11)
    static let body = Font.system(size: 12)
    static let mono = Font.system(size: 11.5, design: .monospaced)
    static let monoSmall = Font.system(size: 10.5, design: .monospaced)
    static let sectionTitle = Font.system(size: 11, weight: .semibold)
    static let caps = Font.system(size: 10, weight: .semibold)
}

/// Uppercase panel title bar with optional trailing accessories.
struct PanelHeader<Trailing: View>: View {
    let title: String
    var icon: String? = nil
    @ViewBuilder var trailing: Trailing

    var body: some View {
        HStack(spacing: 6) {
            if let icon { Image(systemName: icon).font(.system(size: 10)).foregroundStyle(Theme.textFaint) }
            Text(title.uppercased()).font(Theme.caps).tracking(0.6).foregroundStyle(Theme.textDim)
            Spacer(minLength: 4)
            trailing
        }
        .padding(.horizontal, 10)
        .frame(height: 28)
        .background(Theme.header)
        .overlay(alignment: .bottom) { Rectangle().fill(Theme.border).frame(height: 1) }
    }
}

extension PanelHeader where Trailing == EmptyView {
    init(title: String, icon: String? = nil) {
        self.init(title: title, icon: icon) { EmptyView() }
    }
}

/// Compact tab strip used by docked panels.
struct PanelTabs<Tab: Hashable & Identifiable>: View {
    let tabs: [Tab]
    @Binding var selection: Tab
    let title: (Tab) -> String
    let icon: (Tab) -> String
    var trailing: AnyView? = nil

    var body: some View {
        HStack(spacing: 0) {
            ForEach(tabs) { tab in
                Button { selection = tab } label: {
                    HStack(spacing: 5) {
                        Image(systemName: icon(tab)).font(.system(size: 10))
                        Text(title(tab)).font(Theme.label.weight(selection == tab ? .semibold : .regular))
                    }
                    .padding(.horizontal, 10)
                    .frame(height: 28)
                    .foregroundStyle(selection == tab ? Theme.text : Theme.textDim)
                    .background(selection == tab ? Theme.panel : .clear)
                    .overlay(alignment: .top) {
                        if selection == tab { Rectangle().fill(Theme.accent).frame(height: 2) }
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
            Spacer(minLength: 0)
            if let trailing { trailing.padding(.trailing, 8) }
        }
        .frame(height: 28)
        .background(Theme.header)
        .overlay(alignment: .bottom) { Rectangle().fill(Theme.border).frame(height: 1) }
    }
}

/// Small borderless icon button used in panel headers and toolbars.
struct IconButton: View {
    let symbol: String
    let help: String
    var active = false
    var tint: Color? = nil
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 11, weight: .medium))
                .frame(width: 22, height: 20)
                .foregroundStyle(active ? (tint ?? Theme.accent) : Theme.textDim)
                .background(active ? (tint ?? Theme.accent).opacity(0.16) : .clear, in: RoundedRectangle(cornerRadius: 4))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(help)
        .accessibilityLabel(help)
    }
}

/// The Skywalker mark: a small cloud with a sparkle.
struct SkywalkerMark: View {
    var size: CGFloat = 16
    var body: some View {
        ZStack {
            CloudShape()
                .fill(LinearGradient(colors: [Theme.accent, Theme.ai], startPoint: .topLeading, endPoint: .bottomTrailing))
                .frame(width: size * 1.4, height: size)
            Image(systemName: "sparkle")
                .font(.system(size: size * 0.4, weight: .bold))
                .foregroundStyle(.white)
                .offset(x: size * 0.08, y: size * 0.08)
        }
        .accessibilityLabel("Skywalker")
    }
}

extension Color {
    init(hex: String) {
        var s = hex.trimmingCharacters(in: .whitespaces)
        if s.hasPrefix("#") { s.removeFirst() }
        if s.count == 8 { s = String(s.prefix(6)) }  // ignore alpha
        if s.count == 3 { s = s.map { "\($0)\($0)" }.joined() }
        let v = UInt64(s, radix: 16) ?? 0x4f8dff
        self.init(red: Double((v >> 16) & 0xff) / 255, green: Double((v >> 8) & 0xff) / 255, blue: Double(v & 0xff) / 255)
    }

    var hexString: String {
        let c = NSColor(self).usingColorSpace(.sRGB) ?? .white
        return String(format: "#%02x%02x%02x", Int((c.redComponent * 255).rounded()), Int((c.greenComponent * 255).rounded()),
                      Int((c.blueComponent * 255).rounded()))
    }
}
