#!/usr/bin/env python3
"""example_federation.py — ONE COMMAND: fan a query across two engines, fuse.

    python3 examples/example_federation.py

This is spec §16 (federation) made runnable end-to-end. It spawns two genuinely
independent RCP engines as subprocesses — a "papers" engine and a "web" engine,
each with its own corpus — connects to both over stdio, fans the SAME query out
to each, and merges their ranked lists with the reference Reciprocal Rank Fusion
from `rcp.rrf_fuse` (spec §16.3). Every fused hit is tagged with its origin
engine in `meta.engine`, and per-engine weights bias the merge.

No servers to start, no ports, no config — the whole federation lives and dies
with this process.
"""
from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[0] / "sdk" / "python"))

import rcp  # noqa: E402

ENGINE = str(HERE / "_federation_engine.py")
QUERY = "how does reciprocal rank fusion combine rankings"


def main() -> int:
    # Discovery is the caller's job (spec §16.4); here we just know two engines.
    engines = {
        "papers": {"weight": 1.0},
        "web": {"weight": 0.7},  # trust the web engine a bit less
    }

    # Connect to each engine subprocess and fan the query out.
    per_engine_lists = {}
    clients = {}
    for name in engines:
        c = rcp.connect_stdio([sys.executable, ENGINE, name])
        clients[name] = c
        assert c.supports(rcp.Capability.Retrieve), f"{name} lacks retrieve"
        hits = c.retrieve(QUERY, k=4)
        per_engine_lists[name] = hits
        print(f"[{name:6}] returned {len(hits)}: {[h['id'] for h in hits]}")

    # Fuse the per-engine ranked lists with RRF (spec §16.3). Weights bias the
    # merge; the deterministic tie-break makes the output reproducible.
    weights = {name: e["weight"] for name, e in engines.items()}
    fused = rcp.rrf_fuse(per_engine_lists, k=5, weights=weights)

    print(f"\nfused top-{len(fused)} (RRF, weights={weights}):")
    for rank, hit in enumerate(fused, 1):
        meta = hit.get("meta", {})
        print(f"  {rank}. {hit['id']:<10} engine={meta.get('engine'):<7} "
              f"fusedScore={meta.get('fusedScore'):.5f}")

    # The union capability set is the effective federation capability (§16).
    union = set()
    for c in clients.values():
        union |= {cap for cap in rcp.Capability if c.supports(cap)}
    print(f"\nfederation capabilities (union): {sorted(c.value for c in union)}")

    for c in clients.values():
        c.shutdown()

    # A hit that appears in BOTH engines should outrank a same-rank singleton —
    # that is the whole point of fusion. w-rrf & p-rrf both mention the query.
    ids = [h["id"] for h in fused]
    assert "w-rrf" in ids and "p-rrf" in ids, ids
    print("\nOK — two engines fanned out and fused into one ranked list.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
