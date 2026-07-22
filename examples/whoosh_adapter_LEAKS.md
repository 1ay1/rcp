# Leak test — RCP over a real third-party engine (Whoosh)

**What this is.** `whoosh_adapter.py` is an RCP/1 server whose entire retrieval
path is delegated to [Whoosh](https://whoosh.readthedocs.io) — a full-text
search library with a real inverted index, a real BM25F scorer, a real query
parser, snippet highlighting, and term extraction. None of Whoosh's API was
designed by the RCP authors. `whoosh_adapter_client.py` drives it over stdio
with an ordinary RCP client that has no idea Whoosh is behind the wire.

**Why it exists.** Every RCP server written so far was authored by the same
people who wrote the spec, so the abstraction had never been tested against a
backend nobody controls. That is selection bias: the protocol *looks* right
because it was co-designed with its only implementations. This experiment removes
that bias. The rule was strict: **map Whoosh onto RCP naturally and change the
spec zero times; record every mismatch instead of smoothing it over.**

**Result:** the spec did **not** need to change. Whoosh maps onto RCP cleanly —
`retrieve` + `filter` + `citations` + `provenance` all landed on real Whoosh
features without contortion. That is a genuine (not rigged) point in RCP's
favour. But the exercise surfaced **three real leaks**, all in the *SDK*, not the
protocol. They are worth fixing.

Run it yourself:

```
python3 -m venv examples/.poc-venv
examples/.poc-venv/bin/pip install Whoosh jsonschema
examples/.poc-venv/bin/python examples/whoosh_adapter_client.py
```

---

## What mapped cleanly (the honest wins)

| RCP surface | Whoosh feature it landed on | Contortion? |
|---|---|---|
| `retrieve` + `k` | `searcher.search(q, limit=k)` | none |
| `retrieve.modes = ["sparse"]` | BM25F is lexical/sparse — advertised truthfully | none |
| `Hit.score` | `results[i].score` (raw BM25F) | none |
| `Hit.scores.bm25f` | same number, tagged by stage | none |
| `Hit.citation.quote` | `results[i].highlights("body")` (real term-position snippet) | none |
| `Hit.provenance` | Whoosh stored fields (`year`, `lang`, `uri`) | none |
| `filter` boolean tree (§8) | compiled to `And`/`Or`/`Not`/`Term`/`NumericRange` | none |
| unadvertised filter field → `-32602` | `FilterError` → `RcpError(INVALID_PARAMS)` | none |

The single most reassuring finding: RCP's **filter boolean tree** (§8) compiled
onto Whoosh's query algebra one-to-one — `{and:[{field,op,value}]}` → `And([...])`
— with no operator RCP defines that Whoosh couldn't express, and Whoosh's
`filter=` search argument accepting the compiled mask directly. A protocol filter
DSL that maps cleanly onto an engine its authors never saw is the strongest
evidence here that the abstraction is at the right level.

---

## Leak #1 — `score` scale is a semantic gap the protocol punts on (FIXED)

RCP's `Hit.score` is "higher is better" but **the scale is server-defined**. BM25F
scores are *unbounded and corpus-relative*: `6.77` here means nothing to a client
comparing against a different server's cosine similarity in `[0,1]`. RCP already
anticipates this — it has `confidence ∈ [0,1]` for exactly the portable case — so
the adapter squashes raw BM25F into `confidence` and reports raw BM25F in both
`score` and `scores.bm25f`, advertising `scoreScale: "bm25f-unbounded"`.

**Was this a leak?** Partially. The protocol was *correct* (it never promised
score comparability), but `scoreScale` was a field the adapter invented, not a
spec-defined one — so a client federating this server with a cosine server had no
**standard** key telling it the scales differ.

**Fixed:** spec §6.1 now defines an optional `retrieve.scoreScale` enum
(`"cosine"|"dot"|"bm25"|"probability"|"unbounded"`) *and* states normatively that
cross-server ranking/fusion **MUST** use rank position (RRF) or `confidence`,
never raw `score`. One-paragraph spec addition, no redesign.

## Leak #2 — the SDK `Capability` enum was missing spec capabilities (FIXED)

The spec's capability table (§7.2, and §8 "when `filter` is advertised") makes
`filter` a **first-class capability**. Yet *none* of the four SDKs had a `Filter`
member in their capability enum — nor `Streaming`, `Pagination`, `Citations`, or
`Log`, all of which the spec treats as advertised capabilities. The adapter first
had to advertise `filter` through the raw-string escape hatch
(`advertise("filter", {...})`), which worked, but meant **a typed client could
not gate on `filter` via the enum** — `client.supports(Capability.Filter)` didn't
even exist. A real SDK/spec sync bug the maintainer-authored servers never hit
because none of them advertised `filter`. (The C++ SDK was subtler: it had
`filter`/`streaming`/etc. as *struct fields* but not enum members, so
`supports(Capability::Filter)` was equally impossible.)

**Fixed:** added `Filter`, `Streaming`, `Pagination`, `Citations`, `Log` to the
capability enum in **all four SDKs** (Python `_types.py`, C++ `protocol.hpp` +
`slot()`/`has()`/`with_filter()`/(de)serialization, Rust `types.rs` incl. bumping
`Capability::ALL` from `[_;12]` to `[_;17]` — the same array-length footgun as the
parity work, Node `types.js`). All four SDK test suites stay green; the Whoosh
adapter now advertises `filter` via `rcp.Capability.Filter` and the client gates
on it with `c.supports(rcp.Capability.Filter)`.

## Leak #3 — no typed filter builder; clients hand-assemble the tree (OPEN, ergonomic)

The client sends the filter as a **raw nested dict** literal
(`{"and":[{"field":"lang","op":"eq","value":"fr"}]}`). Nothing in the SDK helps
build or validate it before it hits the wire; a typo like `"op":"equals"` sails
through the client and only fails server-side with `-32602`. Contrast the hit
*normaliser*, which is typed and total. **Recommendation (deferred):** ship a
small filter builder in each SDK (`rcp.filter.eq("lang","fr") &
rcp.filter.gte("year",2015)`) that emits the spec tree and rejects unknown
operators locally. Pure ergonomics — the wire format is fine — left as a
follow-up so this change set stays reviewable; it is additive and non-breaking.

---

## What did NOT leak (equally important to state)

- **The single-`retrieve`-owns-the-pipeline bet held.** The client never had to
  know Whoosh runs one BM25 stage vs. the SOTA server's recall→rerank→MMR chain.
  It asked for hits and got hits. The pipeline-as-protocol design (spec §3) did
  not force a lexical engine to pretend it has stages it doesn't.
- **Capability negotiation did its job.** The adapter advertised *only* `["sparse"]`
  modes and no `rerank`/`graph`/`embed`; a conformant client gates correctly and
  never calls a method Whoosh can't serve.
- **The `feedback`/`memory`/`session` surface added during parity work stayed out
  of the way.** A minimal server ignores it entirely, confirming those additions
  are truly optional and don't tax a simple backend — the scope-creep worry from
  the design review did not materialise *for the minimal case*. (It remains a
  thing to watch as more stateful methods are proposed.)

---

## Verdict

RCP passed the test it was most at risk of failing: an engine its authors did not
design mapped onto it without a single protocol change, and the parts most likely
to leak (the filter DSL, the result structure) mapped *cleanest*. The three leaks
found are all fixable SDK/ergonomics gaps, not architectural faults — which is the
best possible outcome for "are we on the right track?": **yes on the protocol,
with a short punch-list on the SDKs.**

The punch-list, updated after acting on it:

1. ~~Add missing `Capability` enum members (`filter`, `streaming`, `pagination`,
   `log`, `citations`) to all four SDKs.~~ **DONE** — all four SDKs, tests green.
2. ~~Add a standard `retrieve.scoreScale` capability field or a normative
   "compare via `confidence`, not `score`" rule.~~ **DONE** — spec §6.1 now has
   both.
3. Ship a typed filter builder in each SDK. *(ergonomics — deferred, additive)*
