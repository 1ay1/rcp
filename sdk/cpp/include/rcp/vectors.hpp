#pragma once
// rcp/vectors.hpp — compact dense-vector codec for RCP/1 (spec §7.3.1).
//
// The wire carries embeddings either as JSON number arrays (`"json"`, the
// default) or as base64-encoded little-endian IEEE-754 binary32 (`"f32-base64"`).
// A 1000×1024 batch is ~4 MB binary vs ~10–20 MB as decimal text, so the compact
// encoding is a real bandwidth win for bulk embedding.
//
// Header-only, no dependencies beyond the SDK's Result<T>. This is the reference
// implementation every SDK matches byte-for-byte: little-endian, 4 bytes per
// float, standard base64 with padding.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "rcp/types.hpp"

namespace rcp::vectors {

inline constexpr const char* kJson      = "json";
inline constexpr const char* kF32Base64 = "f32-base64";

namespace detail {

inline const char* b64_alphabet() {
    return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

inline std::string b64_encode(const std::uint8_t* raw, std::size_t len) {
    const char* A = b64_alphabet();
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        std::uint32_t b0 = raw[i];
        std::uint32_t b1 = i + 1 < len ? raw[i + 1] : 0;
        std::uint32_t b2 = i + 2 < len ? raw[i + 2] : 0;
        std::uint32_t n = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(A[(n >> 18) & 63]);
        out.push_back(A[(n >> 12) & 63]);
        out.push_back(i + 1 < len ? A[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? A[n & 63] : '=');
    }
    return out;
}

inline Result<int> b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return fail<int>(errc::InvalidParams, "invalid base64 character");
}

inline Result<std::vector<std::uint8_t>> b64_decode(const std::string& s) {
    std::vector<std::uint8_t> out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        auto v = b64_val(c);
        if (!v) return std::unexpected(v.error());
        acc = (acc << 6) | static_cast<std::uint32_t>(*v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xff));
        }
    }
    return out;
}

} // namespace detail

// Encode one float vector as an f32-base64 blob (little-endian binary32).
[[nodiscard]] inline std::string encode_f32_base64(const std::vector<float>& v) {
    std::vector<std::uint8_t> raw(v.size() * 4);
    for (std::size_t i = 0; i < v.size(); ++i) {
        std::uint32_t bits;
        std::memcpy(&bits, &v[i], 4);          // IEEE-754 binary32 bit pattern
        raw[i * 4 + 0] = bits & 0xff;          // little-endian
        raw[i * 4 + 1] = (bits >> 8) & 0xff;
        raw[i * 4 + 2] = (bits >> 16) & 0xff;
        raw[i * 4 + 3] = (bits >> 24) & 0xff;
    }
    return detail::b64_encode(raw.data(), raw.size());
}

// Decode an f32-base64 blob back into a float vector. `dimension`, when > 0, is
// enforced. Fails on a blob whose byte length is not a whole number of floats,
// or not `dimension*4` bytes.
[[nodiscard]] inline Result<std::vector<float>>
decode_f32_base64(const std::string& blob, std::size_t dimension = 0) {
    auto raw = detail::b64_decode(blob);
    if (!raw) return std::unexpected(raw.error());
    if (raw->size() % 4 != 0)
        return fail<std::vector<float>>(errc::InvalidParams,
            "f32-base64 blob length is not a multiple of 4 bytes");
    const std::size_t n = raw->size() / 4;
    if (dimension != 0 && n != dimension)
        return fail<std::vector<float>>(errc::InvalidParams,
            "f32-base64 blob has wrong dimension");
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t bits = static_cast<std::uint32_t>((*raw)[i * 4 + 0])
                           | (static_cast<std::uint32_t>((*raw)[i * 4 + 1]) << 8)
                           | (static_cast<std::uint32_t>((*raw)[i * 4 + 2]) << 16)
                           | (static_cast<std::uint32_t>((*raw)[i * 4 + 3]) << 24);
        std::memcpy(&out[i], &bits, 4);
    }
    return out;
}

// Encode a batch under `encoding`. For "f32-base64" all vectors MUST share one
// dimension; returns { "encoding", "dimension", "vectors":[blob…] }. For "json"
// returns the vectors as a plain nested number array (default wire form).
[[nodiscard]] inline Result<Json>
encode_batch(const std::vector<std::vector<float>>& vectors, const std::string& encoding) {
    if (encoding == kJson) {
        Json arr = Json::array();
        for (const auto& v : vectors) arr.push_back(v);
        return arr;
    }
    if (encoding == kF32Base64) {
        std::size_t dim = vectors.empty() ? 0 : vectors[0].size();
        Json blobs = Json::array();
        for (const auto& v : vectors) {
            if (v.size() != dim)
                return fail<Json>(errc::InvalidParams,
                    "all vectors must share one dimension for f32-base64");
            blobs.push_back(encode_f32_base64(v));
        }
        return Json{{"encoding", kF32Base64}, {"dimension", dim}, {"vectors", blobs}};
    }
    return fail<Json>(errc::OptionUnsupported, "unknown vector encoding");
}

} // namespace rcp::vectors
