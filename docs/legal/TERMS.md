# Skywalker Terms of Use

> **Template prepared for legal review — not legal advice.** Text in `{{DOUBLE_BRACES}}` is a
> placeholder that must be filled in, and the whole document reviewed by a qualified lawyer for
> the countries where Skywalker is offered, before it is published or relied on.

Version: 1.0
Effective date: {{EFFECTIVE_DATE}}

## In plain language

This summary helps you read the terms. It does not replace them.

- **The license comes first.** What you may do with Skywalker's code (use, modify, ship games,
  when you need a commercial license) is set by the `LICENSE` file. These terms add the rules
  for using the software and its AI features.
- **Your games are yours.** We claim no rights in your projects, assets, scripts or games, and we
  take no royalties under the free grant.
- **We do not collect your data.** The software has no telemetry, analytics, accounts or ads, and
  sends nothing to us. Data leaves your computer only to the services you choose: the AI
  providers you configure, the websites you download assets from.
- **AI agents are AI.** They can be wrong and they can change your project. Review what they do,
  keep backups or version control, and follow the terms of the AI provider you use.
- **Use it lawfully.** No malware, no illegal content, no infringing assets, no prohibited AI
  practices, no deceptive deepfakes.
- **The software is provided "as is"**, but nothing here limits liability that the law does not
  allow us to limit, or the rights you have as a consumer.
- **Changes are announced.** A new version of these terms comes with a new version of the
  software, and the editor asks you to accept it before you continue. The software you already
  have stays under the terms you accepted for it.

## 1. Who we are and what these terms cover

1.1 These Terms of Use ("**Terms**") are an agreement between you and {{LICENSOR_LEGAL_NAME}},
{{POSTAL_ADDRESS}} ({{COMPANY_REGISTRATION}}) ("**we**", "**us**", the "**Licensor**").

1.2 They apply to your use of the Skywalker software: the editor application, the `skywalker`
command-line tool, the `skywalker-player` game runtime, the engine libraries and SDK, the agent
integrations, and their documentation, in every version we publish (together, the
"**Software**").

1.3 "**You**" means the person using the Software. If you use it on behalf of a company or other
organization, you accept these Terms for that organization and confirm that you are authorized
to do so; "you" then also means that organization.

1.4 A "**consumer**" is a natural person acting for purposes outside their trade, business, craft
or profession. Several sections give consumers additional protection, and nothing in these
Terms takes away rights that consumers have under the mandatory law of the country where they
live.

## 2. How these Terms relate to the License

2.1 The Software is licensed under the Business Source License 1.1 with an Additional Use Grant,
in the `LICENSE` file that ships with every copy (the "**License**"), or under a separate
commercial license agreement where you have one (the "**Commercial License**"). The License or
the Commercial License alone defines your rights to copy, modify, distribute and make production
use of the Software, including the revenue and funding thresholds and the Change License.

2.2 These Terms add the rules for using the Software and its AI features. They do not narrow the
rights the License grants, and they do not apply to a version of the Software after it has
converted to the Change License (Apache License 2.0), except where you keep using our AI-feature
integrations with that version.

2.3 If these Terms and the License or the Commercial License conflict, the License or the
Commercial License prevails on use rights, and these Terms prevail on everything else.

## 3. Accepting these Terms

3.1 The editor asks you to accept these Terms, and to acknowledge the Privacy Notice, the first
time you open it and whenever their version changes. The checkbox is never pre-ticked. You can
read both documents at any time in the editor (Help menu, and Settings ▸ Legal), in
`docs/legal/`, and with `skywalker legal`.

3.2 For automated and headless use (scripts, continuous integration, agent servers), you accept
these Terms by running `skywalker legal --accept` or by using the Software after having had the
opportunity to read them. Automation is never blocked waiting for acceptance.

3.3 An AI agent cannot accept these Terms on your behalf. Acceptance is always an action of a
person.

3.4 You must be at least 16 years old to accept these Terms. If you are younger than the age of
majority where you live, a parent or legal guardian should read them with you and agree to them
on your behalf.

## 4. Your content and your games

4.1 You keep all rights in the projects, scenes, scripts, assets, prompts, games and other
material you create, import or generate with the Software ("**Your Content**"). We claim no
ownership of Your Content and need no license to it, because the Software does not send it to
us.

4.2 You are responsible for Your Content and for having the rights you need in it, including in
third-party assets, fonts, music and models you import or download, and in AI outputs you use.

4.3 Contributions to the Skywalker source code are governed by `CONTRIBUTING.md`, not by this
section.

## 5. Acceptable use

5.1 You must use the Software in compliance with the law that applies to you, the License, and
these Terms.

5.2 You must not use the Software, including its AI agents, to:

1. create, store or distribute content that is illegal where you make it available, including
   child sexual abuse material, content that incites violence or terrorism, or unlawful hate
   speech;
2. infringe intellectual property, privacy, publicity or other rights of others, including by
   downloading or shipping assets without a license that permits your use;
