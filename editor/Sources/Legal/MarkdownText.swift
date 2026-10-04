import SwiftUI

/// The block structure of the markdown the legal documents use: headings, paragraphs, lists, quotes, tables
/// and code. Inline markup (bold, code, links) is left to `AttributedString(markdown:)`.
enum MarkdownBlock: Sendable {
    case heading(level: Int, text: String)
    case paragraph(String)
    case bullets([String])
    case numbered([(marker: String, text: String)])
    case quote(String)
    case table([[String]])
    case code(String)
    case rule

    /// Splits markdown into blocks. Hard-wrapped lines of a paragraph are joined with spaces; short lines (an
    /// address block) keep their line breaks.
    static func parse(_ markdown: String) -> [MarkdownBlock] {
        var blocks: [MarkdownBlock] = []
        var paragraph: [String] = []
        var quote: [String] = []
        var bullets: [String] = []
        var numbered: [(marker: String, text: String)] = []
        var table: [[String]] = []
        var code: [String]?

        func joined(_ lines: [String]) -> String {
            var out = ""
            for (i, line) in lines.enumerated() {
                if i > 0 { out += lines[i - 1].count < 60 ? "\n" : " " }
                out += line
            }
            return out
        }
        func flush() {
            if !paragraph.isEmpty { blocks.append(.paragraph(joined(paragraph))); paragraph = [] }
            if !quote.isEmpty { blocks.append(.quote(joined(quote))); quote = [] }
            if !bullets.isEmpty { blocks.append(.bullets(bullets)); bullets = [] }
            if !numbered.isEmpty { blocks.append(.numbered(numbered)); numbered = [] }
            if !table.isEmpty { blocks.append(.table(table)); table = [] }
        }

        for raw in markdown.components(separatedBy: "\n") {
            if var lines = code {
                if raw.hasPrefix("```") {
                    blocks.append(.code(lines.joined(separator: "\n")))
                    code = nil
                } else {
                    lines.append(raw)
                    code = lines
                }
                continue
            }
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("```") { flush(); code = []; continue }
            if line.isEmpty { flush(); continue }
            if line == "---" { flush(); blocks.append(.rule); continue }
            if line.hasPrefix("#"), let space = line.firstIndex(of: " "),
               line[..<space].allSatisfy({ $0 == "#" }) {
                flush()
                blocks.append(.heading(level: line.distance(from: line.startIndex, to: space), text: String(line[line.index(after: space)...])))
                continue
            }
            if line.hasPrefix(">") {
                if quote.isEmpty { flush() }
                quote.append(line.dropFirst().trimmingCharacters(in: .whitespaces))
                continue
            }
            if line.hasPrefix("|") {
                if table.isEmpty { flush() }
                let cells = line.trimmingCharacters(in: CharacterSet(charactersIn: "|")).components(separatedBy: "|")
                    .map { $0.trimmingCharacters(in: .whitespaces) }
                if !cells.allSatisfy({ !$0.isEmpty && $0.allSatisfy { "-:".contains($0) } }) { table.append(cells) }
                continue
            }
            if line.hasPrefix("- ") || line.hasPrefix("* ") {
                if bullets.isEmpty { flush() }
                bullets.append(String(line.dropFirst(2)))
                continue
            }
            if let dot = line.firstIndex(of: "."), dot > line.startIndex, line[..<dot].allSatisfy(\.isNumber),
               line.index(after: dot) < line.endIndex, line[line.index(after: dot)] == " " {
                if numbered.isEmpty { flush() }
                numbered.append((String(line[...dot]), String(line[line.index(dot, offsetBy: 2)...])))
                continue
            }
            // A continuation line of the current list item, or paragraph text.
            if raw.hasPrefix(" "), !bullets.isEmpty {
                bullets[bullets.count - 1] += " " + line
            } else if raw.hasPrefix(" "), !numbered.isEmpty {
                numbered[numbered.count - 1].text += " " + line
            } else {
                if !bullets.isEmpty || !numbered.isEmpty || !quote.isEmpty || !table.isEmpty { flush() }
                paragraph.append(line)
            }
        }
        if let lines = code { blocks.append(.code(lines.joined(separator: "\n"))) }
        flush()
        return blocks
    }
}

/// Renders markdown blocks in the editor's dense dark style, selectable for copying.
struct MarkdownText: View {
    let blocks: [MarkdownBlock]

    init(_ markdown: String) { blocks = MarkdownBlock.parse(markdown) }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ForEach(Array(blocks.enumerated()), id: \.offset) { _, block in
                view(for: block)
            }
        }
        .textSelection(.enabled)
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    @ViewBuilder
    private func view(for block: MarkdownBlock) -> some View {
        switch block {
        case let .heading(level, text):
            Text(inline(text))
                .font(level == 1 ? .system(size: 17, weight: .semibold) : level == 2 ? .system(size: 13.5, weight: .semibold) : Theme.body.weight(.semibold))
                .foregroundStyle(Theme.text)
                .padding(.top, level == 1 ? 0 : 6)
                .accessibilityAddTraits(.isHeader)
        case let .paragraph(text):
            Text(inline(text)).font(Theme.body).foregroundStyle(Theme.text).fixedSize(horizontal: false, vertical: true)
        case let .bullets(items):
            VStack(alignment: .leading, spacing: 4) {
                ForEach(Array(items.enumerated()), id: \.offset) { _, item in listRow(marker: "•", text: item) }
            }
        case let .numbered(items):
            VStack(alignment: .leading, spacing: 4) {
                ForEach(Array(items.enumerated()), id: \.offset) { _, item in listRow(marker: item.marker, text: item.text) }
            }
        case let .quote(text):
            Text(inline(text))
                .font(Theme.body).foregroundStyle(Theme.textDim)
                .fixedSize(horizontal: false, vertical: true)
                .padding(.vertical, 6).padding(.horizontal, 10)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Theme.warning.opacity(0.08), in: RoundedRectangle(cornerRadius: Theme.radius))
                .overlay(alignment: .leading) { Rectangle().fill(Theme.warning).frame(width: 2) }
        case let .table(rows):
            Grid(alignment: .topLeading, horizontalSpacing: 12, verticalSpacing: 6) {
                ForEach(Array(rows.enumerated()), id: \.offset) { index, row in
                    GridRow {
                        ForEach(Array(row.enumerated()), id: \.offset) { _, cell in
                            Text(inline(cell))
                                .font(index == 0 ? Theme.label.weight(.semibold) : Theme.label)
                                .foregroundStyle(index == 0 ? Theme.textDim : Theme.text)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    if index == 0 { Divider().gridCellUnsizedAxes(.horizontal) }
                }
            }
            .padding(8)
            .background(Theme.field, in: RoundedRectangle(cornerRadius: Theme.radius))
        case let .code(text):
            Text(text).font(Theme.mono).foregroundStyle(Theme.text)
                .padding(8).frame(maxWidth: .infinity, alignment: .leading)
                .background(Theme.field, in: RoundedRectangle(cornerRadius: Theme.radius))
        case .rule:
            Divider()
        }
    }

    private func listRow(marker: String, text: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 6) {
            Text(marker).font(Theme.body).foregroundStyle(Theme.textDim).frame(minWidth: 14, alignment: .trailing)
            Text(inline(text)).font(Theme.body).foregroundStyle(Theme.text).fixedSize(horizontal: false, vertical: true)
        }
    }

    private func inline(_ text: String) -> AttributedString {
        (try? AttributedString(markdown: text, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace))) ?? AttributedString(text)
    }
}
