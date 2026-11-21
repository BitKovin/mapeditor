// tools/texturing/density -- the texel density heat map (port of shell/src/model/texelDensity.ts, REQUIREMENTS section 19 A3). A texture's `pixelsPerMeter` says how sharp it was
// authored to look; a face that shows it larger or smaller has a different density:   density = pixels per repeat / metres per repeat (per axis);
// e = log2(density / authored ppm):  0 = as authored, -1 = stretched 2x (blurry), +1 = squeezed 2x. A face is judged by its worse (blurrier) axis.
// Thread: any (pure). Spec: 07_EDITOR_PORT.md B, 08_MILESTONES.md M11 item 3.
#pragma once

#include <core/brush.h>
#include <core/texplan.h>

#include <array>
#include <optional>
#include <string>

#include "tools/texturing/surface.h"

namespace native::tools::tex {

/// The colour ramp saturates at +/- this many octaves (4x).
constexpr double kHeatRange = 2;
constexpr int kHeatBuckets = 17;

/// Pixels per metre along u and v that the face's map gives a texture of `width` x `height` pixels.
V2 faceDensity(const core::Brush& b, const core::BrushFace& f, double width, double height, const InfoFn& info);
/// Octaves relative to the authored density; nullopt when the texture is not loaded (`texture` null).
std::optional<double> faceHeat(const core::Brush& b, const core::BrushFace& f, const core::texplan::TexInfo* texture, const InfoFn& info);
/// Heat colour (0..1 rgb) of an octave value: blue (dense) - green (as authored) - red (stretched).
std::array<double, 3> heatColor(double octaves);
/// A bucket index (0 .. kHeatBuckets - 1) of an octave value, so a viewport can draw every face of a bucket in one call.
int heatBucket(double octaves);
std::array<double, 3> bucketColor(int bucket);
std::string heatLabel(double octaves);

} // namespace native::tools::tex
