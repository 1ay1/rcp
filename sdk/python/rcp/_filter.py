"""rcp.filter — build and validate RCP metadata filter trees (spec §8).

Two jobs, one canonical implementation shared by every RCP peer:

* **Builder** (client side): compose a filter tree with typed helpers instead of
  hand-writing dicts, so a bad operator or shape is impossible to construct.

      from rcp import filter as f
      flt = f.all_(f.eq("lang", "en"), f.gte("year", 2015))
      client.retrieve("q", opts={"filter": flt.to_json()})

* **Validator** (server side): turn an *arbitrary* incoming filter into either a
  clean, normalized tree or a precise :class:`RcpError` ``-32602`` naming the
  offending path — so no server ever hand-parses a tree and re-introduces the
  ``KeyError``/``TypeError`` → generic ``-32603`` leak.

      from rcp import filter as f
      tree = f.validate(params.get("filter"), fields={"year": "int", "lang": "keyword"})
      # tree is None or a normalized dict; malformed input already raised -32602.

The wire format is unchanged: a nested tree of ``{and|or|not: ...}`` combinators
over ``{field, op, value}`` leaves (spec §8). This module only makes producing
and consuming it total and safe.
"""
from __future__ import annotations

from ._types import Errc, RcpError

# The complete operator set from spec §8, with the JSON shape each `value` takes.
SCALAR_OPS = frozenset({"eq", "ne", "gt", "gte", "lt", "lte"})
ARRAY_OPS = frozenset({"in", "nin"})
UNARY_OPS = frozenset({"exists"})          # value is a bool (field present-or-not)
STRING_OPS = frozenset({"contains"})       # substring / element membership
ALL_OPS = SCALAR_OPS | ARRAY_OPS | UNARY_OPS | STRING_OPS

# Which operators the ordered comparisons require an ordered field type for.
_ORDERED_OPS = frozenset({"gt", "gte", "lt", "lte"})
_ORDERED_TYPES = frozenset({"int", "float", "date"})


# ── Builder ──────────────────────────────────────────────────────────────────
class Filter:
    """An immutable filter tree node. Combine with ``&`` / ``|`` / ``~`` or the
    :func:`all_` / :func:`any_` / :func:`not_` helpers; emit with :meth:`to_json`."""

    __slots__ = ("_node",)

    def __init__(self, node: dict):
        self._node = node

    def to_json(self) -> dict:
        """The spec §8 wire dict. Always a fresh copy — safe to mutate."""
        return _copy(self._node)

    def __and__(self, other: "Filter") -> "Filter":
        return all_(self, other)

    def __or__(self, other: "Filter") -> "Filter":
        return any_(self, other)

    def __invert__(self) -> "Filter":
        return not_(self)

    def __repr__(self) -> str:
        return f"Filter({self._node!r})"


def _leaf(field: str, op: str, value=None) -> Filter:
    if not isinstance(field, str) or not field:
        raise ValueError("filter field must be a non-empty string")
    if op not in ALL_OPS:
        raise ValueError(f"unknown filter operator {op!r}; expected one of {sorted(ALL_OPS)}")
    node = {"field": field, "op": op}
    if op not in UNARY_OPS:
        node["value"] = value
    else:
        node["value"] = bool(value) if value is not None else True
    return Filter(node)


# Scalar comparisons.
def eq(field: str, value) -> Filter: return _leaf(field, "eq", value)
def ne(field: str, value) -> Filter: return _leaf(field, "ne", value)
def gt(field: str, value) -> Filter: return _leaf(field, "gt", value)
def gte(field: str, value) -> Filter: return _leaf(field, "gte", value)
def lt(field: str, value) -> Filter: return _leaf(field, "lt", value)
def lte(field: str, value) -> Filter: return _leaf(field, "lte", value)


def in_(field: str, values) -> Filter:
    """Set membership. ``values`` must be a non-empty iterable of scalars."""
    return _leaf(field, "in", list(values))


def nin(field: str, values) -> Filter:
    """Set exclusion. ``values`` must be a non-empty iterable of scalars."""
    return _leaf(field, "nin", list(values))


def contains(field: str, value) -> Filter:
    """Substring match on a `text` field, or element membership in an array field."""
    return _leaf(field, "contains", value)


def exists(field: str, present: bool = True) -> Filter:
    """Whether ``field`` is present (``present=True``) or absent (``False``)."""
    return _leaf(field, "exists", present)


def all_(*clauses: Filter) -> Filter:
    """Boolean AND. ``all_()`` with no clauses is the always-true empty filter."""
    return _combine("and", clauses)


def any_(*clauses: Filter) -> Filter:
    """Boolean OR."""
    return _combine("or", clauses)


def not_(clause: Filter) -> Filter:
    """Boolean NOT."""
    if not isinstance(clause, Filter):
        raise TypeError("not_ expects a Filter")
    return Filter({"not": clause.to_json()})