3. develop or distribute malware, or attack, probe or overload systems, networks or services you
   are not authorized to test;
4. place on the market or use an AI system for a practice prohibited by Article 5 of the EU
   Artificial Intelligence Act (Regulation (EU) 2024/1689) or equivalent law, such as
   manipulative techniques that cause significant harm, exploiting the vulnerabilities of
   children or other vulnerable groups, or social scoring;
5. create deepfakes, or synthetic text, audio or images of real people, without the disclosures
   and consents the law requires, or to deceive people about who or what they are interacting
   with;
6. get around the safety measures, usage limits or terms of an AI provider, or the approval
   prompts of the Software for actions that reach outside your project;
7. process other people's personal data without a legal basis; or
8. breach export control or sanctions law (section 13).

5.3 You remain responsible for every action that an AI agent or automated client takes through
the Software at your direction, including through the agent socket and the MCP server.

## 6. AI features

6.1 **What they are.** The Software can connect AI agents to your project: the in-editor crew,
the studio runner (`skywalker studio run`), and external AI clients that you connect through the
MCP server or the agent socket. Agents act only through the Software's tools. Tools that reach
outside your project, such as downloading assets or starting design applications, ask for your
approval before they run, unless you have explicitly allowed them for an agent. The AI features are optional; the rest of the Software works without
them.

6.2 **Third-party AI providers.** The Software does not include an AI model. It sends requests to
the provider you configure (for example Anthropic, OpenAI, DeepSeek, or a model running on your
own computer), with an account and API key that you obtain from that provider. That provider's
terms of service, usage policies and privacy policy apply to those requests, and you are
responsible for complying with them and for any fees they charge. We are not a party to your
relationship with the provider and are not responsible for its availability, behavior, outputs or
data handling.

6.3 **Outputs can be wrong.** AI output can be inaccurate, incomplete, insecure, biased, or similar
to existing works. It is not professional, legal or safety advice. You are responsible for
reviewing and testing what agents produce and change before you rely on it, publish it or ship it.
Agents can modify and delete project files: use version control or backups. Scene edits by
agents can be undone in the editor's history.

6.4 **Transparency.** In line with the transparency obligations of the EU Artificial Intelligence
Act (Article 50):

1. the Software identifies AI agents as AI: agents are labelled as agents in the editor, and every
   change they make is attributed to the agent by name in the edit history and the activity log;
2. the Software supports marking AI-generated content: generated and downloaded assets carry
   provenance in their `.meta` files (for example the generator, the prompt and the license), which
   you can keep, extend and export; and
3. where you publish or ship content generated or manipulated with AI, you are responsible for the
   disclosures that apply to you, for example telling players when they interact with an AI system
   in your game, and labelling deepfakes and AI-generated text published to inform the public.

6.5 **Your instructions.** You decide which provider an agent uses, what it may see (including
whether viewport screenshots are sent), and which actions you approve. Do not send personal data
or confidential information to a provider unless you are allowed to.

## 7. Third-party components and content

7.1 The Software includes open-source components under their own licenses, listed in
`docs/LICENSING.md` (Help ▸ Licenses in the editor). Packaged games carry the required notices in
`Contents/Resources/Licenses/`. Those licenses govern those components, and nothing in these
Terms restricts the rights they give you.

7.2 Design applications (such as Blender), AI providers, asset libraries and websites are third
parties. The Software starts or contacts them at your request; their own licenses and terms
apply. Assets you download keep their own licenses, which the Software records in `CREDITS.md`
and the asset metadata; following them is your responsibility.

## 8. Privacy

How the Software handles data is explained in the Skywalker Privacy Notice
(`docs/legal/PRIVACY.md`). In short: the Software collects nothing for us; data goes only where you
send it.

## 9. Updates and availability

9.1 We may publish new versions of the Software, but do not have to, except where mandatory law
requires us to provide updates (including security updates) to consumers for a period of time.
We will provide those updates for the period the law requires.

9.2 The Software runs on your computer and needs no account or connection to us. The AI features
need the third-party services you configure, which may change or stop.

## 10. Warranty

10.1 Subject to section 11 and to your statutory rights, the Software is provided "as is" and "as
available", without warranties of any kind, express or implied, including warranties of
merchantability, fitness for a particular purpose, and non-infringement, to the extent the law
allows them to be excluded.

10.2 **Consumers** keep their statutory rights, including, where it applies to the Software, the
legal guarantee of conformity under the law of their country (for example the national rules
implementing the EU Digital Content Directive (EU) 2019/770). Nothing in these Terms limits those
rights.

## 11. Liability

11.1 **Unlimited liability.** Nothing in these Terms excludes or limits our liability:

1. for intent (wilful misconduct) or gross negligence;
2. for death or personal injury, or damage to health;
3. for fraud or fraudulent misrepresentation;
4. under mandatory product liability law, including the national laws implementing the EU Product
   Liability Directive (EU) 2024/2853, which covers software;
