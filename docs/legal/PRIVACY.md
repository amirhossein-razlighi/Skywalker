# Skywalker Privacy Notice

> **Template prepared for legal review — not legal advice.** Text in `{{DOUBLE_BRACES}}` is a
> placeholder that must be filled in, and the whole document reviewed by a qualified lawyer, before
> it is published or relied on.

Version: 1.0
Effective date: {{EFFECTIVE_DATE}}

## In plain language

- **Skywalker sends nothing to us.** No telemetry, analytics, crash reports, update checks,
  accounts, advertising or tracking. We have checked the source of this version to confirm it.
- **Your projects stay on your computer.** API keys are kept in the macOS Keychain.
- **Data leaves your computer only when you send it:** to the AI provider you configure when you
  run an agent, or to a website when you approve an asset download. Those services handle it under
  their own privacy policies.
- **Games made with Skywalker collect nothing** unless their developer adds that. Developers are
  responsible for their own games' privacy (see the last section).
- This notice asks you to **acknowledge** it. We do not ask for consent, because we do not process
  your data on the basis of consent.

## 1. Who is responsible

1.1 The Skywalker software (the editor, the `skywalker` command-line tool, the `skywalker-player`
runtime and the engine libraries, the "**Software**") is published by {{LICENSOR_LEGAL_NAME}},
{{POSTAL_ADDRESS}} ("**we**", "**us**").

1.2 For personal data that we receive ourselves, for example when you email us or ask for a
commercial license, we are the controller under the EU and UK General Data Protection Regulation
("**GDPR**") and a "business" under the California Consumer Privacy Act as amended ("**CCPA**")
where it applies. Contact: {{PRIVACY_EMAIL}}.

1.3 Representative in the EU (Article 27 GDPR, where required): {{EU_REPRESENTATIVE}}.
Representative in the UK (where required): {{UK_REPRESENTATIVE}}.
Data protection officer: {{DPO_CONTACT}}.

1.4 The Software runs on your computer, and we do not receive what it processes. For that data,
**you** decide what happens to it. If you use the Software for purely personal activities, data
protection law generally does not apply to that use (Article 2(2)(c) GDPR). If you use it for a
business, your organization is the controller of the personal data in its projects.

## 2. What the Software does not do

As of this version, verified in the source code:

- no telemetry, usage statistics or analytics;
- no crash reports sent anywhere (the game player writes crash logs only to your own disk);
- no automatic update checks, license checks or "phone home" connections;
- no user accounts, sign-in or device identifiers;
- no advertising, tracking, fingerprinting, or sale or sharing of personal information;
- no connection to us at all.

If a future version adds any of these, it will be optional, described in a new version of this
notice, and shown to you before it takes effect.

## 3. Data the Software keeps on your computer

| What | Where | Notes |
|---|---|---|
| Your projects | The project folder you choose | Scenes, scripts, assets and their `.meta` provenance, `CREDITS.md`, the studio board, messages, usage counts (`studio/`), playtest screenshots, agent definitions (`agents/`). |
| AI provider settings | `~/Library/Application Support/Skywalker/crew.json` | Provider names, endpoint URLs and model names. No API keys. |
| API keys | macOS Keychain, service `dev.skywalker.editor` | Encrypted by macOS, readable after the first unlock, not synchronized to iCloud Keychain. The command-line tool reads keys only from environment variables and never stores them. |
| Preferences | macOS user defaults (`dev.skywalker.editor`) | Last project folder, whether external agents may connect, and your acceptance of the terms. |
| Acceptance record | `~/Library/Application Support/Skywalker/legal-acceptance.json` | Which versions of the Terms and this notice you accepted and acknowledged, when, and from which tool. Stays on your computer as your own proof of acceptance. |
| Caches and backups | `~/.skywalker/` and `.skywalker/` inside projects | Rendering caches, design-app bridge settings and backups, backups made by `skywalker setup`. |
| Agent socket | `~/.skywalker/editor.sock` | A local Unix socket that lets AI clients on your computer drive the editor. Only your user account can open it; it cannot be reached over the network. Turn it off in Settings ▸ External Agents. |
| Game crash logs | `~/Library/Logs/SkywalkerGames/` | Written by games when they crash: a program backtrace. Never sent anywhere. |

## 4. Data that leaves your computer, and only when you send it

