// tools/texturing/props_edit -- the editing model of the Texture Properties window (the data side of shell/src/ui/texturePropsWindow.ts): one texture's sidecar properties
// as a JSON value (pixelsPerMeter, repeat per axis, hotspots, transparency; relief and unknown keys are carried along untouched). Every function returns a new value; the
// result is checked by core::sanitizeTextureProps (the same validation Core applies when it writes the sidecar). `forWrite` makes a value that REPLACES the stored keys
// (Core merges the keys that are present: an absent hotspot list would keep the old one).
// Thread: any (pure). Spec: 07_EDITOR_PORT.md B (texprops), 08_MILESTONES.md M11 item 4.
#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "tools/texturing/hotspots.h"

namespace native::tools::tex {

constexpr double kPpmMin = 1, kPpmMax = 4096;
inline constexpr double kPpmPresets[] = {16, 32, 64, 128, 256};

/// The repetition behaviour of one axis (0 = u / horizontal, 1 = v / vertical).
struct AxisProps {
    std::string mode = "repeat";     // "repeat" | "mirror" | "anchor"
    std::string anchor = "start";    // anchor mode: the edge that stays put: "start" | "end" (v defaults to "end")
    double split = 0.5;              // anchor mode: where the repeating band starts (0.02 .. 0.98 of the texture)
    std::string fill = "mirror";     // anchor mode: how the band repeats: "repeat" | "mirror"
    std::string edge = "auto";       // "auto" | "wrap" | "clamp"
    std::string altTexture;          // anchor mode: a second texture that fills the open end ("" = none)
    std::array<double, 2> altOffset{0, 0};
    bool operator==(const AxisProps& o) const {
        return mode == o.mode && anchor == o.anchor && split == o.split && fill == o.fill && edge == o.edge && altTexture == o.altTexture && altOffset == o.altOffset;
    }
};
AxisProps defaultAxis(int axis);

struct TransparencyProps {
    std::string mode = "auto";       // "auto" | "opaque" | "cutout" | "translucent"
    double cutoff = 0.5, opacity = 1;
    bool operator==(const TransparencyProps& o) const { return mode == o.mode && cutoff == o.cutoff && opacity == o.opacity; }
};

/// `props` checked and clamped (anything unusable dropped); a non-object gives an empty object.
nlohmann::json normalizedProps(const nlohmann::json& props);
/// The explicit pixels per metre; nullopt = the game profile default.
std::optional<double> ppmOf(const nlohmann::json& props);
nlohmann::json withPpm(const nlohmann::json& props, std::optional<double> ppm);
AxisProps axisOf(const nlohmann::json& props, int axis);
nlohmann::json withAxis(const nlohmann::json& props, int axis, const AxisProps& axisProps);
std::vector<Hotspot> hotspotsOf(const nlohmann::json& props);
nlohmann::json withHotspots(const nlohmann::json& props, const std::vector<Hotspot>& hotspots);
TransparencyProps transparencyOf(const nlohmann::json& props);
nlohmann::json withTransparency(const nlohmann::json& props, const TransparencyProps& t);
/// True when nothing differs from a texture without a sidecar.
bool isPlain(const nlohmann::json& props);
/// The value to hand to core::writeTextureProps so that it replaces every editable key (hotspots, repeat, transparency; pixelsPerMeter absent = the default).
nlohmann::json forWrite(const nlohmann::json& props);

} // namespace native::tools::tex