def _combine(kind: str, clauses) -> Filter:
    nodes = []
    for c in clauses:
        if not isinstance(c, Filter):
            raise TypeError(f"{kind} expects Filter clauses, got {type(c).__name__}")
        nodes.append(c.to_json())
    return Filter({kind: nodes})


# ── Validator ────────────────────────────────────────────────────────────────
def validate(node, fields=None, operators=None, *, max_depth: int = 32):
    """Validate + normalize an incoming filter (spec §8). Server-side entry point.

    Returns ``None`` for an absent/empty filter, else a normalized wire dict.
    Raises :class:`RcpError` ``-32602`` with ``data.field`` naming the offending
    path for anything malformed, unauthorized, or type-mismatched — so a server
    never has to hand-parse and never leaks a raw ``KeyError`` as ``-32603``.

    * ``fields`` — the advertised ``filter.fields`` (a ``{name: type}`` dict or a
      list/set of allowed names). ``None`` allows any field (no field gating).
    * ``operators`` — the advertised operator allow-list. ``None`` allows all
      spec operators.
    * ``max_depth`` — reject pathologically deep trees (DoS guard, spec §15.5).
    """
    if node is None or node == {}:
        return None
    allowed_ops = frozenset(operators) if operators is not None else ALL_OPS
    field_types = _field_type_map(fields)
    return _validate(node, allowed_ops, field_types, "filter", max_depth)


def _validate(node, allowed_ops, field_types, path, depth):
    if depth < 0:
        _bad(path, "filter nesting too deep")
    if not isinstance(node, dict):
        _bad(path, "filter node must be an object")

    combinator = _combinator_key(node, path)
    if combinator in ("and", "or"):
        children = node[combinator]
        if not isinstance(children, list):
            _bad(f"{path}.{combinator}", f"'{combinator}' must be an array")
        return {combinator: [
            _validate(c, allowed_ops, field_types, f"{path}.{combinator}[{i}]", depth - 1)
            for i, c in enumerate(children)
        ]}
    if combinator == "not":
        return {"not": _validate(node["not"], allowed_ops, field_types, f"{path}.not", depth - 1)}

    # Leaf: {field, op, value?}
    field = node.get("field")
    op = node.get("op")
    if not isinstance(field, str) or not field:
        _bad(path, "leaf requires a non-empty string 'field'")
    if op not in ALL_OPS:
        _bad(f"{path}.op", f"unknown operator {op!r}")
    if op not in allowed_ops:
        _bad(f"{path}.op", f"operator {op!r} not advertised")
    if field_types is not None and field not in field_types:
        _bad(f"{path}.{field}", f"field {field!r} not advertised")

    ftype = field_types.get(field) if field_types else None
    _check_value(op, node, ftype, path)
    out = {"field": field, "op": op}
    if op in UNARY_OPS:
        out["value"] = bool(node.get("value", True))
    else:
        out["value"] = node["value"]
    return out


def _check_value(op, node, ftype, path):
    has_value = "value" in node
    if op in UNARY_OPS:
        return  # exists: value optional (defaults true)
    if not has_value:
        _bad(f"{path}.value", f"operator {op!r} requires a 'value'")
    value = node["value"]
    if op in ARRAY_OPS:
        if not isinstance(value, (list, tuple)) or len(value) == 0:
            _bad(f"{path}.value", f"operator {op!r} requires a non-empty array")
        if any(isinstance(v, (list, dict)) for v in value):
            _bad(f"{path}.value", f"operator {op!r} array must hold scalars")
    elif op in SCALAR_OPS or op in STRING_OPS:
        if isinstance(value, (list, dict)):
            _bad(f"{path}.value", f"operator {op!r} requires a scalar value")
    # Type gating: ordered comparisons need an ordered field type when declared.
    if op in _ORDERED_OPS and ftype is not None and ftype not in _ORDERED_TYPES:
        _bad(f"{path}.op", f"operator {op!r} not valid on {ftype!r} field")


def _combinator_key(node, path):
    present = [k for k in ("and", "or", "not") if k in node]
    is_leaf = "field" in node or "op" in node
    if len(present) > 1:
        _bad(path, "filter node has multiple combinators")
    if present and is_leaf:
        _bad(path, "filter node mixes a combinator with a leaf")
    if present:
        return present[0]
    if not is_leaf:
        _bad(path, "filter node is neither a combinator nor a leaf")
    return None


def _field_type_map(fields):
    if fields is None:
        return None
    if isinstance(fields, dict):
        return dict(fields)
    return {name: None for name in fields}  # list/set of allowed names, no types


def _bad(path, message):
    raise RcpError(Errc.INVALID_PARAMS, f"{message} ({path})", data={"field": path})


def _copy(node):
    if isinstance(node, dict):
        return {k: _copy(v) for k, v in node.items()}
    if isinstance(node, list):
        return [_copy(v) for v in node]
    return node
