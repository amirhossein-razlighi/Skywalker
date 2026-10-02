// Renders the Skywalker app icon (1024x1024 PNG) with SwiftUI.
// Usage: swift tools/branding/make_icon.swift <out.png>
// Then tools/branding/make_icns.sh turns it into editor/Resources/AppIcon.icns.

import AppKit
import SwiftUI

struct Cloud: Shape {
    func path(in r: CGRect) -> Path {
        let w = r.width, h = r.height
        var p = Path()
        p.addRoundedRect(in: CGRect(x: r.minX + w * 0.08, y: r.minY + h * 0.45, width: w * 0.84, height: h * 0.55),
                         cornerSize: CGSize(width: h * 0.27, height: h * 0.27))
        p.addEllipse(in: CGRect(x: r.minX, y: r.minY + h * 0.38, width: w * 0.40, height: h * 0.62))
        p.addEllipse(in: CGRect(x: r.minX + w * 0.20, y: r.minY + h * 0.05, width: w * 0.46, height: h * 0.76))
        p.addEllipse(in: CGRect(x: r.minX + w * 0.48, y: r.minY + h * 0.22, width: w * 0.40, height: h * 0.62))
        p.addEllipse(in: CGRect(x: r.minX + w * 0.66, y: r.minY + h * 0.42, width: w * 0.34, height: h * 0.56))
        return p
    }
}

struct Smile: Shape {
    func path(in r: CGRect) -> Path {
        var p = Path()
        p.move(to: CGPoint(x: r.minX, y: r.minY))
        p.addQuadCurve(to: CGPoint(x: r.maxX, y: r.minY), control: CGPoint(x: r.midX, y: r.maxY * 1.6))
        return p
    }
}

struct Icon: View {
    var body: some View {
        let ink = Color(red: 0.12, green: 0.14, blue: 0.25)
        ZStack {
            RoundedRectangle(cornerRadius: 230, style: .continuous)
                .fill(LinearGradient(colors: [Color(red: 0.36, green: 0.58, blue: 0.95), Color(red: 0.55, green: 0.45, blue: 0.98)],
                                     startPoint: .top, endPoint: .bottom))
                .frame(width: 824, height: 824)
            // sun
            Circle().fill(Color(red: 1, green: 0.78, blue: 0.5)).frame(width: 230).offset(x: 210, y: -215)
                .blur(radius: 2)
            // trail
            Capsule().fill(.white.opacity(0.35)).frame(width: 300, height: 26).rotationEffect(.degrees(-14)).offset(x: -230, y: 210)
            ZStack {
                Cloud().fill(.white).shadow(color: .black.opacity(0.18), radius: 24, y: 18)
                VStack(spacing: 22) {
                    HStack(spacing: 92) {
                        Ellipse().fill(ink).frame(width: 46, height: 58)
                        Ellipse().fill(ink).frame(width: 46, height: 58)
                    }
                    Smile().stroke(ink, style: StrokeStyle(lineWidth: 18, lineCap: .round)).frame(width: 90, height: 36)
                }
                .offset(y: 70)
                HStack(spacing: 190) {
                    Circle().fill(Color(red: 0.93, green: 0.42, blue: 0.48).opacity(0.35)).frame(width: 60)
                    Circle().fill(Color(red: 0.93, green: 0.42, blue: 0.48).opacity(0.35)).frame(width: 60)
                }
                .offset(y: 120)
            }
            .frame(width: 600, height: 420)
            .offset(y: 40)
        }
        .frame(width: 1024, height: 1024)
    }
}

@MainActor
func render(to path: String) {
    let renderer = ImageRenderer(content: Icon())
    renderer.scale = 1
    guard let cg = renderer.cgImage else { fatalError("render failed") }
    let rep = NSBitmapImageRep(cgImage: cg)
    try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: path))
    print("wrote \(path)")
}

MainActor.assumeIsolated { render(to: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "icon.png") }
