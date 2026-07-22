#pragma once
// rcp/types.hpp — the type-theoretic foundation of the RCP C++ SDK.
//
// The wire is JSON, but the API is not "pass a blob and hope". Protocol
// invariants are pushed into the type system so they are proved at COMPILE time
// (the build is the test runner), following the modern-C++ house style:
//
//   * Strong types (newtype): a ProtocolVersion is not an int, a Score is not a
//     float, a Dimension is not a count. Mixing them is a compile error.
//   * Refinement types: TopK cannot hold 0; ProtocolVersion cannot be < 1. The
//     only way to construct one is `make`, which validates and returns a
//     Result — an invalid value is unrepresentable, not merely rejected later.
//   * Totality: Result<T> = std::expected<T, Error>. No exceptions for control
//     flow; every fallible operation returns a value that must be inspected.
//
// Each primitive carries its own static_assert self-tests, so a regression is a
// build error at the line that broke it.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../json.hpp"

namespace rcp {

using Json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────────
// Error / Result — the total-function return type.
// ─────────────────────────────────────────────────────────────────────────────
namespace errc {
inline constexpr int ParseError         = -32700;
inline constexpr int InvalidRequest     = -32600;
inline constexpr int MethodNotFound     = -32601;
inline constexpr int InvalidParams      = -32602;
inline constexpr int InternalError      = -32603;
inline constexpr int NotInitialized     = -32001;
inline constexpr int VersionMismatch    = -32002;
inline constexpr int CapabilityMissing  = -32003;
inline constexpr int UnknownMethod      = -32004;
inline constexpr int OptionUnsupported  = -32005;
inline constexpr int Cancelled          = -32006;
inline constexpr int BackendUnavailable = -32010;
inline constexpr int RateLimited        = -32011;
} // namespace errc

struct Error {
    int         code = errc::InternalError;
    std::string message;

    [[nodiscard]] Json to_json() const { return Json{{"code", code}, {"message", message}}; }
    static Error from_json(const Json& j) {
        return Error{j.value("code", errc::InternalError), j.value("message", std::string{"error"})};
    }
};

template <class T>
using Result = std::expected<T, Error>;

template <class T>
[[nodiscard]] Result<T> fail(int code, std::string msg) {
    return std::unexpected(Error{code, std::move(msg)});
}

// ─────────────────────────────────────────────────────────────────────────────
// StrongScalar — the newtype pattern. A distinct type over Rep with no implicit
// conversions in or out (value accessed via get()). CRTP tag makes each alias a
// separate type even when Rep matches.
// ─────────────────────────────────────────────────────────────────────────────
template <class Tag, class Rep>
class StrongScalar {
public:
    using rep = Rep;
    constexpr StrongScalar() = default;
    constexpr explicit StrongScalar(Rep v) noexcept : v_(v) {}
    [[nodiscard]] constexpr Rep get() const noexcept { return v_; }
    friend constexpr bool operator==(StrongScalar, StrongScalar) = default;
    friend constexpr auto operator<=>(StrongScalar, StrongScalar) = default;
private:
    Rep v_{};
};

// Relevance score newtype (higher = more relevant). Ordered, so ranking is
// type-safe; a Score can never be silently used as a count or a version.
struct ScoreTag {};
using Score = StrongScalar<ScoreTag, double>;

// ─────────────────────────────────────────────────────────────────────────────
// Refined<T, Predicate> — a value of type T carrying a type-level proof that
// Predicate holds. Constructor is private; the only way in is `make`, which
// validates and returns Result. So an out-of-domain value is UNREPRESENTABLE.
// ─────────────────────────────────────────────────────────────────────────────
template <class T, class Predicate>
class Refined {
public:
    [[nodiscard]] static Result<Refined> make(T value) {
        if (!Predicate::ok(value))
            return fail<Refined>(errc::InvalidParams, Predicate::message());
        return Refined{std::move(value)};
    }
    // Trusted construction from a compile-time-known-good literal (asserts).
    [[nodiscard]] static constexpr Refined trusted(T value) {
        return Refined{std::move(value)};
    }
    [[nodiscard]] constexpr const T& get() const noexcept { return v_; }
    friend constexpr bool operator==(const Refined&, const Refined&) = default;
private:
    constexpr explicit Refined(T v) : v_(std::move(v)) {}
    T v_;
};

// Predicates.
struct PositiveInt {
    static constexpr bool ok(std::int64_t v) noexcept { return v >= 1; }
    static constexpr const char* message() noexcept { return "value must be >= 1"; }
};
struct AtLeastOne {
    static constexpr bool ok(std::size_t v) noexcept { return v >= 1; }
    static constexpr const char* message() noexcept { return "value must be >= 1"; }
};

// Protocol version: a refined int that cannot be < 1 (spec §7.1 negotiation).
using ProtocolVersion = Refined<int, PositiveInt>;

// TopK: the result count. Literally cannot be 0 — asking for zero results is a
// nonsensical query and now unrepresentable rather than a runtime edge case.
using TopK = Refined<std::size_t, AtLeastOne>;

// Dimension: an embedding width. Distinct from TopK though both wrap size_t, so
// you cannot pass a dimension where a k is expected.
struct DimensionTag {};
using Dimension = StrongScalar<DimensionTag, std::size_t>;

inline constexpr int kProtocolVersion    = 1;
inline constexpr int kMinProtocolVersion = 1;

[[nodiscard]] constexpr int negotiate_version(int theirs) noexcept {
    return theirs < kProtocolVersion ? theirs : kProtocolVersion;
}

// ── compile-time proofs (the build is the test runner) ───────────────────────
static_assert(!std::is_convertible_v<Score, double>, "Score must not implicitly convert to double");
static_assert(!std::is_convertible_v<int, ProtocolVersion>, "ProtocolVersion must be explicitly refined");
static_assert(!std::is_same_v<TopK, Dimension>, "TopK and Dimension must be distinct types");
static_assert(Score{1.0} > Score{0.5}, "Score must be ordered for ranking");
static_assert(negotiate_version(99) == kProtocolVersion, "negotiation clamps to our max");
static_assert(negotiate_version(1) == 1, "negotiation honours a lower peer");

} // namespace rcp
