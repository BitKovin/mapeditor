#include "core/light_styles.h"
#include <cmath>

namespace core {

bool validStylePattern(const std::string& p) {
    for (char c : p)
        if (c < 'a' || c > 'z') return false;
    return true;
}

LightStyleTable LightStyleTable::defaults() {
    LightStyleTable t;
    // the reference engine's table (Quake 3 / q3map2 lightstyles); id 0 is the base style
    const char* known[] = {"m",
                           "mmnmmommommnonmmonqnmmo",
                           "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba",
                           "mmmmmaaaaammmmmaaaaaabcdefgabcdefg",
                           "mamamamamama",
                           "jklmnopqrstuvwxyzyxwvutsrqponmlkj",
                           "nmonqnmomnmomomno",
                           "mmmaaaabcdefgmmmmaaaammmaamm",
                           "mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa",
                           "aaaaaaaazzzzzzzz",
                           "mmamammmmammamamaaamammma",
                           "abcdefghijklmnopqrrqponmlkjihgfedcba"};
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) t.patterns[i] = known[i];
    return t;
}

LightStyleTable LightStyleTable::fromProfile(const GameProfile& profile) {
    LightStyleTable t = defaults();
    for (const auto& [id, pattern] : profile.lightStyles)
        if (id >= 1 && id <= kMaxStyleId) t.patterns[static_cast<size_t>(id)] = pattern;  // style 0 stays the base
    return t;
}

double LightStyleTable::value(int style, double seconds) const {
    if (style <= 0 || style > kMaxStyleId) return 1.0;
    const std::string& p = patterns[static_cast<size_t>(style)];
    if (p.empty()) return 1.0;
    const long long frame = static_cast<long long>(std::floor(seconds * kStyleHz));
    const char c = p[static_cast<size_t>(((frame % static_cast<long long>(p.size())) + static_cast<long long>(p.size())) % static_cast<long long>(p.size()))];
    return (c - 'a') / static_cast<double>('m' - 'a');
}

nlohmann::json LightStyleTable::toJson() const {
    nlohmann::json j = nlohmann::json::array();
    for (const auto& p : patterns) j.push_back(p);
    return j;
}

}  // namespace core
