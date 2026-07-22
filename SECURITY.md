# Security Policy

## Reporting a vulnerability

RCP is a protocol specification with reference SDKs. If you discover a
vulnerability — in the spec (e.g. an under-specified trust boundary), the schema,
or any of the reference SDKs — please report it privately.

**Do not open a public GitHub issue for security problems.**

Instead, open a [GitHub Security Advisory](https://github.com/1ay1/rcp/security/advisories/new)
on the repository. We aim to acknowledge reports within 72 hours.

## Scope

In scope:

- Ambiguities or gaps in the normative spec that enable a security bypass
  (trust-boundary confusion, prompt-injection surfaces, filter/query injection).
- Vulnerabilities in the reference SDKs (`sdk/cpp`, `sdk/python`, `sdk/node`,
  `sdk/rust`) or example servers/clients.
- Schema flaws that permit unsafe or ambiguous messages.

Out of scope:

- Vulnerabilities in third-party engines that merely *implement* RCP.
- Issues in vendored dependencies already tracked upstream.

## Threat model

The spec's [Security Considerations](https://rcp-6d6ef6d5.mintlify.site/operating/security)
section is the authoritative threat model — indirect prompt injection, corpus
poisoning, DoS/SSRF, and privacy are all treated there. Please read it before
reporting, so reports can reference the relevant guarantees.
