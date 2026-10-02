import SwiftUI

/// A puffy cloud silhouette built from overlapping circles, scaled to its rect.
struct CloudShape: Shape {
    func path(in rect: CGRect) -> Path {
        let w = rect.width, h = rect.height
        var p = Path()
        let base = CGRect(x: rect.minX + w * 0.08, y: rect.minY + h * 0.45, width: w * 0.84, height: h * 0.55)
        p.addRoundedRect(in: base, cornerSize: CGSize(width: h * 0.27, height: h * 0.27))
        p.addEllipse(in: CGRect(x: rect.minX, y: rect.minY + h * 0.38, width: w * 0.40, height: h * 0.62))
        p.addEllipse(in: CGRect(x: rect.minX + w * 0.20, y: rect.minY + h * 0.05, width: w * 0.46, height: h * 0.76))
        p.addEllipse(in: CGRect(x: rect.minX + w * 0.48, y: rect.minY + h * 0.22, width: w * 0.40, height: h * 0.62))
        p.addEllipse(in: CGRect(x: rect.minX + w * 0.66, y: rect.minY + h * 0.42, width: w * 0.34, height: h * 0.56))
        return p
    }
}

/// Face variants keep crew members distinguishable at small sizes.
enum CloudFace: String, Codable, CaseIterable, Sendable {
    case happy, focused, dreamy, wink, determined, curious
}

/// A Cloudling avatar: a flat cloud in the member's color with a minimal face.
/// Deliberately quiet — it should read like a collaborator's avatar in a pro tool, not a
/// mascot. When the agent is working, a small activity ring pulses around it.
struct CloudAvatar: View {
    var color: Color
    var face: CloudFace = .happy
    var size: CGFloat = 20
    var working = false
    var dimmed = false

    var body: some View {
        ZStack {
            CloudShape()
                .fill(color.gradient)
            CloudShape()
                .stroke(.black.opacity(0.25), lineWidth: 0.5)
            faceView.offset(y: size * 0.1)
        }
        .frame(width: size * 1.35, height: size)
        .overlay(alignment: .bottomTrailing) {
            if working {
                Circle()
                    .fill(Theme.success)
                    .frame(width: max(5, size * 0.28), height: max(5, size * 0.28))
                    .overlay(Circle().stroke(Theme.panel, lineWidth: 1.5))
                    .phaseAnimator([1.0, 0.45]) { dot, phase in dot.opacity(phase) } animation: { _ in .easeInOut(duration: 0.7) }
                    .offset(x: 1, y: 1)
            }
        }
        .opacity(dimmed ? 0.45 : 1)
        .accessibilityHidden(true)
    }

    private var faceView: some View {
        let ink = Color(hex: "#1b1d2b")
        let eye = max(1.6, size * 0.085)
        return VStack(spacing: size * 0.06) {
            HStack(spacing: size * 0.2) {
                eyeShape(open: face != .dreamy, size: eye, ink: ink)
                eyeShape(open: face != .dreamy && face != .wink, size: eye, ink: ink)
            }
            if size >= 18 {
                Capsule().fill(ink.opacity(0.85))
                    .frame(width: size * (face == .curious ? 0.07 : 0.14), height: max(1, size * 0.04))
            }
        }
    }

    @ViewBuilder
    private func eyeShape(open: Bool, size eye: CGFloat, ink: Color) -> some View {
        if open {
            Capsule().fill(ink).frame(width: eye, height: face == .focused ? eye * 0.8 : eye * 1.25)
        } else {
            Capsule().fill(ink).frame(width: eye * 1.3, height: max(1, eye * 0.35))
        }
    }
}
