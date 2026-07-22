"""RCP Selector — choose ONE backend from several reachable engines.

Mirrors ``rcp::Selector`` (``sdk/cpp/include/rcp/selector.hpp``): you list
candidate engines (by hand or from an ``rcp.json`` registry) and pick one — by
id, by required capability, or by priority with liveness fallback. Connection is
**lazy**: an engine is dialled only when actually selected. The result is a
fully-connected :class:`~rcp._client.Client`.
"""
from __future__ import annotations

import json

from ._client import Client
from ._types import Errc, RcpError, cap_key


class EngineSpec:
    """How to reach one backend. Inert until selected; exactly one of ``argv``
    (stdio subprocess) or ``url`` (HTTP) is set."""

    def __init__(self, id, argv=None, url="", priority=0, weight=1.0, required=False):
        self.id = id
        self.argv = list(argv) if argv else []
        self.url = url
        self.priority = priority
        self.weight = weight
        self.required = required

    def open(self) -> Client:
        c = Client()
        if self.argv:
            return c.connect_stdio(self.argv)
        if self.url:
            return c.connect_http(self.url)
        raise RcpError(Errc.INVALID_PARAMS, f"engine '{self.id}' has no transport")

    @staticmethod
    def from_json(entry) -> "EngineSpec":
        """Parse one registry entry (spec §16.1). ``transport`` is "stdio" (needs
        ``command``) or "http" (needs ``url``); inferred if omitted."""
        if not isinstance(entry, dict) or "id" not in entry:
            raise RcpError(Errc.INVALID_PARAMS, "registry entry needs an 'id'")
        transport = entry.get("transport", "")
        command = entry.get("command") or entry.get("argv")
        url = entry.get("url", "")
        spec = EngineSpec(
            id=entry.get("id", ""),
            priority=entry.get("priority", 0),
            weight=entry.get("weight", 1.0),
            required=entry.get("required", False),
        )
        if transport == "stdio" or (not transport and command):
            if not isinstance(command, list) or not command:
                raise RcpError(Errc.INVALID_PARAMS,
                               f"stdio engine '{spec.id}' needs a 'command' array")
            spec.argv = [str(x) for x in command]
        elif transport == "http" or (not transport and url):
            if not url:
                raise RcpError(Errc.INVALID_PARAMS, f"http engine '{spec.id}' needs a 'url'")
            spec.url = url
        else:
            raise RcpError(Errc.INVALID_PARAMS,
                           f"engine '{spec.id}' has no usable transport")
        return spec


class Selector:
    """Registry of candidate engines; selection connects and returns a Client."""

    def __init__(self):
        self._specs: list[EngineSpec] = []

    # ── construction ────────────────────────────────────────────────────────
    @staticmethod
    def load(path: str) -> "Selector":
        """Build a Selector from a registry JSON file."""
        with open(path, "r", encoding="utf-8") as f:
            return Selector.loads(f.read())

    @staticmethod
    def loads(text: str) -> "Selector":
        """Build a Selector from a registry JSON string
        (``{"engines": [ ... ]}``)."""
        try:
            doc = json.loads(text)
        except ValueError as e:
            raise RcpError(Errc.PARSE_ERROR, f"invalid registry JSON: {e}")
        engines = doc.get("engines", []) if isinstance(doc, dict) else []
        sel = Selector()
        for entry in engines:
            sel._specs.append(EngineSpec.from_json(entry))
        return sel

    def add_stdio(self, id, argv, priority=0) -> "Selector":
        self._specs.append(EngineSpec(id, argv=list(argv), priority=priority))
        return self

    def add_http(self, id, url, priority=0) -> "Selector":
        self._specs.append(EngineSpec(id, url=url, priority=priority))
        return self

    @property
    def size(self) -> int:
        return len(self._specs)

    # ── selection ───────────────────────────────────────────────────────────
    def select(self, id) -> Client:
        """Connect the engine with the given id (or raise if unknown)."""
        for spec in self._specs:
            if spec.id == id:
                return spec.open()
        raise RcpError(Errc.INVALID_PARAMS, f"no engine with id '{id}'")

    def select_primary(self) -> Client:
        """Connect the highest-priority engine that answers (liveness fallback)."""
        last = None
        for spec in self._by_priority():
            try:
                return spec.open()
            except RcpError as e:
                last = e
        raise last or RcpError(Errc.BACKEND_UNAVAILABLE, "no engines configured")

    def select_capable(self, capability) -> Client:
        """Connect the highest-priority engine that answers AND advertises
        ``capability``."""
        last = None
        for spec in self._by_priority():
            try:
                c = spec.open()
            except RcpError as e:
                last = e
                continue
            if c.supports(capability):
                return c
            c.shutdown()
            last = RcpError(Errc.CAPABILITY_MISSING,
                            f"engine '{spec.id}' lacks '{cap_key(capability)}'")
        raise last or RcpError(Errc.BACKEND_UNAVAILABLE, "no capable engine")

    def _by_priority(self) -> list[EngineSpec]:
        # Stable sort by descending priority (Python's sort is stable).
        return sorted(self._specs, key=lambda s: s.priority, reverse=True)
