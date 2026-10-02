import AppKit
import SwiftUI

/// A compact code editor for Wander: NSTextView with syntax highlighting, monospaced font,
/// no smart quotes/substitutions, and inline error-line marking.
struct CodeEditor: NSViewRepresentable {
    @Binding var text: String
    var errorLines: Set<Int> = []

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeNSView(context: Context) -> NSScrollView {
        let scroll = NSTextView.scrollableTextView()
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        let tv = scroll.documentView as! NSTextView  // swiftlint:disable:this force_cast
        tv.delegate = context.coordinator
        tv.isRichText = false
        tv.allowsUndo = true
        tv.drawsBackground = false
        tv.isAutomaticQuoteSubstitutionEnabled = false
        tv.isAutomaticDashSubstitutionEnabled = false
        tv.isAutomaticTextReplacementEnabled = false
        tv.isAutomaticSpellingCorrectionEnabled = false
        tv.isContinuousSpellCheckingEnabled = false
        tv.font = CodeEditor.font
        tv.textColor = NSColor(Theme.text)
        tv.insertionPointColor = NSColor(Theme.accent)
        tv.textContainerInset = NSSize(width: 4, height: 6)
        tv.string = text
        context.coordinator.highlight(tv)
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        context.coordinator.parent = self
        guard let tv = scroll.documentView as? NSTextView else { return }
        if tv.string != text {
            let selected = tv.selectedRanges
            tv.string = text
            tv.selectedRanges = selected.filter { $0.rangeValue.upperBound <= (text as NSString).length }
        }
        context.coordinator.highlight(tv)
    }

    static let font = NSFont.monospacedSystemFont(ofSize: 11.5, weight: .regular)

    @MainActor
    final class Coordinator: NSObject, NSTextViewDelegate {
        var parent: CodeEditor
        init(_ parent: CodeEditor) { self.parent = parent }

        func textDidChange(_ notification: Notification) {
            guard let tv = notification.object as? NSTextView else { return }
            parent.text = tv.string
            highlight(tv)
        }

        private static let keywords = try! NSRegularExpression(  // swiftlint:disable:this force_try
            pattern: #"\b(behavior|intent|var|on|end|if|then|elif|else|every|after|repeat|times|move|by|toward|at|rotate|look|emit|to|destroy|log|stop|let|set|and|or|not|seconds?)\b"#)
        private static let literals = try! NSRegularExpression(pattern: #"\b(true|false|none|self|dt|time|frame|pi|start|tick|event|key|click)\b"#)  // swiftlint:disable:this force_try
        private static let numbers = try! NSRegularExpression(pattern: #"\b\d+(\.\d+)?\b"#)  // swiftlint:disable:this force_try
        private static let strings = try! NSRegularExpression(pattern: #""[^"\n]*"?|'[^'\n]*'?"#)  // swiftlint:disable:this force_try
        private static let colors = try! NSRegularExpression(pattern: #"#(?:[0-9a-fA-F]{8}|[0-9a-fA-F]{6}|[0-9a-fA-F]{3,4})\b"#)  // swiftlint:disable:this force_try
        private static let comments = try! NSRegularExpression(pattern: #"(--|//|#(?=\s)).*$"#, options: [.anchorsMatchLines])  // swiftlint:disable:this force_try
        private static let calls = try! NSRegularExpression(pattern: #"\b[a-z_][a-zA-Z0-9_]*(?=\()"#)  // swiftlint:disable:this force_try

        func highlight(_ tv: NSTextView) {
            guard let storage = tv.textStorage else { return }
            let s = storage.string
            let full = NSRange(location: 0, length: (s as NSString).length)
            storage.beginEditing()
            storage.setAttributes([.font: CodeEditor.font, .foregroundColor: NSColor(Theme.text)], range: full)
            func paint(_ re: NSRegularExpression, _ color: Color, bold: Bool = false) {
                for m in re.matches(in: s, range: full) {
                    storage.addAttribute(.foregroundColor, value: NSColor(color), range: m.range)
                    if bold {
                        storage.addAttribute(.font, value: NSFont.monospacedSystemFont(ofSize: 11.5, weight: .semibold), range: m.range)
                    }
                }
            }
            paint(Self.calls, Color(hex: "#7cc7ff"))
            paint(Self.keywords, Color(hex: "#c792ea"), bold: true)
            paint(Self.literals, Color(hex: "#f2b44c"))
            paint(Self.numbers, Color(hex: "#f78c6c"))
            paint(Self.colors, Color(hex: "#89ddff"))
            paint(Self.strings, Color(hex: "#a5d97a"))
            paint(Self.comments, Theme.textFaint)
            // Mark error lines with a subtle red background.
            if !parent.errorLines.isEmpty {
                var line = 1
                (s as NSString).enumerateSubstrings(in: full, options: [.byLines, .substringNotRequired]) { _, range, enclosing, _ in
                    if self.parent.errorLines.contains(line) {
                        storage.addAttribute(.backgroundColor, value: NSColor(Theme.error.opacity(0.18)), range: enclosing)
                    }
                    _ = range
                    line += 1
                }
            }
            storage.endEditing()
        }
    }
}
