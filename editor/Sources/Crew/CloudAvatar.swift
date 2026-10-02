import SwiftUI

/// A puffy cloud silhouette built from overlapping circles, scaled to its rect.
struct CloudShape: Shape {
    func path(in rect: CGRect) -> Path {
        let w = rect.width, h = rect.height
        var p = Path()
        // Flat-ish bottom with three puffs on top.
        let base = CGRect(x: rect.minX + w * 0.08, y: rect.minY + h * 0.45, width: w * 0.84, height: h * 0.55)
        p.addRoundedRect(in: base, cornerSize: CGSize(width: h * 0.27, height: h * 0.27))
        p.addEllipse(in: CGRect(x: rect.minX, y: rect.minY + h * 0.38, width: w * 0.40, height: h * 0.62))
        p.addEllipse(in: CGRect(x: rect.minX + w * 0.20, y: rect.minY + h * 0.05, width: w * 0.46, height: h * 0.76))
        p.addEllipse(in: CGRect(x: rect.minX + w * 0.48, y: rect.minY + h * 0.22, width: w * 0.40, height: h * 0.62))
        p.addEllipse(in: CGRect(x: rect.minX + w * 0.66, y: rect.minY + h * 0.42, width: w * 0.34, height: h * 0.56))
        return p
    }
}

/// The face style of a Cloudling, so each crew member is recognizable at a glance.
enum CloudFace: String, Codable, CaseIterable, Sendable {
    case happy, focused, dreamy, wink, determined, curious

    var eyesClosed: Bool { self == .dreamy }
}

/// A Cloudling avatar: a cute cloud with a face, tinted by role, optionally holding an
/// accessory symbol (a pen for the writer, a map for the level designer...). It bobs and
/// shows a sparkle when the agent is working, so you can *see* who is busy.
struct CloudAvatar: View {
    var color: Color
    var face: CloudFace = .happy
    var accessory: String? = nil
    var size: CGFloat = 44
    var working = false
    var dimmed = false

    @State private var bob = false

    var body: some View {
        ZStack {
            CloudShape()
                .fill(LinearGradient(colors: [color.mix(with: .white, by: 0.55), color],
                                     startPoint: .top, endPoint: .bottom))
                .shadow(color: color.opacity(0.35), radius: size * 0.08, y: size * 0.05)
            CloudShape()
                .stroke(.white.opacity(0.55), lineWidth: max(1, size * 0.025))
            faceView
                .offset(y: size * 0.08)
            if let accessory {
                Image(systemName: accessory)
                    .font(.system(size: size * 0.22, weight: .semibold))
                    .foregroundStyle(.white)
                    .padding(size * 0.06)
                    .background(Circle().fill(color.mix(with: .black, by: 0.2)))
                    .offset(x: size * 0.42, y: size * 0.26)
            }
            if working {
                Image(systemName: "sparkles")
                    .font(.system(size: size * 0.24))
                    .foregroundStyle(Theme.dawn)
                    .offset(x: -size * 0.45, y: -size * 0.28)
                    .symbolEffect(.pulse, options: .repeating)
            }
        }
        .frame(width: size * 1.35, height: size)
        .offset(y: working && bob ? -size * 0.06 : 0)
        .opacity(dimmed ? 0.45 : 1)
        .animation(working ? .easeInOut(duration: 0.6).repeatForever(autoreverses: true) : .default, value: bob)
        .onAppear { bob = working }
        .onChange(of: working) { _, now in bob = now }
        .accessibilityHidden(true)
    }

    private var faceView: some View {
        let eye = size * 0.075
        let ink = Color(red: 0.16, green: 0.18, blue: 0.28)
        return VStack(spacing: size * 0.04) {
            HStack(spacing: size * 0.2) {
                eyeShape(open: !face.eyesClosed, size: eye, ink: ink)
                eyeShape(open: face != .wink && !face.eyesClosed, size: eye, ink: ink)
            }
            mouth(ink: ink)
        }
        .overlay(alignment: .center) {
            HStack(spacing: size * 0.38) {  // blush
                Circle().fill(Theme.rose.opacity(0.35)).frame(width: eye * 1.4)
                Circle().fill(Theme.rose.opacity(0.35)).frame(width: eye * 1.4)
            }
            .offset(y: size * 0.05)
        }
    }

    @ViewBuilder
    private func eyeShape(open: Bool, size eye: CGFloat, ink: Color) -> some View {
        if open {
            Ellipse().fill(ink).frame(width: eye, height: face == .focused ? eye * 0.7 : eye * 1.15)
        } else {
            Capsule().fill(ink).frame(width: eye * 1.2, height: eye * 0.32)
        }
    }

    @ViewBuilder
    private func mouth(ink: Color) -> some View {
        switch face {
        case .determined, .focused:
            Capsule().fill(ink).frame(width: size * 0.13, height: size * 0.035)
        case .curious:
            Circle().stroke(ink, lineWidth: size * 0.03).frame(width: size * 0.08)
        default:
            SmileShape().stroke(ink, style: StrokeStyle(lineWidth: size * 0.035, lineCap: .round))
                .frame(width: size * 0.16, height: size * 0.07)
        }
    }
}

private struct SmileShape: Shape {
    func path(in rect: CGRect) -> Path {
        var p = Path()
        p.move(to: CGPoint(x: rect.minX, y: rect.minY))
        p.addQuadCurve(to: CGPoint(x: rect.maxX, y: rect.minY), control: CGPoint(x: rect.midX, y: rect.maxY * 1.6))
        return p
    }
}
