import AppKit
import SwiftUI

/// What the acceptance sheet and the document window can show.
private enum LegalPage: String, CaseIterable, Identifiable {
    case summary, terms, privacy, license
    var id: String { rawValue }
    var title: String {
        switch self {
        case .summary: "Summary"
        case .terms: "Terms of Use"
        case .privacy: "Privacy Notice"
        case .license: "License"
        }
    }
}

/// First launch, and whenever the Terms or the Privacy Notice change: the editor stays behind this sheet until
/// the person accepts the Terms (unticked checkbox) or quits. The Privacy Notice is acknowledged, not consented
/// to: processing does not rely on consent.
struct LegalAcceptanceSheet: View {
    @Environment(LegalStore.self) private var legal
    @State private var page: LegalPage = .summary
    @State private var agreed = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            Picker("Document", selection: $page) {
                ForEach(LegalPage.allCases) { Text($0.title).tag($0) }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .padding(.horizontal, 16)
            .padding(.vertical, 8)
            .accessibilityLabel("Document to read")
            Divider()
            ScrollView {
                pageContent.padding(16)
            }
            .background(Theme.panel)
            .id(page)  // start each document at the top
            Divider()
            footer
        }
        .frame(width: 760, height: 640)
        .background(Theme.window)
        .interactiveDismissDisabled()
    }

    private var header: some View {
        HStack(alignment: .top, spacing: 12) {
            SkywalkerMark(size: 22)
            VStack(alignment: .leading, spacing: 3) {
                Text(legal.isUpdate ? "Skywalker's terms have changed" : "Welcome to Skywalker")
                    .font(.system(size: 15, weight: .semibold))
                    .accessibilityAddTraits(.isHeader)
                Text(legal.isUpdate
                     ? "Please review the updated documents before you continue."
                     : "Before you start, please review the Terms of Use and the Privacy Notice.")
                    .font(Theme.body).foregroundStyle(Theme.textDim)
            }
            Spacer()
            VStack(alignment: .trailing, spacing: 2) {
                Text("Terms \(legal.current.terms)").accessibilityLabel("Terms of Use version \(legal.current.terms)")
                Text("Privacy \(legal.current.privacy)").accessibilityLabel("Privacy Notice version \(legal.current.privacy)")
            }
            .font(Theme.monoSmall).foregroundStyle(Theme.textFaint)
        }
        .padding(16)
        .background(Theme.header)
    }

    @ViewBuilder
    private var pageContent: some View {
        switch page {
        case .summary: LegalSummary(onOpen: { page = $0 })
        case .terms: MarkdownText(LegalDocument.terms.text)
        case .privacy: MarkdownText(LegalDocument.privacy.text)
        case .license: LicenseText()
        }
    }

    private var footer: some View {
        VStack(alignment: .leading, spacing: 10) {
            Toggle(isOn: $agreed) {
                Text("I have read and agree to the Terms of Use").font(Theme.body)
            }
            .toggleStyle(.checkbox)
            .accessibilityHint("Required to use Skywalker")
            HStack(alignment: .firstTextBaseline, spacing: 4) {
                Image(systemName: "info.circle").foregroundStyle(Theme.textFaint).accessibilityHidden(true)
                Text("Accepting also confirms that you have read the Privacy Notice. It explains how data is handled; it does not ask you to consent to anything. Skywalker sends no data to us.")
                    .font(Theme.label).foregroundStyle(Theme.textDim)
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack {
                Button("View Privacy Notice") { page = .privacy }.buttonStyle(.link)
                Button("View License") { page = .license }.buttonStyle(.link)
                Spacer()
                Text(LegalStore.developerCredit).font(Theme.label).foregroundStyle(Theme.textFaint)
                Spacer()
                Button("Quit") { NSApp.terminate(nil) }
                    .accessibilityHint("Quits Skywalker without accepting")
                Button("Accept") { legal.accept() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!agreed)
                    .accessibilityHint(agreed ? "Accepts the Terms of Use and opens the editor" : "Tick the checkbox first")
            }
        }
        .padding(16)
        .background(Theme.header)
    }
}

