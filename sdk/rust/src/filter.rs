//! Build and validate RCP metadata filter trees (spec §8).
//!
//! Two jobs, one canonical implementation shared by every RCP peer:
//!
//! * **Builder** (client side): compose a filter tree with typed helpers instead
//!   of hand-writing [`Json`], so a bad operator or shape is impossible to build.
//!
//!   ```
//!   use rcp::filter;
//!   let flt = filter::eq("lang", "en").and(filter::gte("year", 2015));
//!   let _wire = flt.to_json();
//!   ```
//!
//! * **Validator** (server side): turn an arbitrary incoming filter into either a
//!   clean normalized tree or a precise [`RcpError`] `-32602` naming the offending
//!   path — so no server hand-parses a tree and re-introduces a panic/`unwrap` leak.
//!
//!   ```
//!   use rcp::{filter, Json};
//!   let fields = [("year", "int"), ("lang", "keyword")];
//!   let params_filter: Option<&Json> = None;
//!   let _tree = filter::validate(params_filter, Some(&fields), None, 32)?;
//!   # Ok::<(), rcp::RcpError>(())
//!   ```
//!
//! Wire format is unchanged: a nested tree of `{and|or|not: …}` combinators over
//! `{field, op, value}` leaves (spec §8).

use crate::json::{obj, Json};
use crate::types::{Errc, RcpError};

/// Scalar comparison operators (spec §8).
pub const SCALAR_OPS: [&str; 6] = ["eq", "ne", "gt", "gte", "lt", "lte"];
/// Set-membership operators.
pub const ARRAY_OPS: [&str; 2] = ["in", "nin"];
/// Presence operator.
pub const UNARY_OPS: [&str; 1] = ["exists"];
/// Substring / element-membership operator.
pub const STRING_OPS: [&str; 1] = ["contains"];

const ORDERED_OPS: [&str; 4] = ["gt", "gte", "lt", "lte"];
const ORDERED_TYPES: [&str; 3] = ["int", "float", "date"];

fn is_known_op(op: &str) -> bool {
    SCALAR_OPS.contains(&op)
        || ARRAY_OPS.contains(&op)
        || UNARY_OPS.contains(&op)
        || STRING_OPS.contains(&op)
}

// ── Builder ──────────────────────────────────────────────────────────────────

/// An immutable filter tree node. Combine with [`Filter::and`] / [`Filter::or`] /
/// [`Filter::not`] and emit the wire form with [`Filter::to_json`].
#[derive(Clone, Debug)]
pub struct Filter(Json);

impl Filter {
    /// The spec §8 wire object. Always a fresh clone — safe to embed.
    pub fn to_json(&self) -> Json {
        self.0.clone()
    }

    /// Boolean AND of `self` and `other`.
    pub fn and(self, other: Filter) -> Filter {
        all([self, other])
    }

    /// Boolean OR of `self` and `other`.
    pub fn or(self, other: Filter) -> Filter {
        any([self, other])
    }

    /// Boolean NOT of `self`. Also available as `!filter` via [`std::ops::Not`].
    pub fn negate(self) -> Filter {
        Filter(obj(&[("not", self.0)]))
    }
}

impl std::ops::Not for Filter {
    type Output = Filter;
    fn not(self) -> Filter {
        self.negate()
    }
}

impl std::ops::BitAnd for Filter {
    type Output = Filter;
    fn bitand(self, rhs: Filter) -> Filter {
        self.and(rhs)
    }
}

impl std::ops::BitOr for Filter {
    type Output = Filter;
    fn bitor(self, rhs: Filter) -> Filter {
        self.or(rhs)
    }
}

fn leaf(field: &str, op: &str, value: Json) -> Filter {
    Filter(obj(&[
        ("field", Json::from(field)),
        ("op", Json::from(op)),
        ("value", value),
    ]))
}

/// `field == value`.
pub fn eq(field: &str, value: impl Into<Json>) -> Filter {
    leaf(field, "eq", value.into())
}
/// `field != value`.
pub fn ne(field: &str, value: impl Into<Json>) -> Filter {
    leaf(field, "ne", value.into())
}
/// `field > value`.
pub fn gt(field: &str, value: impl Into<Json>) -> Filter {
    leaf(field, "gt", value.into())
}
/// `field >= value`.
pub fn gte(field: &str, value: impl Into<Json>) -> Filter {
    leaf(field, "gte", value.into())
}
/// `field < value`.
pub fn lt(field: &str, value: impl Into<Json>) -> Filter {
    leaf(field, "lt", value.into())
}
/// `field <= value`.
pub fn lte(field: &str, value: impl Into<Json>) -> Filter {
    leaf(field, "lte", value.into())
}

