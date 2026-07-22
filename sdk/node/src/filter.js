// rcp/filter — build and validate RCP metadata filter trees (spec §8).
//
// Two jobs, one canonical implementation shared by every RCP peer:
//
//  * Builder (client side): compose a filter tree with typed helpers instead of
//    hand-writing objects, so a bad operator or shape is impossible to construct.
//
//      import * as rcp from "rcp-protocol";
//      const flt = rcp.filter.all(rcp.filter.eq("lang", "en"), rcp.filter.gte("year", 2015));
//      await client.retrieve("q", 10, { filter: flt.toJSON() });
//
//  * Validator (server side): turn an arbitrary incoming filter into either a
//    clean normalized tree or a precise RcpError -32602 naming the offending
//    path — so no server hand-parses a tree and re-introduces a raw-throw leak.
//
//      const tree = rcp.filter.validate(params.filter, { fields: { year: "int", lang: "keyword" } });
//
// Wire format is unchanged: a nested tree of {and|or|not: ...} combinators over
// {field, op, value} leaves (spec §8).
import { Errc, RcpError } from "./types.js";

export const SCALAR_OPS = Object.freeze(["eq", "ne", "gt", "gte", "lt", "lte"]);
export const ARRAY_OPS = Object.freeze(["in", "nin"]);
export const UNARY_OPS = Object.freeze(["exists"]);
export const STRING_OPS = Object.freeze(["contains"]);
export const ALL_OPS = Object.freeze([...SCALAR_OPS, ...ARRAY_OPS, ...UNARY_OPS, ...STRING_OPS]);

const OPSET = new Set(ALL_OPS);
const ORDERED_OPS = new Set(["gt", "gte", "lt", "lte"]);
const ORDERED_TYPES = new Set(["int", "float", "date"]);

function deepCopy(v) {
  if (Array.isArray(v)) return v.map(deepCopy);
  if (v && typeof v === "object") {
    const o = {};
    for (const k of Object.keys(v)) o[k] = deepCopy(v[k]);
    return o;
  }
  return v;
}

// ── Builder ──────────────────────────────────────────────────────────────────
export class Filter {
  constructor(node) {
    this._node = node;
  }
  toJSON() {
    return deepCopy(this._node);
  }
  and(other) {
    return all(this, other);
  }
  or(other) {
    return any(this, other);
  }
  not() {
    return not_(this);
  }
}

function leaf(field, op, value) {
  if (typeof field !== "string" || field.length === 0)
    throw new TypeError("filter field must be a non-empty string");
  if (!OPSET.has(op))
    throw new TypeError(`unknown filter operator '${op}'; expected one of ${ALL_OPS.join(", ")}`);
  const node = { field, op };
  if (UNARY_OPS.includes(op)) node.value = value === undefined ? true : Boolean(value);
  else node.value = value;
  return new Filter(node);
}

export const eq = (f, v) => leaf(f, "eq", v);
export const ne = (f, v) => leaf(f, "ne", v);
export const gt = (f, v) => leaf(f, "gt", v);
export const gte = (f, v) => leaf(f, "gte", v);
export const lt = (f, v) => leaf(f, "lt", v);
export const lte = (f, v) => leaf(f, "lte", v);
export const in_ = (f, vs) => leaf(f, "in", Array.from(vs));
export const nin = (f, vs) => leaf(f, "nin", Array.from(vs));
export const contains = (f, v) => leaf(f, "contains", v);
export const exists = (f, present = true) => leaf(f, "exists", present);

export function all(...clauses) {
  return combine("and", clauses);
}
export function any(...clauses) {
  return combine("or", clauses);
}
export function not_(clause) {
  if (!(clause instanceof Filter)) throw new TypeError("not_ expects a Filter");
  return new Filter({ not: clause.toJSON() });
}

function combine(kind, clauses) {
  const nodes = [];
  for (const c of clauses) {
    if (!(c instanceof Filter)) throw new TypeError(`${kind} expects Filter clauses`);
    nodes.push(c.toJSON());
  }
  return new Filter({ [kind]: nodes });
}