/// The plain-language summaries of the Terms and the Privacy Notice, with links to the full texts.
private struct LegalSummary: View {
    let onOpen: (LegalPage) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            section("Terms of Use, in short", text: LegalText.summary(of: LegalDocument.terms.text), page: .terms)
            section("Privacy, in short", text: LegalText.summary(of: LegalDocument.privacy.text), page: .privacy)
        }
    }

    private func section(_ title: String, text: String, page: LegalPage) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(title).font(Theme.sectionTitle).foregroundStyle(Theme.textDim).accessibilityAddTraits(.isHeader)
                Spacer()
                Button("Read the full \(page.title)") { onOpen(page) }.buttonStyle(.link).font(Theme.label)
            }
            MarkdownText(text)
        }
    }
}

/// The LICENSE (plain text) followed by the plain-language explanation and third-party notices.
private struct LicenseText: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            MarkdownText(LegalDocument.licensing.text)
            Divider()
            Text(LegalDocument.license.text)
                .font(Theme.monoSmall).foregroundStyle(Theme.text)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

/// Help ▸ Terms of Use / Privacy Notice / Licenses, and Settings ▸ Legal: one document per window.
struct LegalDocumentWindow: View {
    @State var document: LegalDocument

    var body: some View {
        VStack(spacing: 0) {
            Picker("Document", selection: $document) {
                ForEach(LegalDocument.allCases) { Label($0.title, systemImage: $0.symbol).tag($0) }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .padding(8)
            .background(Theme.header)
            Divider()
            ScrollView {
                Group {
                    if document.isMarkdown {
                        MarkdownText(document.text)
                    } else {
                        Text(document.text).font(Theme.monoSmall).textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                .padding(16)
            }
            .id(document)
            .background(Theme.panel)
        }
        .frame(minWidth: 560, minHeight: 420)
        .navigationTitle(document.title)
    }
}

struct LegalCommands: Commands {
    @Environment(\.openWindow) private var openWindow

    var body: some Commands {
        CommandGroup(after: .help) {
            Divider()
            Button("Terms of Use") { openWindow(id: LegalWindow.id, value: LegalDocument.terms) }
            Button("Privacy Notice") { openWindow(id: LegalWindow.id, value: LegalDocument.privacy) }
            Button("Licenses") { openWindow(id: LegalWindow.id, value: LegalDocument.licensing) }
        }
    }
}

enum LegalWindow {
    static let id = "legal"
}

/// Settings ▸ Legal: what was accepted and when, the documents, and where the proof of acceptance lives.
struct LegalSettings: View {
    @Environment(LegalStore.self) private var legal
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        Form {
            Section("Accepted on this Mac") {
                LabeledContent("Terms of Use", value: status(legal.acceptedTerms, current: legal.current.terms))
                LabeledContent("Privacy Notice (acknowledged)", value: status(legal.acknowledgedPrivacy, current: legal.current.privacy))
                LabeledContent("Date", value: legal.acceptedAt?.formatted(date: .abbreviated, time: .shortened) ?? "—")
                LabeledContent("Record") {
                    HStack {
                        Text(LegalStore.recordURL.path).font(Theme.monoSmall).foregroundStyle(.secondary).textSelection(.enabled)
                            .lineLimit(1).truncationMode(.middle)
                        Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([LegalStore.recordURL]) }
                            .disabled(!FileManager.default.fileExists(atPath: LegalStore.recordURL.path))
                    }
                }
                if let error = legal.lastError { Text(error).font(.caption).foregroundStyle(Theme.warning) }
            }
            Section("Documents") {
                ForEach(LegalDocument.allCases) { doc in
                    Button { openWindow(id: LegalWindow.id, value: doc) } label: {
                        Label(doc.title, systemImage: doc.symbol)
                    }
                    .buttonStyle(.link)
                }
            }
            Section("About") {
                Text(LegalStore.developerCredit)
            }
            Section("Data") {
                Text("Skywalker sends no telemetry, analytics or crash reports, and nothing to its publisher. Data leaves this Mac only when you send it: to the AI providers you configure and to the websites of assets you approve for download. Games you build with Skywalker collect nothing unless you add it.")
                    .font(.caption).foregroundStyle(.secondary)
                Text("For automation: `skywalker legal` prints the documents, `skywalker legal --accept` records acceptance for headless use.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }

    private func status(_ accepted: String?, current: String) -> String {
        guard let accepted else { return "not accepted (current \(current))" }
        return accepted == current ? "\(accepted) (current)" : "\(accepted) (current is \(current))"
    }
}
