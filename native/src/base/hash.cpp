// base/hash -- see hash.h.
#include "base/hash.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace native::base {

namespace {

constexpr uint64_t P1 = 11400714785074694791ULL;
constexpr uint64_t P2 = 14029467366897019727ULL;
constexpr uint64_t P3 = 1609587929392839161ULL;
constexpr uint64_t P4 = 9650029242287828579ULL;
constexpr uint64_t P5 = 2870177450012600261ULL;

inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t read64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint32_t read32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t round1(uint64_t acc, uint64_t input) { return rotl(acc + input * P2, 31) * P1; }
inline uint64_t mergeRound(uint64_t acc, uint64_t val) { return (acc ^ round1(0, val)) * P1 + P4; }

// Incremental feeder for the JSON walk: collects bytes and hashes them once at the end (documents are small per node).
struct Bytes {
    std::string buf;
    void tag(char c) { buf.push_back(c); }
    void u64(uint64_t v) { buf.append(reinterpret_cast<const char*>(&v), 8); }
    void str(const std::string& s) { u64(s.size()); buf.append(s); }
};

template <class J>
void feed(Bytes& out, const J& v) {
    switch (v.type()) {
        case J::value_t::null: out.tag('n'); break;
        case J::value_t::boolean: out.tag(v.get<bool>() ? 't' : 'f'); break;
        case J::value_t::number_integer: out.tag('i'); out.u64(static_cast<uint64_t>(v.get<int64_t>())); break;
        case J::value_t::number_unsigned: {
            const uint64_t u = v.get<uint64_t>();
            if (u <= static_cast<uint64_t>(INT64_MAX)) { out.tag('i'); out.u64(u); }
            else { out.tag('u'); out.u64(u); }
            break;
        }
        case J::value_t::number_float: {
            const double d = v.get<double>();
            double ip;
            if (std::isfinite(d) && std::modf(d, &ip) == 0.0 && std::fabs(d) < 9.0e18) {
                out.tag('i'); out.u64(static_cast<uint64_t>(static_cast<int64_t>(d)));
            } else {
                out.tag('d');
                uint64_t bits;
                const double canon = (d == 0.0) ? 0.0 : d;   // -0.0 == 0.0
                std::memcpy(&bits, &canon, 8);
                out.u64(bits);
            }
            break;
        }
        case J::value_t::string: out.tag('s'); out.str(v.get_ref<const std::string&>()); break;
        case J::value_t::array:
            out.tag('['); out.u64(v.size());
            for (const auto& e : v) feed(out, e);
            break;
        case J::value_t::object: {
            out.tag('{'); out.u64(v.size());
            std::vector<const std::string*> keys;
            keys.reserve(v.size());
            for (auto it = v.begin(); it != v.end(); ++it) keys.push_back(&it.key());
            std::sort(keys.begin(), keys.end(), [](const std::string* a, const std::string* b) { return *a < *b; });
            for (const std::string* k : keys) { out.str(*k); feed(out, v.at(*k)); }
            break;
        }
        case J::value_t::binary: out.tag('b'); out.str(std::string(v.get_binary().begin(), v.get_binary().end())); break;
        case J::value_t::discarded: out.tag('x'); break;
    }
}

} // namespace

uint64_t hashBytes(const void* data, size_t len, uint64_t seed) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* const end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t* const limit = end - 32;
        do {
            v1 = round1(v1, read64(p)); p += 8;
            v2 = round1(v2, read64(p)); p += 8;
            v3 = round1(v3, read64(p)); p += 8;
            v4 = round1(v4, read64(p)); p += 8;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = mergeRound(h, v1);
        h = mergeRound(h, v2);
        h = mergeRound(h, v3);
        h = mergeRound(h, v4);
    } else {
        h = seed + P5;
    }
    h += static_cast<uint64_t>(len);
    while (p + 8 <= end) {
        h ^= round1(0, read64(p));
        h = rotl(h, 27) * P1 + P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= static_cast<uint64_t>(read32(p)) * P1;
        h = rotl(h, 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<uint64_t>(*p) * P5;
        h = rotl(h, 11) * P1;
        ++p;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

template <class J>
static uint64_t hashJsonImpl(const J& value, uint64_t seed) {
    Bytes b;
    feed(b, value);
    return hashBytes(b.buf.data(), b.buf.size(), seed);
}

uint64_t hashJson(const nlohmann::json& value, uint64_t seed) { return hashJsonImpl(value, seed); }
uint64_t hashJson(const nlohmann::ordered_json& value, uint64_t seed) { return hashJsonImpl(value, seed); }

} // namespace native::base
