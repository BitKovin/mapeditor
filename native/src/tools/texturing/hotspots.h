// tools/texturing/hotspots -- Scythe-style hotspots (port of shell/src/model/hotspots.ts): rectangles drawn on a texture (texture properties window); a face picks the one that
// fits its size and shows exactly that rectangle, so atlas-like textures (a trim sheet, a door + frame + panel sheet) can be applied to faces of any size.
// Rectangles are in image coordinates 0..1, y down.
// Thread: any (pure). Spec: 07_EDITOR_PORT.md B (model/hotspots.ts), 08_MILESTONES.md M11 item 3.
#pragma once

#include <core/brush.h>
#include <core/texplan.h>

#include <vector>

#include "tools/texturing/surface.h"

namespace native::tools::tex {

struct Hotspot {
    double x = 0, y = 0, w = 0, h = 0;
    bool operator==(const Hotspot& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};
constexpr size_t kMaxHotspots = 64;
constexpr double kHotspotMin = 0.01;

/// The size of a texture: pixels and pixels per metre.
struct TexSize {
    double width = 64, height = 64, ppm = 64;
};
inline TexSize texSizeOf(const core::texplan::TexInfo& i) { return {static_cast<double>(i.width), static_cast<double>(i.height), i.ppm}; }

/// Valid rectangles only, clamped into the texture, at most kMaxHotspots (the same leniency as the Shell's parseHotspots).
std::vector<Hotspot> parseHotspots(const nlohmann::json& j);
nlohmann::json hotspotsJson(const std::vector<Hotspot>& hotspots);

/// Size of the face along its texture axes (axial, unrotated) in metres.
V2 faceSize(const core::Brush& b, const core::BrushFace& f);
/// Natural size of a hotspot in metres (what it measures at the texture's pixels per metre).
V2 hotspotSize(const Hotspot& h, const TexSize& info);
/// The best hotspot for a face of `face` metres: the least distortion first, then the least change of size. -1 without hotspots; ties go to the earlier one.
int chooseHotspot(const std::vector<Hotspot>& hotspots, const TexSize& info, const V2& face);
/// The projection that makes the face show exactly the hotspot (bottom-left origin kept), pulled in by the edge inset so the linear filter does not blend the neighbouring part
/// of the sheet in.
Projection hotspotProjection(const core::Brush& b, const core::BrushFace& f, const Hotspot& h, const TexSize& info);
/// The hotspot (index) under an image point, the smallest when they overlap; -1 when none.
int hotspotAt(const std::vector<Hotspot>& hotspots, double x, double y);
/// A rectangle from two corner points (any order), clamped into the texture, at least `min` in size.
Hotspot rectFromCorners(double ax, double ay, double bx, double by, double min = kHotspotMin);
/// Moves a rectangle by (dx, dy), kept inside the texture.
Hotspot moveRect(const Hotspot& r, double dx, double dy);

} // namespace native::tools::tex