/// `field ∈ values` (set membership).
pub fn in_(field: &str, values: impl IntoIterator<Item = Json>) -> Filter {
    leaf(field, "in", Json::Array(values.into_iter().collect()))
}
/// `field ∉ values` (set exclusion).
pub fn nin(field: &str, values: impl IntoIterator<Item = Json>) -> Filter {
    leaf(field, "nin", Json::Array(values.into_iter().collect()))
}
/// Substring on a `text` field, or element membership in an array field.
pub fn contains(field: &str, value: impl Into<Json>) -> Filter {
    leaf(field, "contains", value.into())
}
/// Whether `field` is present (`present = true`) or absent (`false`).
pub fn exists(field: &str, present: bool) -> Filter {
    leaf(field, "exists", Json::Bool(present))
}

/// Boolean AND of all `clauses`.
pub fn all(clauses: impl IntoIterator<Item = Filter>) -> Filter {
    combine("and", clauses)
}
/// Boolean OR of all `clauses`.
pub fn any(clauses: impl IntoIterator<Item = Filter>) -> Filter {
    combine("or", clauses)
}
/// Boolean NOT of `clause` (free-function form of the `!` operator).
pub fn not(clause: Filter) -> Filter {
    clause.negate()
}

fn combine(kind: &str, clauses: impl IntoIterator<Item = Filter>) -> Filter {
    let arr = Json::Array(clauses.into_iter().map(|c| c.0).collect());
    Filter(obj(&[(kind, arr)]))
}

// ── Validator ────────────────────────────────────────────────────────────────

/// Validate + normalize an incoming filter (spec §8). Server-side entry point.
///
/// Returns `Ok(None)` for an absent/empty filter, else `Ok(Some(tree))` with a
/// normalized wire object. Returns `Err(RcpError)` `-32602` with `data.field`
/// naming the offending path for anything malformed, unauthorized, or
/// type-mismatched — so a server never hand-parses and never panics on bad input.
///
/// * `fields` — advertised `filter.fields` as `(name, type)` pairs. `None` allows
///   any field (no field gating). `type` may be `""` when only names are gated.
/// * `operators` — advertised operator allow-list. `None` allows all spec ops.
/// * `max_depth` — reject pathologically deep trees (DoS guard, spec §15.5).
pub fn validate(
    node: Option<&Json>,
    fields: Option<&[(&str, &str)]>,
    operators: Option<&[&str]>,
    max_depth: i32,
) -> Result<Option<Json>, RcpError> {
    let node = match node {
        None => return Ok(None),
        Some(n) if n.is_null() => return Ok(None),
        Some(Json::Object(o)) if o.is_empty() => return Ok(None),
        Some(n) => n,
    };
    Ok(Some(validate_node(node, fields, operators, "filter", max_depth)?))
}

fn validate_node(
    node: &Json,
    fields: Option<&[(&str, &str)]>,
    operators: Option<&[&str]>,
    path: &str,
    depth: i32,
) -> Result<Json, RcpError> {
    if depth < 0 {
        return Err(bad(path, "filter nesting too deep"));
    }
    let obj_pairs = match node.as_object() {
        Some(o) => o,
        None => return Err(bad(path, "filter node must be an object")),
    };

    let has = |k: &str| obj_pairs.iter().any(|(kk, _)| kk == k);
    let present: Vec<&str> = ["and", "or", "not"].into_iter().filter(|k| has(k)).collect();
    let is_leaf = has("field") || has("op");
    if present.len() > 1 {
        return Err(bad(path, "filter node has multiple combinators"));
    }
    if !present.is_empty() && is_leaf {
        return Err(bad(path, "filter node mixes a combinator with a leaf"));
    }

    if let Some(&comb) = present.first() {
        if comb == "not" {
            let child = node.get("not").unwrap();
            let inner = validate_node(child, fields, operators, &format!("{path}.not"), depth - 1)?;
            return Ok(obj(&[("not", inner)]));
        }
        let kids = match node.get(comb).and_then(|v| v.as_array()) {
            Some(a) => a,
            None => return Err(bad(&format!("{path}.{comb}"), "combinator must be an array")),
        };
        let mut out = Vec::with_capacity(kids.len());
        for (i, c) in kids.iter().enumerate() {
            out.push(validate_node(c, fields, operators, &format!("{path}.{comb}[{i}]"), depth - 1)?);
        }
        return Ok(obj(&[(comb, Json::Array(out))]));
    }

    if !is_leaf {
        return Err(bad(path, "filter node is neither a combinator nor a leaf"));
    }

    // Leaf: {field, op, value?}
    let field = match node.get_str("field") {
        Some(f) if !f.is_empty() => f,
        _ => return Err(bad(path, "leaf requires a non-empty string 'field'")),
    };
    let op = match node.get_str("op") {
        Some(o) => o,
        None => return Err(bad(&format!("{path}.op"), "leaf requires a string 'op'")),
    };
    if !is_known_op(op) {
        return Err(bad(&format!("{path}.op"), &format!("unknown operator '{op}'")));
    }
    if let Some(ops) = operators {
        if !ops.contains(&op) {
            return Err(bad(&format!("{path}.op"), &format!("operator '{op}' not advertised")));
        }
    }
    let ftype = match fields {
        Some(fs) => match fs.iter().find(|(n, _)| *n == field) {
            Some((_, t)) => Some(*t),
            None => return Err(bad(&format!("{path}.{field}"), &format!("field '{field}' not advertised"))),
        },
        None => None,
    };

    check_value(op, node, ftype, path)?;

    let value = if UNARY_OPS.contains(&op) {
        Json::Bool(node.get("value").and_then(|v| v.as_bool()).unwrap_or(true))
    } else {
        node.get("value").cloned().unwrap()
    };
    Ok(obj(&[
        ("field", Json::from(field)),
        ("op", Json::from(op)),
        ("value", value),
    ]))
}

