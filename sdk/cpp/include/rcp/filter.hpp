#pragma once
// rcp/filter.hpp — build and validate RCP metadata filter trees (spec §8).
//
// Two jobs, one canonical implementation shared by every RCP peer:
//
//  * Builder (client side): compose a filter tree with typed helpers instead of
//    hand-writing JSON, so a bad operator or shape is impossible to construct.
//
//      using namespace rcp::filter;
//      Json flt = all({eq("lang", "en"), gte("year", 2015)}).to_json();
//      // client.retrieve("q", 10, Json{{"filter", flt}});
//
//  * Validator (server side): turn an arbitrary incoming filter into either a
//    clean normalized tree or a precise Error -32602 naming the offending path —
//    so no server hand-parses a tree and re-introduces a throw/UB leak.
//
//      auto tree = rcp::filter::validate(params.value("filter", Json(nullptr)),
//                                        {{"year","int"},{"lang","keyword"}});
//      if (!tree) return std::unexpected(tree.error());   // -32602
//
// Wire format is unchanged: a nested tree of {and|or|not: …} combinators over
// {field, op, value} leaves (spec §8).

#include "types.hpp"

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rcp::filter {

// The complete operator set from spec §8.
inline constexpr std::string_view SCALAR_OPS[] = {"eq", "ne", "gt", "gte", "lt", "lte"};
inline constexpr std::string_view ARRAY_OPS[]  = {"in", "nin"};
inline constexpr std::string_view UNARY_OPS[]  = {"exists"};
inline constexpr std::string_view STRING_OPS[] = {"contains"};

namespace detail {
inline bool in(std::string_view v, std::initializer_list<std::string_view> set) {
    for (auto s : set) if (s == v) return true;
    return false;
}
inline bool is_scalar_op(std::string_view op) { return detail::in(op, {"eq","ne","gt","gte","lt","lte"}); }
inline bool is_array_op(std::string_view op)  { return op == "in" || op == "nin"; }
inline bool is_unary_op(std::string_view op)  { return op == "exists"; }
inline bool is_string_op(std::string_view op) { return op == "contains"; }
inline bool is_known_op(std::string_view op) {
    return is_scalar_op(op) || is_array_op(op) || is_unary_op(op) || is_string_op(op);
}
inline bool is_ordered_op(std::string_view op) { return detail::in(op, {"gt","gte","lt","lte"}); }
inline bool is_ordered_type(std::string_view t) { return t == "int" || t == "float" || t == "date"; }
} // namespace detail

// ── Builder ──────────────────────────────────────────────────────────────────
// An immutable filter tree node. Combine with operator&& / operator|| / operator!
// (or the all/any/not_ free functions); emit with to_json().
class Filter {
public:
    explicit Filter(Json node) : node_(std::move(node)) {}

    [[nodiscard]] const Json& to_json() const noexcept { return node_; }

    [[nodiscard]] Filter operator&&(const Filter& rhs) const {
        return Filter(Json{{"and", Json::array({node_, rhs.node_})}});
    }
    [[nodiscard]] Filter operator||(const Filter& rhs) const {
        return Filter(Json{{"or", Json::array({node_, rhs.node_})}});
    }
    [[nodiscard]] Filter operator!() const {
        return Filter(Json{{"not", node_}});
    }

private:
    Json node_;
};

namespace detail {
inline Filter leaf(std::string_view field, std::string_view op, Json value) {
    return Filter(Json{{"field", field}, {"op", op}, {"value", std::move(value)}});
}
} // namespace detail

inline Filter eq(std::string_view f, Json v)  { return detail::leaf(f, "eq",  std::move(v)); }
inline Filter ne(std::string_view f, Json v)  { return detail::leaf(f, "ne",  std::move(v)); }
inline Filter gt(std::string_view f, Json v)  { return detail::leaf(f, "gt",  std::move(v)); }
inline Filter gte(std::string_view f, Json v) { return detail::leaf(f, "gte", std::move(v)); }
inline Filter lt(std::string_view f, Json v)  { return detail::leaf(f, "lt",  std::move(v)); }
inline Filter lte(std::string_view f, Json v) { return detail::leaf(f, "lte", std::move(v)); }
inline Filter in_(std::string_view f, Json values)  { return detail::leaf(f, "in",  std::move(values)); }
inline Filter nin(std::string_view f, Json values)  { return detail::leaf(f, "nin", std::move(values)); }
inline Filter contains(std::string_view f, Json v)  { return detail::leaf(f, "contains", std::move(v)); }
inline Filter exists(std::string_view f, bool present = true) { return detail::leaf(f, "exists", present); }

inline Filter all(std::initializer_list<Filter> clauses) {
    Json arr = Json::array();
    for (const auto& c : clauses) arr.push_back(c.to_json());
    return Filter(Json{{"and", std::move(arr)}});
}
inline Filter any(std::initializer_list<Filter> clauses) {
    Json arr = Json::array();
    for (const auto& c : clauses) arr.push_back(c.to_json());
    return Filter(Json{{"or", std::move(arr)}});
}
inline Filter not_(const Filter& clause) { return !clause; }

// ── Validator ────────────────────────────────────────────────────────────────
// A field allow-list entry: name plus its advertised type ("" = name-only gating).
struct FieldSpec {
    std::string_view name;
    std::string_view type;   // "keyword"|"text"|"int"|"float"|"bool"|"date"|"geo"|""
};

namespace detail {
inline Error bad(std::string_view path, std::string message) {
    return Error{errc::InvalidParams,
                 message + " (" + std::string(path) + ")",
                 Json{{"field", std::string(path)}}};
}

inline std::optional<std::string_view> field_type(const std::vector<FieldSpec>& fields,
                                                   std::string_view name, bool& gated) {
    gated = !fields.empty();
    for (const auto& f : fields) if (f.name == name) return f.type;
    return std::nullopt;
}

inline Result<Json> validate_node(const Json& node,
                                  const std::vector<FieldSpec>& fields,
                                  const std::vector<std::string_view>& operators,
                                  const std::string& path, int depth) {
    if (depth < 0) return std::unexpected(bad(path, "filter nesting too deep"));
    if (!node.is_object()) return std::unexpected(bad(path, "filter node must be an object"));

    const bool has_and = node.contains("and"), has_or = node.contains("or"), has_not = node.contains("not");
    const int combs = int(has_and) + int(has_or) + int(has_not);
    const bool is_leaf = node.contains("field") || node.contains("op");
    if (combs > 1) return std::unexpected(bad(path, "filter node has multiple combinators"));
    if (combs == 1 && is_leaf) return std::unexpected(bad(path, "filter node mixes a combinator with a leaf"));

    if (has_not) {
        auto inner = validate_node(node["not"], fields, operators, path + ".not", depth - 1);
        if (!inner) return inner;
        return Json{{"not", *inner}};
    }
    if (has_and || has_or) {
        const char* key = has_and ? "and" : "or";
        const Json& kids = node[key];
        if (!kids.is_array())
            return std::unexpected(bad(path + "." + key, std::string(key) + " must be an array"));
        Json out = Json::array();
        for (std::size_t i = 0; i < kids.size(); ++i) {
            auto c = validate_node(kids[i], fields, operators,
                                   path + "." + key + "[" + std::to_string(i) + "]", depth - 1);
            if (!c) return c;
            out.push_back(*c);
        }
        return Json{{key, std::move(out)}};
    }

    if (!is_leaf) return std::unexpected(bad(path, "filter node is neither a combinator nor a leaf"));

    // Leaf: {field, op, value?}
    if (!node.contains("field") || !node["field"].is_string() || node["field"].get<std::string>().empty())
        return std::unexpected(bad(path, "leaf requires a non-empty string 'field'"));
    if (!node.contains("op") || !node["op"].is_string())
        return std::unexpected(bad(path + ".op", "leaf requires a string 'op'"));
    const std::string field = node["field"].get<std::string>();
    const std::string op = node["op"].get<std::string>();

    if (!detail::is_known_op(op))
        return std::unexpected(bad(path + ".op", "unknown operator '" + op + "'"));
    if (!operators.empty()) {
        bool ok = false;
        for (auto a : operators) if (a == op) { ok = true; break; }
        if (!ok) return std::unexpected(bad(path + ".op", "operator '" + op + "' not advertised"));
    }
    bool gated = false;
    auto ftype = field_type(fields, field, gated);
    if (gated && !ftype)
        return std::unexpected(bad(path + "." + field, "field '" + field + "' not advertised"));

    // Value shape checks.
    if (!detail::is_unary_op(op)) {
        if (!node.contains("value"))
            return std::unexpected(bad(path + ".value", "operator '" + op + "' requires a 'value'"));
        const Json& v = node["value"];
        if (detail::is_array_op(op)) {
            if (!v.is_array() || v.empty())
                return std::unexpected(bad(path + ".value", "operator '" + op + "' requires a non-empty array"));
            for (const auto& e : v)
                if (e.is_array() || e.is_object())
                    return std::unexpected(bad(path + ".value", "operator '" + op + "' array must hold scalars"));
        } else if ((detail::is_scalar_op(op) || detail::is_string_op(op)) && (v.is_array() || v.is_object())) {
            return std::unexpected(bad(path + ".value", "operator '" + op + "' requires a scalar value"));
        }
    }
    if (detail::is_ordered_op(op) && ftype && !ftype->empty() && !detail::is_ordered_type(*ftype))
        return std::unexpected(bad(path + ".op", "operator '" + op + "' not valid on '" + std::string(*ftype) + "' field"));

    Json out{{"field", field}, {"op", op}};
    if (detail::is_unary_op(op)) out["value"] = node.value("value", true);
    else out["value"] = node["value"];
    return out;
}
} // namespace detail

// Validate + normalize an incoming filter (spec §8). Server-side entry point.
//
// Returns an empty optional (via a null Json) for an absent/empty filter, else a
// normalized wire object. Returns Error -32602 with data.field naming the
// offending path for anything malformed, unauthorized, or type-mismatched — so a
// server never hand-parses and never throws on bad input.
//
//  * fields    — advertised filter.fields; empty = allow any field.
//  * operators — advertised operator allow-list; empty = allow all spec ops.
//  * max_depth — reject pathologically deep trees (DoS guard, spec §15.5).
[[nodiscard]] inline Result<Json> validate(const Json& node,
                                           std::vector<FieldSpec> fields = {},
                                           std::vector<std::string_view> operators = {},
                                           int max_depth = 32) {
    if (node.is_null()) return Json(nullptr);
    if (node.is_object() && node.empty()) return Json(nullptr);
    return detail::validate_node(node, fields, operators, "filter", max_depth);
}

} // namespace rcp::filter
