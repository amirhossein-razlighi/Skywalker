import Foundation
import Observation

/// The legal documents bundled in Skywalker.app/Contents/Resources/Legal (copied from docs/legal, LICENSE and
/// docs/LICENSING.md at build time). The "Version:" lines of the Terms and the Privacy Notice decide when the
/// first-launch sheet asks again; the engine's `legal_info` tool and `skywalker legal` read the same lines.
enum LegalDocument: String, CaseIterable, Identifiable, Codable, Hashable, Sendable {
    case terms, privacy, license, licensing

    var id: String { rawValue }

    var title: String {
        switch self {
        case .terms: "Terms of Use"
        case .privacy: "Privacy Notice"
        case .license: "License"
        case .licensing: "Third-Party Licenses"
        }
    }

    var symbol: String {
        switch self {
        case .terms: "doc.text"
        case .privacy: "hand.raised"
        case .license: "checkmark.seal"
        case .licensing: "shippingbox"
        }
    }

    /// The license is plain text; everything else is markdown.
    var isMarkdown: Bool { self != .license }

    private var resource: (name: String, ext: String?) {
        switch self {
        case .terms: ("TERMS", "md")
        case .privacy: ("PRIVACY", "md")
        case .license: ("LICENSE", nil)
        case .licensing: ("LICENSING", "md")
        }
    }

    /// The bundled text, or an explanation when the app was built without it.
    var text: String {
        if let url = Bundle.main.url(forResource: resource.name, withExtension: resource.ext, subdirectory: "Legal"),
           let text = try? String(contentsOf: url, encoding: .utf8) {
            return text
        }
        return "# \(title)\n\nThis copy of Skywalker was built without its legal documents. They are in the Skywalker " +
            "repository (`docs/legal/`, `LICENSE`, `docs/LICENSING.md`), and `skywalker legal` prints them."
    }
}

/// Pure helpers shared by the views (kept free of UI so they are easy to reason about and test).
enum LegalText {
    /// Value of a "Key: value" line, e.g. `field("Version", in: text)`.
    static func field(_ key: String, in text: String) -> String? {
        let prefix = key + ":"
        for line in text.split(separator: "\n", omittingEmptySubsequences: false) where line.hasPrefix(prefix) {
            return line.dropFirst(prefix.count).trimmingCharacters(in: .whitespaces)
        }
        return nil
    }

    /// The body of the "## In plain language" section.
    static func summary(of text: String) -> String {
        guard let start = text.range(of: "## In plain language\n") else { return "" }
        let rest = text[start.upperBound...]
        let end = rest.range(of: "\n## ")?.lowerBound ?? rest.endIndex
        return rest[..<end].trimmingCharacters(in: .whitespacesAndNewlines)
    }
}

struct LegalVersions: Equatable, Sendable {
    var terms: String
    var privacy: String

    static func current() -> LegalVersions {
        LegalVersions(terms: LegalText.field("Version", in: LegalDocument.terms.text) ?? "unknown",
                      privacy: LegalText.field("Version", in: LegalDocument.privacy.text) ?? "unknown")
    }
}

/// Acceptance of the Terms of Use and acknowledgement of the Privacy Notice. Stored in UserDefaults (what the
/// editor checks on launch) and appended to a local JSON record (proof of acceptance, shared with
/// `skywalker legal --accept`). Nothing is sent anywhere.
@MainActor @Observable
final class LegalStore {
    nonisolated static let termsKey = "legal.acceptedTermsVersion"
    nonisolated static let privacyKey = "legal.acknowledgedPrivacyVersion"
    nonisolated static let dateKey = "legal.acceptedAt"
    nonisolated static let historyLimit = 100
    /// Shown in the acceptance sheet and Settings ▸ Legal (the About panel shows the copyright from Info.plist).
    nonisolated static let developerCredit = "Developed by: AmirHossein (Amir) Razlighi"

    let current = LegalVersions.current()
    private(set) var acceptedTerms: String?
    private(set) var acknowledgedPrivacy: String?
    private(set) var acceptedAt: Date?
    private(set) var lastError: String?

    init() {
        let defaults = UserDefaults.standard
        acceptedTerms = defaults.string(forKey: Self.termsKey)
        acknowledgedPrivacy = defaults.string(forKey: Self.privacyKey)
        acceptedAt = defaults.object(forKey: Self.dateKey) as? Date
    }

    var needsAcceptance: Bool {
        Self.needsAcceptance(terms: acceptedTerms, privacy: acknowledgedPrivacy, current: current)
    }

    /// True on first launch and whenever either document's version differs from what was accepted.
    nonisolated static func needsAcceptance(terms: String?, privacy: String?, current: LegalVersions) -> Bool {
        terms != current.terms || privacy != current.privacy
    }

    /// Whether this is a re-acceptance after a change (rather than the first launch).
    var isUpdate: Bool { acceptedTerms != nil }

    func accept(now: Date = Date()) {
        let defaults = UserDefaults.standard
        defaults.set(current.terms, forKey: Self.termsKey)
        defaults.set(current.privacy, forKey: Self.privacyKey)
        defaults.set(now, forKey: Self.dateKey)
        acceptedTerms = current.terms
        acknowledgedPrivacy = current.privacy
        acceptedAt = now
        do {
            try Self.appendRecord(versions: current, at: now, to: Self.recordURL)
            lastError = nil
        } catch {
            lastError = "Could not write \(Self.recordURL.path): \(error.localizedDescription)"
        }
    }

    /// ~/Library/Application Support/Skywalker/legal-acceptance.json, or $SKY_LEGAL_DIR like the CLI.
    nonisolated static var recordURL: URL {
        if let dir = ProcessInfo.processInfo.environment["SKY_LEGAL_DIR"], !dir.isEmpty {
            return URL(filePath: dir, directoryHint: .isDirectory).appending(path: "legal-acceptance.json")
        }
        return FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appending(path: "Skywalker/legal-acceptance.json")
    }

    /// Same layout as the engine's record (engine/src/legal/Legal.cpp): the latest acceptance at the top level
    /// plus a capped history.
    nonisolated static func appendRecord(versions: LegalVersions, at date: Date, to url: URL) throws {
        let iso = ISO8601DateFormatter()
        iso.formatOptions = [.withInternetDateTime]
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? ""
        let entry: [String: Any] = ["terms_version": versions.terms, "privacy_version": versions.privacy,
                                    "accepted_at": iso.string(from: date), "via": "editor", "app_version": version]
        var history: [[String: Any]] = []
        if let data = try? Data(contentsOf: url),
           let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           let previous = object["history"] as? [[String: Any]] {
            history = previous
        }
        history.append(entry)
        if history.count > historyLimit { history.removeFirst(history.count - historyLimit) }
        var record = entry
        record["schema"] = 1
        record["history"] = history
        let data = try JSONSerialization.data(withJSONObject: record, options: [.prettyPrinted, .sortedKeys])
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try data.write(to: url, options: .atomic)
    }
}
