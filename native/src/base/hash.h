// base/hash -- 64-bit content hashes (xxHash64 algorithm) for bytes, strings and JSON values.
//
// Owns: the hash used for every cache key and "inputs hash" in the scene / lighting code (04_SCENE_AND_DERIVED.md §4).
// Thread: any (pure functions).
// Spec: 01_ARCHITECTURE.md §4 (base), 08_MILESTONES.md T1.1.
//
// hashJson() is canonical: the same value gives the same hash whatever the key insertion order of objects was, and a number
// that is integral hashes the same whether it was stored as an integer or as a double (1 and 1.0 compare equal in
// nlohmann::json, so they must hash equal).
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace native::base {

uint64_t hashBytes(const void* data, size_t size, uint64_t seed = 0);

inline uint64_t hashString(std::string_view s, uint64_t seed = 0) { return hashBytes(s.data(), s.size(), seed); }

// Order-dependent combination of two hashes (boost::hash_combine style, 64-bit).
inline uint64_t hashCombine(uint64_t seed, uint64_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 12) + (seed >> 4));
}

uint64_t hashJson(const nlohmann::json& value, uint64_t seed = 0);
uint64_t hashJson(const nlohmann::ordered_json& value, uint64_t seed = 0);   // key order is ignored here too

} // namespace native::base
