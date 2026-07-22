---
title: Errors & retryability
description: The RCP error code block, structured error.data, and the recommended client retry policy.
sidebar:
  order: 3
---

Every failure is a JSON-RPC error object: `{ "code": int, "message": string,
"data"?: any }`. Codes in the `-32000..-32099` block are RCP-specific; the rest
are standard JSON-RPC.

## Error codes

| Code | Symbol | Meaning |
|------|--------|---------|
| `-32700` | `ParseError` | Invalid JSON. |
| `-32600` | `InvalidRequest` | Not a valid Request object. |
| `-32601` | `MethodNotFound` | (Reserved; RCP prefers `-32004`.) |
| `-32602` | `InvalidParams` | Missing/malformed params or filter field. |
| `-32603` | `InternalError` | Server-side failure. |
| `-32001` | `NotInitialized` | Called before `initialize`. |
| `-32002` | `VersionMismatch` | No common protocol version. |
| `-32003` | `CapabilityMissing` | Method exists but not advertised. |
| `-32004` | `UnknownMethod` | Method not defined by RCP. |
| `-32005` | `OptionUnsupported` | A requested option (mode/method) is not advertised. |
| `-32006` | `Cancelled` | Request was cancelled via `notifications/cancel`. |
| `-32010` | `BackendUnavailable` | Model offline, index not built, upstream down. |
| `-32011` | `RateLimited` | Server is shedding load; client should back off. |

The range `-32000..-32099` is reserved for RCP; implementers **MUST NOT** define
custom codes there.

## Structured `error.data`

Servers **MAY** attach a structured `error.data` object with these conventional
fields (all optional): `field`, `option`, `retryable`, `retryAfterMs`, `detail`.

## Retryability

Clients **SHOULD** apply this policy, using exponential backoff with jitter and
honouring `retryAfterMs` / `Retry-After` when given.

| Code | Retryable? | Client action |
|------|-----------|---------------|
| `-32011` `RateLimited` | yes | Back off and retry the same request. |
| `-32010` `BackendUnavailable` | yes (transient) | Retry with backoff; then fail over (Selector) or drop the engine (Federation). |
| `-32603` `InternalError` | maybe | Retry once; if it recurs, treat as fatal. |
| `-32001` `NotInitialized` | yes, after fixing | Send `initialize`, then retry. |
| `-32002` `VersionMismatch` | no | Abort; no common version. |
| `-32003` / `-32004` / `-32005` | no | Programming/capability error; do not retry. |
| `-32602` `InvalidParams` | no | Fix the request. |
| `-32006` `Cancelled` | n/a | Expected after a cancel; do not retry automatically. |

See the [specification §12](/reference/spec/#12-errors) for the normative text.