// ── Validator ────────────────────────────────────────────────────────────────
// Returns null for an absent/empty filter, else a normalized wire object.
// Throws RcpError(-32602) with data.field naming the offending path otherwise.
export function validate(node, { fields = null, operators = null, maxDepth = 32 } = {}) {
  if (node === null || node === undefined) return null;
  if (typeof node === "object" && !Array.isArray(node) && Object.keys(node).length === 0) return null;
  const allowedOps = operators ? new Set(operators) : OPSET;
  const fieldTypes = fieldTypeMap(fields);
  return validateNode(node, allowedOps, fieldTypes, "filter", maxDepth);
}

function validateNode(node, allowedOps, fieldTypes, path, depth) {
  if (depth < 0) bad(path, "filter nesting too deep");
  if (typeof node !== "object" || node === null || Array.isArray(node))
    bad(path, "filter node must be an object");

  const comb = combinatorKey(node, path);
  if (comb === "and" || comb === "or") {
    const kids = node[comb];
    if (!Array.isArray(kids)) bad(`${path}.${comb}`, `'${comb}' must be an array`);
    return { [comb]: kids.map((c, i) => validateNode(c, allowedOps, fieldTypes, `${path}.${comb}[${i}]`, depth - 1)) };
  }
  if (comb === "not") {
    return { not: validateNode(node.not, allowedOps, fieldTypes, `${path}.not`, depth - 1) };
  }

  const field = node.field;
  const op = node.op;
  if (typeof field !== "string" || field.length === 0) bad(path, "leaf requires a non-empty string 'field'");
  if (!OPSET.has(op)) bad(`${path}.op`, `unknown operator '${op}'`);
  if (!allowedOps.has(op)) bad(`${path}.op`, `operator '${op}' not advertised`);
  if (fieldTypes && !(field in fieldTypes)) bad(`${path}.${field}`, `field '${field}' not advertised`);

  const ftype = fieldTypes ? fieldTypes[field] : null;
  checkValue(op, node, ftype, path);
  const out = { field, op };
  out.value = UNARY_OPS.includes(op) ? node.value === undefined ? true : Boolean(node.value) : node.value;
  return out;
}

function checkValue(op, node, ftype, path) {
  if (UNARY_OPS.includes(op)) return;
  if (!("value" in node)) bad(`${path}.value`, `operator '${op}' requires a 'value'`);
  const value = node.value;
  if (ARRAY_OPS.includes(op)) {
    if (!Array.isArray(value) || value.length === 0) bad(`${path}.value`, `operator '${op}' requires a non-empty array`);
    if (value.some((v) => v !== null && typeof v === "object")) bad(`${path}.value`, `operator '${op}' array must hold scalars`);
  } else if (SCALAR_OPS.includes(op) || STRING_OPS.includes(op)) {
    if (value !== null && typeof value === "object") bad(`${path}.value`, `operator '${op}' requires a scalar value`);
  }
  if (ORDERED_OPS.has(op) && ftype && !ORDERED_TYPES.has(ftype))
    bad(`${path}.op`, `operator '${op}' not valid on '${ftype}' field`);
}

function combinatorKey(node, path) {
  const present = ["and", "or", "not"].filter((k) => k in node);
  const isLeaf = "field" in node || "op" in node;
  if (present.length > 1) bad(path, "filter node has multiple combinators");
  if (present.length && isLeaf) bad(path, "filter node mixes a combinator with a leaf");
  if (present.length) return present[0];
  if (!isLeaf) bad(path, "filter node is neither a combinator nor a leaf");
  return null;
}

function fieldTypeMap(fields) {
  if (fields === null || fields === undefined) return null;
  if (Array.isArray(fields)) {
    const m = {};
    for (const n of fields) m[n] = null;
    return m;
  }
  return { ...fields };
}

function bad(path, message) {
  throw new RcpError(Errc.INVALID_PARAMS, `${message} (${path})`, { field: path });
}
