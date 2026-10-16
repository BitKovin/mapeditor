#pragma once
#include "core/derived.h"
#include "core/light_styles.h"
#include "core/lighting_classes.h"
#include "core/lights.h"
#include "core/raytrace.h"
#include <map>
#include <string>
#include <vector>

namespace core {

// Which styled lights touch which face (REQUIREMENTS §9: "arrange charts so that as few styles as possible touch each surface; q3map2 assigns a style
// to a face even if that light never reaches it"). A light of style s != 0 touches a face when, from some sample point of the face, the light is in
// range (attenuation radius / cone / rect side), the point faces it and a ray to the light (to a few points of its source) is not blocked by a shadow
// caster. Style 0 is the base layer every chart has and is not listed.
//
// When more styles touch a face than the slots allow (kMaxStylesPerSurface including the base), the ones with the smallest influence are discarded:
// influence = mean over the face's sample points of the unshadowed irradiance estimate brightness * luminance(colour) * window(d / radius) * cos / d^2.
struct StyleAnalysis {
    std::map<std::string, std::vector<int>> faceStyles;  // LightmapFace::key() -> kept non-base styles, ascending (faces without any are absent)
    std::map<std::string, std::vector<int>> discarded;   // same key -> styles that did not fit
    std::vector<std::string> warnings;
};

struct StyleAnalysisSettings {
    int samplesPerFace = 64;
    int maxSlots = kMaxStylesPerSurface;
};

// `bvh` (built from the shadow casters, without a layout) may be null: then nothing is occluded.
StyleAnalysis analyseLightStyles(const std::vector<LightSurface>& surfaces, const LightingClasses& classes, const LightScene& lights, const TriangleBvh* bvh,
                                 const StyleAnalysisSettings& settings = {});

}  // namespace core
