import SwiftUI

/// Skywalker's visual language: a calm daytime sky. Soft blues, warm sunlight accents,
/// rounded shapes. Colors adapt to light/dark appearance.
enum Theme {
    static let sky = Color(red: 0.36, green: 0.56, blue: 0.93)
    static let skyDeep = Color(red: 0.16, green: 0.27, blue: 0.56)
    static let dawn = Color(red: 1.0, green: 0.72, blue: 0.45)
    static let mint = Color(red: 0.33, green: 0.80, blue: 0.68)
    static let rose = Color(red: 0.93, green: 0.42, blue: 0.48)
    static let violet = Color(red: 0.55, green: 0.45, blue: 0.98)

    static let gradient = LinearGradient(colors: [sky, violet.opacity(0.85)], startPoint: .topLeading,
                                         endPoint: .bottomTrailing)

    static let cornerRadius: CGFloat = 10
    static let mono = Font.system(.body, design: .monospaced)
    static let monoSmall = Font.system(.caption, design: .monospaced)
}

/// Rounded card background used by panels.
struct CardBackground: ViewModifier {
    func body(content: Content) -> some View {
        content
            .padding(10)
            .background(.background.secondary, in: RoundedRectangle(cornerRadius: Theme.cornerRadius, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: Theme.cornerRadius, style: .continuous)
                .strokeBorder(.separator.opacity(0.6), lineWidth: 0.5))
    }
}

extension View {
    func card() -> some View { modifier(CardBackground()) }
}

/// The Skywalker wordmark: a little cloud with a trail.
struct SkywalkerLogo: View {
    var size: CGFloat = 22
    var body: some View {
        HStack(spacing: size * 0.3) {
            ZStack {
                CloudShape()
                    .fill(Theme.gradient)
                    .frame(width: size * 1.5, height: size)
                Image(systemName: "sparkle")
                    .font(.system(size: size * 0.42, weight: .bold))
                    .foregroundStyle(.white)
                    .offset(x: size * 0.1, y: size * 0.08)
            }
            Text("Skywalker")
                .font(.system(size: size * 0.8, weight: .bold, design: .rounded))
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Skywalker")
    }
}