4.1 **AI providers.** When you run an in-editor agent or `skywalker studio run`, the Software sends
a request to the endpoint configured for that agent's provider. A request can contain:

- the agent's instructions and your messages, and the conversation so far;
- tool calls and their results, which can include scene contents, file contents, script code,
  logs, names you gave to entities and assets, and anything else in your project that the agent
  reads;
- viewport screenshots, if the model is marked as able to see images;
- your API key (for authentication), and technical data any web request carries: your IP address
  and a client identifier naming the Software and its version (and, from the editor, the macOS
  networking version).

The provider receives and processes this data under its own terms and privacy policy, as your
processor or as an independent controller, depending on your contract with it. We are not involved
and receive nothing. Before you use a provider, read its privacy policy, data-retention and
model-training settings, and, if you are a business, its data processing agreement. Models that run
on your own computer (for example through Ollama or LM Studio) keep the data on your computer.

AI clients that you connect to the Software yourself (through `skywalker mcp` or the agent socket)
send data to their own providers under their own terms.

4.2 **Asset downloads.** When an agent downloads an asset with your approval, the Software
fetches the address it names. That website sees your IP address, the client identifier
`Skywalker/<version>` and the address requested, and handles them under its own policy.

4.3 **Design applications.** When you use the design-app bridge, the Software starts the
applications installed on your computer (such as Blender) and talks to them only over your own
computer's loopback interface.

## 5. Why we process data, and on what legal basis

5.1 The Software processes data on your computer to provide the features you use. We do not
receive that data and do not process it.

5.2 For data we receive ourselves:

| Purpose | Data | Legal basis (GDPR) |
|---|---|---|
| Answering your questions and support requests | Name, email address, message contents | Our legitimate interest in answering you (Art. 6(1)(f)); steps before a contract where you ask about one (Art. 6(1)(b)) |
| Commercial licenses | Contact and company details, billing data | Contract (Art. 6(1)(b)); legal obligations such as tax records (Art. 6(1)(c)) |
| Contributions to the source code | Name, email and content of your contribution and sign-off | Our legitimate interest in maintaining the project and a record of contribution rights (Art. 6(1)(f)) |
| Legal claims | Data relevant to the claim | Our legitimate interest in establishing, exercising or defending legal claims (Art. 6(1)(f)) |

5.3 We do not rely on consent, do not make decisions based solely on automated processing that
have legal or similarly significant effects (Article 22 GDPR), and do not process special
categories of personal data.

## 6. How long data is kept

6.1 Data on your computer stays until you delete it. To remove everything the Software stored:
delete your project folders, `~/Library/Application Support/Skywalker/`, `~/.skywalker/` and
`~/Library/Logs/SkywalkerGames/`; remove the Keychain items of the service `dev.skywalker.editor`
in Keychain Access; and run `defaults delete dev.skywalker.editor`.

6.2 We keep correspondence for {{CORRESPONDENCE_RETENTION}} after our last exchange, contract and
billing records for as long as tax and commercial law requires, and data needed for a legal claim
until the claim is resolved.

## 7. Your rights

7.1 Under the GDPR you have the right to access your personal data, to have it corrected or erased,
to restrict its processing, to data portability, and to object to processing based on legitimate
interests (Articles 15 to 21). Write to {{PRIVACY_EMAIL}}. We answer within one month, which the
law allows us to extend in some cases.

7.2 You also have the right to lodge a complaint with a data protection supervisory authority, in
particular in the country where you live or work. Our lead authority is {{SUPERVISORY_AUTHORITY}}.
In the UK this is the Information Commissioner's Office.

7.3 **California and other U.S. states.** Residents have the right to know what personal
information we collect, to have it deleted or corrected, and not to be discriminated against for
exercising these rights; you may use an authorized agent. The only categories we collect are
identifiers and the contents of communications you send us, for the purposes in section 5. We do
not sell or share personal information, and do not use or disclose sensitive personal information,
so there is nothing to opt out of. Contact: {{PRIVACY_EMAIL}}.

7.4 Because the Software sends us nothing, we hold no data about your use of it. Requests about the
data an AI provider or website received from you go to that provider or website.

## 8. International transfers