fn check_value(op: &str, node: &Json, ftype: Option<&str>, path: &str) -> Result<(), RcpError> {
    if UNARY_OPS.contains(&op) {
        return Ok(());
    }
    let value = match node.get("value") {
        Some(v) => v,
        None => return Err(bad(&format!("{path}.value"), &format!("operator '{op}' requires a 'value'"))),
    };
    if ARRAY_OPS.contains(&op) {
        match value.as_array() {
            Some(a) if !a.is_empty() => {
                if a.iter().any(|v| matches!(v, Json::Array(_) | Json::Object(_))) {
                    return Err(bad(&format!("{path}.value"), &format!("operator '{op}' array must hold scalars")));
                }
            }
            _ => return Err(bad(&format!("{path}.value"), &format!("operator '{op}' requires a non-empty array"))),
        }
    } else if (SCALAR_OPS.contains(&op) || STRING_OPS.contains(&op))
        && matches!(value, Json::Array(_) | Json::Object(_))
    {
        return Err(bad(&format!("{path}.value"), &format!("operator '{op}' requires a scalar value")));
    }
    if ORDERED_OPS.contains(&op) {
        if let Some(t) = ftype {
            if !t.is_empty() && !ORDERED_TYPES.contains(&t) {
                return Err(bad(&format!("{path}.op"), &format!("operator '{op}' not valid on '{t}' field")));
            }
        }
    }
    Ok(())
}

fn bad(path: &str, message: &str) -> RcpError {
    RcpError::with_data(
        Errc::INVALID_PARAMS,
        format!("{message} ({path})"),
        obj(&[("field", Json::from(path))]),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    const FIELDS: [(&str, &str); 2] = [("year", "int"), ("lang", "keyword")];
    const OPS: [&str; 8] = ["eq", "ne", "gt", "gte", "lt", "lte", "in", "nin"];

    #[test]
    fn builder_emits_spec_shape() {
        let leaf = eq("lang", "fr").to_json();
        assert_eq!(leaf.get_str("field"), Some("lang"));
        assert_eq!(leaf.get_str("op"), Some("eq"));
        assert_eq!(leaf.get("value").and_then(|v| v.as_str()), Some("fr"));

        let tree = eq("lang", "en").and(gte("year", 2015i64)).to_json();
        let arr = tree.get("and").and_then(|v| v.as_array()).unwrap();
        assert_eq!(arr.len(), 2);
    }

    #[test]
    fn validate_accepts_well_formed() {
        let tree = all([eq("lang", "en"), gte("year", 2015i64)]).to_json();
        let out = validate(Some(&tree), Some(&FIELDS), Some(&OPS), 32).unwrap();
        assert!(out.is_some());
        // empty / absent -> None
        assert!(validate(None, Some(&FIELDS), None, 32).unwrap().is_none());
        assert!(validate(Some(&Json::object()), Some(&FIELDS), None, 32).unwrap().is_none());
    }

    #[test]
    fn validate_rejects_every_malformed_tree_with_32602() {
        let cases = [
            obj(&[("field", "author".into()), ("op", "eq".into()), ("value", "z".into())]),
            obj(&[("field", "lang".into()), ("op", "equals".into()), ("value", "en".into())]),
            obj(&[("and", "not-a-list".into())]),
            obj(&[("field", "year".into()), ("op", "in".into()), ("value", Json::Int(2017))]),
            obj(&[("or", Json::Array(vec![obj(&[("field", "lang".into())])]))]),
            obj(&[("field", "lang".into()), ("op", "gt".into()), ("value", "en".into())]),
        ];
        for c in &cases {
            let err = validate(Some(c), Some(&FIELDS), Some(&OPS), 32).unwrap_err();
            assert_eq!(err.code, Errc::INVALID_PARAMS, "case: {}", c.dump());
            assert!(err.data.as_ref().and_then(|d| d.get("field")).is_some());
        }
    }
}
