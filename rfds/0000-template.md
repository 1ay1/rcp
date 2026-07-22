# RFD NNNN — Title

- **Status:** Draft <!-- Draft | Discussion | Accepted | Rejected | Withdrawn -->
- **Author(s):** Your Name
- **Created:** YYYY-MM-DD
- **Target version:** RCP/1.x
- **Tracking issue:** #NNN

## Summary

One paragraph: what this proposes and why, in plain language.

## Motivation

What retrieval technique, workflow, or interoperability need does today's
protocol fail to express? Cite the concrete gap. Reference prior art (papers,
other protocols) where relevant.

## Proposal

The concrete change to the wire, schema, and/or capability lattice. Include:

- New or changed **methods** with full params/result JSON examples.
- New or changed **capability** keys and what advertising them means.
- Any new **error codes** and their retryability.

```json
{ "example": "wire message showing the new shape" }
```

## Compatibility

- **Additivity.** Explain how existing peers are unaffected. Which capability
  gates the new behaviour? A client that never sees the capability must behave
  exactly as before.
- **Version.** Which minor version introduces this. No shape that already exists
  may change meaning.
- **SDKs.** What each of the four reference SDKs must add.

## Security considerations

Trust boundaries, prompt-injection surface, poisoning, DoS/SSRF, privacy. Map
onto the spec's [Security Considerations](../spec/rcp-1.0.md#15-security-considerations).

## Alternatives considered

What else was on the table and why this shape won.

## Unresolved questions

Open points to settle before acceptance.
