// Renders crew avatars (same cloud shape/face as the editor) to PNGs for the demo video.
// Usage: swift render_avatars.swift OUT_DIR
import AppKit
import SwiftUI

struct CloudShape: Shape {
    func path(in r: CGRect) -> Path {
        let w = r.width, h = r.height
        var p = Path()
        p.addRoundedRect(in: CGRect(x: r.minX + w * 0.08, y: r.minY + h * 0.45, width: w * 0.84, height: h * 0.55),
                         cornerSize: CGSize(width: h * 0.27, height: h * 0.27))
        p.addEllipse(in: CGRect(x: r.minX, y: r.minY + h * 0.38, width: w * 0.40, height: h * 0.62))
        p.addEllipse(in: CGRect(x: r.minX + w * 0.20, y: r.minY + h * 0.05, width: w * 0.46, height: h * 0.76))
        p.addEllipse(in: CGRect(x: r.minX + w * 0.48, y: r.minY + h * 0.22, width: w * 0.40, height: h * 0.62))
        p.addEllipse(in: CGRect(x: r.minX + w * 0.66, y: r.minY + h * 0.42, width: w * 0.34, height: h * 0.56))
        return p.normalized(eoFill: false)
    }
}

struct Avatar: View {
    let color: Color
    let face: String
    var body: some View {
        let ink = Color(red: 0.106, green: 0.114, blue: 0.169)
        let size: CGFloat = 200
        ZStack {
            CloudShape().fill(color.gradient)
            CloudShape().stroke(.black.opacity(0.25), lineWidth: 2)
            VStack(spacing: size * 0.06) {
                HStack(spacing: size * 0.2) {
                    eye(open: face != "dreamy", ink: ink, size: size)
                    eye(open: face != "dreamy" && face != "wink", ink: ink, size: size)
                }
                Capsule().fill(ink.opacity(0.85)).frame(width: size * 0.14, height: size * 0.04)
            }
            .offset(y: size * 0.1)
        }
        .frame(width: size * 1.35, height: size)
        .padding(12)
    }
    @ViewBuilder func eye(open: Bool, ink: Color, size: CGFloat) -> some View {
        let e = size * 0.085
        if open { Capsule().fill(ink).frame(width: e, height: face == "focused" ? e * 0.8 : e * 1.25) }
        else { Capsule().fill(ink).frame(width: e * 1.3, height: e * 0.35) }
    }
}

let crew: [(String, String, String)] = [
    ("nimbus", "#8b73fa", "determined"), ("cirro", "#5c8fed", "happy"), ("stratus", "#54ccad", "focused"),
    ("aurora", "#ffb873", "dreamy"), ("haze", "#ed6b7a", "wink"),
]

func color(_ hex: String) -> Color {
    let v = UInt64(hex.dropFirst(), radix: 16)!
    return Color(red: Double((v >> 16) & 0xff) / 255, green: Double((v >> 8) & 0xff) / 255, blue: Double(v & 0xff) / 255)
}

MainActor.assumeIsolated {
    let out = CommandLine.arguments[1]
    for (name, hex, face) in crew {
        let r = ImageRenderer(content: Avatar(color: color(hex), face: face))
        r.scale = 2
        let rep = NSBitmapImageRep(cgImage: r.cgImage!)
        try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: "\(out)/\(name).png"))
    }
    print("ok")
}