5. under a guarantee we have expressly given; or
6. for anything else for which the applicable law does not allow liability to be excluded or
   limited.

11.2 **Slight negligence.** Otherwise, where we are liable for slight (ordinary) negligence, we are
liable only for breach of an obligation whose fulfilment is essential to the proper performance of
these Terms and on which you may regularly rely, and only for the damage that was typical and
foreseeable when you accepted these Terms.

11.3 **Software provided free of charge.** Where you receive the Software free of charge and the
applicable law allows it, we are liable only in the cases of section 11.1.

11.4 **Business users.** If you are not a consumer, then, except in the cases of section 11.1 and to
the extent permitted by law: (a) we are not liable for loss of profits, revenue, data or goodwill,
or for indirect or consequential damage; and (b) our total liability arising out of or in
connection with these Terms is limited to the greater of the fees you paid us for the Software in
the twelve months before the event giving rise to the claim and EUR 100. A Commercial License may
set different limits.

11.5 **Consumers.** If you are a consumer, sections 11.2 to 11.4 apply only as far as the mandatory
law of your country of residence allows. If you live in the United Kingdom, nothing in these
Terms limits liability that cannot be limited under the Consumer Rights Act 2015.

11.6 **Your duty to mitigate.** You are responsible for keeping backups of your projects, a
reasonable precaution given that AI agents and other tools can change files. Where the law allows,
we are not liable for data loss that a reasonable backup would have prevented.

11.7 The limitations in this section also protect our employees, contractors and representatives.

## 12. Indemnity (business users only)

If you are not a consumer, you will indemnify us against third-party claims, and the reasonable
costs of defending them, arising from Your Content or from your breach of these Terms or of the
law, except to the extent the claim is caused by us. We will tell you promptly about such a
claim and let you control its defense, and we will not settle it without your consent.

## 13. Export controls and sanctions

13.1 The Software may be subject to export control and sanctions law, including the EU Dual-Use
Regulation (EU) 2021/821, EU, UN and UK sanctions, and the U.S. Export Administration Regulations
and sanctions administered by the U.S. Office of Foreign Assets Control.

13.2 You must not use, export or re-export the Software, or make games built with it available, in
breach of those laws, including to persons on a sanctions list or for prohibited end uses such as
weapons of mass destruction. By accepting these Terms you confirm that you are not such a person
and are not acting for one.

## 14. Changes to these Terms

14.1 We may change these Terms for a valid reason, such as a change in the law, a new feature, or
a security need. Every change gets a new version number and effective date, and we keep earlier
versions available in the Skywalker repository history.

14.2 A new version applies to the versions of the Software released with it or after it. The
editor shows it and asks you to accept it before you continue using the new version. If you do
not accept, you may keep using the version you already have under the Terms you accepted for it,
subject to the License.

14.3 We will not change these Terms in a way that removes rights you already have for a version
you already received, unless the law requires the change.

## 15. Termination

15.1 You may stop using the Software at any time. Deleting the Software and the files listed in
the Privacy Notice removes everything it stored on your computer.

15.2 If you materially breach these Terms and do not cure the breach within 30 days after we have
notified you (where a cure is possible), your right to use the Software under these Terms ends.
Your rights under the License end as the License provides.

15.3 Sections 4, 7, 10, 11, 12, 13, 16 and 17 survive termination.

## 16. Governing law and disputes

16.1 These Terms are governed by the law of {{GOVERNING_LAW_COUNTRY}}, excluding its conflict-of-law
rules and the UN Convention on Contracts for the International Sale of Goods.

16.2 **Consumers** additionally keep the protection of the mandatory provisions of the law of the
country where they habitually reside (for consumers in the EU, Article 6(2) of the Rome I
Regulation (EC) No 593/2008).

16.3 The courts of {{VENUE}} have jurisdiction over disputes with business users. **Consumers** may
bring proceedings in the courts of their country of residence, and we may bring proceedings
against a consumer only in those courts (for consumers in the EU, Article 18 of the Brussels Ia
Regulation (EU) No 1215/2012).

16.4 {{ADR_STATEMENT}}

## 17. General

17.1 These Terms, the License and any Commercial License are the whole agreement between you and
us about the Software, and replace earlier statements about it.

17.2 If a provision of these Terms is invalid or unenforceable, the rest stays in effect, and the
provision is replaced by the applicable statutory rules.

17.3 Not enforcing a provision is not a waiver of it.

17.4 We may transfer this agreement to a successor of our business in the Software. We will tell
you, and the transfer will not reduce your rights. You may not transfer it without our consent,
except together with your rights under the License.

17.5 These Terms are written in English. A translation is for convenience; where a consumer has a
right to rely on a translation in their own language, that right is not affected.

## 18. Contact

{{LICENSOR_LEGAL_NAME}}
{{POSTAL_ADDRESS}}
Email: {{CONTACT_EMAIL}}

Commercial licensing questions go to the same address.
