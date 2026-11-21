// tools/texturing/hotspots -- see hotspots.h.
#include "tools/texturing/hotspots.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "doc/json_write.h"

namespace native::tools::tex {

using nlohmann::json;

namespace {
bool fin(const json& v) { return v.is_number() && std::isfinite(v.get<double>()); }
double clamp(double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }
} // namespace

std::vector<Hotspot> parseHotspots(const json& j) {
    std::vector<Hotspot> out;
    if (!j.is_array()) return out;
    for (const json& r : j) {
        if (!r.is_object() || !r.contains("x") || !r.contains("y") || !r.contains("w") || !r.contains("h")) continue;
        if (!fin(r["x"]) || !fin(r["y"]) || !fin(r["w"]) || !fin(r["h"])) continue;
        Hotspot h;
        h.x = clamp(r["x"].get<double>(), 0, 1 - kHotspotMin);
        h.y = clamp(r["y"].get<double>(), 0, 1 - kHotspotMin);
        h.w = clamp(r["w"].get<double>(), kHotspotMin, 1 - h.x);
        h.h = clamp(r["h"].get<double>(), kHotspotMin, 1 - h.y);
        out.push_back(h);
        if (out.size() >= kMaxHotspots) break;
    }
    return out;
}

json hotspotsJson(const std::vector<Hotspot>& hotspots) {
    json j = json::array();
    for (const Hotspot& h : hotspots) j.push_back({{"x", doc::numberJson(h.x)}, {"y", doc::numberJson(h.y)}, {"w", doc::numberJson(h.w)}, {"h", doc::numberJson(h.h)}});
    return j;
}

V2 faceSize(const core::Brush& b, const core::BrushFace& f) {
    Projection p;
    p.scale = {1, 1};
    const Bounds bd = faceBounds(b, f, p);
    return {bd.max[0] - bd.min[0], bd.max[1] - bd.min[1]};
}

V2 hotspotSize(const Hotspot& h, const TexSize& info) {
    return {h.w * info.width / info.ppm, h.h * info.height / info.ppm};
}

int chooseHotspot(const std::vector<Hotspot>& hotspots, const TexSize& info, const V2& face) {
    int best = -1;
    double bestScore = std::numeric_limits<double>::infinity();
    const double fw = std::max(face[0], 1e-6), fh = std::max(face[1], 1e-6);
    for (size_t i = 0; i < hotspots.size(); ++i) {
        const V2 hs = hotspotSize(hotspots[i], info);
        const double distortion = std::fabs(std::log(fw / fh / (hs[0] / hs[1])));
        const double size = std::fabs(std::log(std::sqrt((fw * fh) / (hs[0] * hs[1]))));
        const double score = 3 * distortion + size;
        if (score < bestScore - 1e-12) {
            bestScore = score;
            best = static_cast<int>(i);
        }
    }
    return best;
}

Projection hotspotProjection(const core::Brush& b, const core::BrushFace& f, const Hotspot& h, const TexSize& info) {
    const double ix = core::texplan::edgeInsetTexels(info.width) / info.width, iy = core::texplan::edgeInsetTexels(info.height) / info.height;
    const double x0 = h.x + std::min(ix, h.w / 4), x1 = h.x + h.w - std::min(ix, h.w / 4);
    const double y0 = h.y + std::min(iy, h.h / 4), y1 = h.y + h.h - std::min(iy, h.h / 4);
    Projection unit;
    unit.scale = {1, 1};
    const Bounds bd = faceBounds(b, f, unit);
    const double su = std::max(bd.max[0] - bd.min[0], 1e-6) / (x1 - x0), sv = std::max(bd.max[1] - bd.min[1], 1e-6) / (y1 - y0);
    Projection out;
    out.scale = {su, sv};
    out.shift = {x0 - bd.min[0] / su, y0 - bd.min[1] / sv};
    out.rotate = 0;
    return out;
}

int hotspotAt(const std::vector<Hotspot>& hotspots, double x, double y) {
    int best = -1;
    double bestArea = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < hotspots.size(); ++i) {
        const Hotspot& h = hotspots[i];
        if (x >= h.x && x <= h.x + h.w && y >= h.y && y <= h.y + h.h && h.w * h.h < bestArea) {
            best = static_cast<int>(i);
            bestArea = h.w * h.h;
        }
    }
    return best;
}

Hotspot rectFromCorners(double ax, double ay, double bx, double by, double min) {
    const auto cl = [](double v) { return std::min(1.0, std::max(0.0, v)); };
    const double x0 = cl(std::min(ax, bx)), x1 = cl(std::max(ax, bx)), y0 = cl(std::min(ay, by)), y1 = cl(std::max(ay, by));
    const double w = std::max(x1 - x0, min), h = std::max(y1 - y0, min);
    return {std::min(x0, 1 - w), std::min(y0, 1 - h), w, h};
}

Hotspot moveRect(const Hotspot& r, double dx, double dy) {
    return {std::min(1 - r.w, std::max(0.0, r.x + dx)), std::min(1 - r.h, std::max(0.0, r.y + dy)), r.w, r.h};
}

} // namespace native::tools::tex