8.1 The Software does not transfer data to us. When you use a cloud AI provider, your data may be
processed in countries outside the European Economic Area or the United Kingdom, including the
United States. That transfer happens at your direction, under the provider's terms. Providers
commonly rely on the EU Commission's adequacy decisions (such as the EU-U.S. Data Privacy Framework
for certified companies), the EU Standard Contractual Clauses (Commission Implementing Decision (EU)
2021/914), or the UK International Data Transfer Agreement or Addendum. If you transfer personal
data of others, make sure such a safeguard is in place, for example in the provider's data
processing agreement.

8.2 Where we transfer data we receive ourselves outside the EEA or the UK, for example to an email
service provider, we use one of these safeguards. {{TRANSFER_DETAILS}}

## 9. Children

The Software is a professional tool. It is not directed at children under 16, and we do not
knowingly collect personal data from them. If you believe a child has sent us personal data, contact
us and we will delete it.

## 10. Security

- API keys are kept in the macOS Keychain, never in project files or settings files.
- The agent socket is restricted to your user account; the design-app bridge listens only on the
  loopback interface.
- Tools that reach outside your project (downloads, design applications, building and running
  games) are marked as such: the in-editor crew and the studio runner ask for your approval before
  running them unless you have explicitly allowed them for that agent, and connected AI clients are
  told to ask you. Downloads require a usable license and are
  size-limited.
- Requests to cloud AI providers use HTTPS by default. If you configure a plain `http://` endpoint,
  use it only for a model on your own computer or a network you trust.

No software is perfectly secure. Keep macOS and the Software up to date, and review the agents you
connect.

## 11. Changes to this notice

Every change gets a new version number and effective date. The editor shows a new version and asks
you to acknowledge it before you continue. Earlier versions remain available in the Skywalker
repository history.

## 12. Contact

{{LICENSOR_LEGAL_NAME}}
{{POSTAL_ADDRESS}}
Privacy questions: {{PRIVACY_EMAIL}}
Other questions: {{CONTACT_EMAIL}}

## For developers who ship games

This section is for you if you build and distribute games with Skywalker.

**What the runtime does.** `skywalker-player`, the runtime inside every app that `skywalker build`
packages, collects nothing: no telemetry, analytics, advertising or network connections of its own,
and no terms or consent screens. It writes crash logs only to the player's own disk
(`~/Library/Logs/SkywalkerGames/`). The `--agent-socket` option is a development aid; do not ship a
game that starts with it.

**Who is responsible.** Your game is your product. You are the controller (and, under the CCPA,
the business) for any personal data your game processes. We are neither the controller nor a
processor for your games, and the Skywalker Terms and this notice are not shown to your players.

**Checklist if your game adds networking, accounts, multiplayer, analytics, crash reporting, ads or
AI features.** Native modules, scripts and third-party SDKs you add can change what the game
collects. Then you must, as applicable:

1. Publish a privacy policy for the game and link it in the game and its store listing: who you
   are, what you collect, why and on which legal basis, recipients and processors, international
   transfers, retention, players' rights, and how to contact you.
2. Ask for consent before non-essential storage on or access to the player's device, such as
   analytics identifiers or advertising IDs (EU ePrivacy Directive, Article 5(3)), and for any
   processing based on consent. Never pre-tick boxes, and make refusing as easy as accepting.
3. Protect children. If your game is directed at children or you know a player is a child, follow
   the U.S. Children's Online Privacy Protection Act (verifiable parental consent under 13), the
   GDPR digital-consent ages (13 to 16 depending on the country, Article 8) and the UK Age
   Appropriate Design Code, with privacy-protective defaults.
4. Sign a data processing agreement (Article 28 GDPR) with every vendor that processes players'
   data for you, and put a transfer safeguard in place for vendors outside the EEA or the UK.
5. Complete the platform disclosures: Apple's App Store privacy details and privacy manifest
   (including for third-party SDKs), App Tracking Transparency before tracking, Google Play's Data
   safety form, and the privacy requirements of other stores you use.
6. Honor players' rights requests (access, deletion, correction, objection), including deleting
   accounts from inside the game where a store requires it, and honor Global Privacy Control
   signals where U.S. state law requires.
7. Secure the data, keep a record of processing (Article 30 GDPR), assess high-risk processing
   (Article 35), and be ready to notify breaches within 72 hours (Articles 33 and 34).
8. If your game uses AI, tell players when they are interacting with an AI system unless it is
   obvious, and mark AI-generated content as the EU AI Act (Article 50) and other laws require.
9. If players can post content or chat with each other, set terms and a notice-and-action
   process, as the EU Digital Services Act requires of hosting services.
