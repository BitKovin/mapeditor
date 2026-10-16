#pragma once
#include "core/profile.h"
#include <array>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace core {

// Light styles (REQUIREMENTS §9, DESIGN §6.8). The engine model, taken from the reference engine: a light has a style id; every surface has up to
// kMaxStylesPerSurface slots, each a lightmap LAYER baked with only the lights of that style; the renderer shows
//     light = sum over slots of  lightmap_slot * styleValue(style_slot, time)
// Style 0 is the always-on base (value 1). Any other style plays a pattern at 10 Hz: one letter per frame, 'a' = 0, 'm' = 1, 'z' = 25/12 (brighter than
// normal). A style without a pattern is constant 1.
inline constexpr int kMaxStyleId = 63;            // size of the engine's pattern table
inline constexpr int kMaxStylesPerSurface = 8;    // slots, base style 0 included (the new BSP version)
inline constexpr double kStyleHz = 10.0;

struct LightStyleTable {
    std::array<std::string, kMaxStyleId + 1> patterns;  // empty = constant 1

    // The engine's built-in patterns (styles 0..11); everything else empty.
    static LightStyleTable defaults();
    // defaults overridden by the profile's `lighting.lightStyles` (id -> pattern).
    static LightStyleTable fromProfile(const GameProfile& profile);

    // Brightness multiplier of a style at `seconds`.
    double value(int style, double seconds) const;
    // All values of one pattern frame (what the Shell plays back too).
    nlohmann::json toJson() const;
};

// Valid pattern: letters a..z only.
bool validStylePattern(const std::string& p);

}  // namespace core
