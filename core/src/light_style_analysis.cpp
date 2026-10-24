#include "core/light_style_analysis.h"
#include "core/lightmap_uv.h"
#include "core/parallel.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace core {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

double lum(const Vec3& c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }
double window(double d, double radius) {
    const double q = d / std::max(radius, 1e-6);
    const double w = std::clamp(1.0 - q * q * q * q, 0.0, 1.0);
    return w * w;
}

struct FacePiece { std::vector<Vec3> verts; Vec3 normal; };

// Deterministic points inside a triangle (R2 low discrepancy sequence folded into the triangle).
Vec3 pointIn(const Vec3& a, const Vec3& b, const Vec3& c, int i) {
    const double g = 1.32471795724474602596;
    double u = std::fmod(0.5 + i / g, 1.0), v = std::fmod(0.5 + i / (g * g), 1.0);
    if (u + v > 1.0) { u = 1.0 - u; v = 1.0 - v; }
    return a + (b - a) * u + (c - a) * v;
}

struct Reach { bool reached = false; double influence = 0; };

// Points on / around the light that a shadow ray aims at.
std::vector<Vec3> lightTargets(const Light& L) {
    std::vector<Vec3> t{L.position};
    if (L.type == LightType::Point || L.type == LightType::Spot) {
        const double r = L.sourceSize;
        if (r > 0) for (const Vec3& d : {L.right, L.up, L.right * -1.0, L.up * -1.0, L.forward}) t.push_back(L.position + d * r);
    } else if (L.type == LightType::Rect) {
        const double hw = L.width * 0.5, hh = L.height * 0.5;
        for (double sx : {-1.0, 1.0}) for (double sy : {-1.0, 1.0}) t.push_back(L.position + L.right * (sx * hw) + L.up * (sy * hh));
    }
    return t;
}

Reach reach(const Light& L, const Vec3& p, const Vec3& n, const TriangleBvh* bvh, const std::vector<Vec3>& targets) {
    Reach r;
    const Vec3 o = p + n * 0.01;
    double irradiance = 0;
    bool open = false;
    auto blocked = [&](const Vec3& to) {
        if (!bvh || !L.castShadows) return false;
        const Vec3 d = to - o;
        const double len = d.length();
        return len > 1e-6 && bvh->occluded(o, d, 1.0 - 1e-4);
    };
    if (L.type == LightType::Directional || L.type == LightType::Sky) {
        const Vec3 l = L.type == LightType::Directional ? L.forward * -1.0 : Vec3{0, 1, 0};
        const double c = L.type == LightType::Directional ? n.dot(l) : n.y * 0.5 + 0.5;  // sky: any face that sees some of the upper hemisphere
        if (c <= 1e-6) return r;
        std::vector<Vec3> dirs{l};
        if (L.type == LightType::Sky) for (const Vec3& d : {Vec3{0.5, 1, 0}, Vec3{-0.5, 1, 0}, Vec3{0, 1, 0.5}, Vec3{0, 1, -0.5}, Vec3{1, 0.3, 0}, Vec3{-1, 0.3, 0}, Vec3{0, 0.3, 1}, Vec3{0, 0.3, -1}}) dirs.push_back(d.normalized());
        for (const Vec3& dir : dirs) {
            if (n.dot(dir) <= 1e-6) continue;
            if (!blocked(o + dir * 1.0e4)) { open = true; break; }
        }
        irradiance = L.brightness * lum(L.color) * (L.type == LightType::Directional ? c : 0.5);
    } else {
        const Vec3 v = L.position - p;
        const double d = v.length();
        if (d > L.radius || d < 1e-6) return r;
        const Vec3 toLight = v * (1.0 / d);
        const double cosT = n.dot(toLight);
        if (cosT <= 1e-6) return r;
        if (L.type == LightType::Rect && (p - L.position).dot(L.forward) <= 0) return r;  // one-sided
        if (L.type == LightType::Spot) {
            const double c = (p - L.position).normalized().dot(L.forward);
            if (std::acos(std::clamp(c, -1.0, 1.0)) > L.outerAngle * kDeg + std::asin(std::min(1.0, L.sourceSize / d))) return r;
        }
        for (const Vec3& t : targets) if (!blocked(t)) { open = true; break; }
        irradiance = L.brightness * lum(L.color) * window(d, L.radius) * cosT / std::max(d * d, 0.0025);
    }
    r.reached = open;
    r.influence = open ? irradiance : 0;
    return r;
}
}  // namespace

StyleAnalysis analyseLightStyles(const std::vector<LightSurface>& surfaces, const LightingClasses& classes, const LightScene& lights, const TriangleBvh* bvh,
                                 const StyleAnalysisSettings& cfg) {
    StyleAnalysis out;
    std::vector<const Light*> styled;
    for (const Light& l : lights.lights) if (l.style != 0) styled.push_back(&l);
    if (styled.empty()) return out;
    std::vector<std::vector<Vec3>> targets;
    for (const Light* l : styled) targets.push_back(lightTargets(*l));

    // group the pieces by authored face
    std::map<std::string, std::vector<FacePiece>> faces;
    for (const auto& s : surfaces) {
        if (!classes.isLightmapped(s.classname) || s.verts.size() < 3) continue;
        LightmapFace probe;
        probe.instance = s.instance; probe.entityId = s.entityId; probe.brushInstance = s.brushInstance; probe.brushId = s.brushId;
        probe.faceId = s.faceId; probe.side = s.side;
        faces[probe.key()].push_back({s.verts, s.normal});
    }
    const int maxOthers = std::max(0, cfg.maxSlots - 1);  // the base style takes a slot
    // Every face is independent (the BVH is only read), so the faces run in parallel; the results are merged in key order afterwards.
    struct FaceResult {
        std::vector<int> ids;      // styles that reach the face (empty: none)
        std::vector<int> dropped;  // styles that did not fit
        std::string warning;
    };
    std::vector<const std::pair<const std::string, std::vector<FacePiece>>*> order;
    for (const auto& kv : faces) order.push_back(&kv);
    std::vector<FaceResult> results(order.size());
    parallelFor(order.size(), 16, [&](size_t begin, size_t end) {
    for (size_t fi = begin; fi < end; ++fi) {
        const std::string& key = order[fi]->first;
        const std::vector<FacePiece>& pieces = order[fi]->second;
        FaceResult& res = results[fi];
        // sample points: proportional to the area of each piece, at least 3 per piece
        double total = 0;
        std::vector<double> area(pieces.size(), 0.0);
        for (size_t i = 0; i < pieces.size(); ++i) {
            const auto& v = pieces[i].verts;
            for (size_t k = 1; k + 1 < v.size(); ++k) area[i] += 0.5 * (v[k] - v[0]).cross(v[k + 1] - v[0]).length();
            total += area[i];
        }
        std::vector<std::pair<Vec3, Vec3>> pts;  // point, normal
        for (size_t i = 0; i < pieces.size(); ++i) {
            const auto& v = pieces[i].verts;
            const int n = std::max(3, static_cast<int>(std::lround(cfg.samplesPerFace * (total > 0 ? area[i] / total : 1.0 / pieces.size()))));
            Vec3 c;
            for (const Vec3& p : v) c = c + p;
            c = c * (1.0 / static_cast<double>(v.size()));
            for (const Vec3& p : v) pts.push_back({p + (c - p) * 0.02, pieces[i].normal});  // the corners, pulled in
            pts.push_back({c, pieces[i].normal});
            for (int k = 0; k < n; ++k) {
                const size_t tri = static_cast<size_t>(k) % (v.size() - 2);
                pts.push_back({pointIn(v[0], v[tri + 1], v[tri + 2], k + 1), pieces[i].normal});
            }
        }
        std::map<int, double> influence;
        std::set<int> touched;
        for (const auto& [p, n] : pts)
            for (size_t li = 0; li < styled.size(); ++li) {
                const Reach r = reach(*styled[li], p, n, bvh, targets[li]);
                if (!r.reached) continue;
                touched.insert(styled[li]->style);
                influence[styled[li]->style] += r.influence / static_cast<double>(pts.size());
            }
        if (touched.empty()) continue;
        std::vector<int> ids(touched.begin(), touched.end());
        if (static_cast<int>(ids.size()) > maxOthers) {
            std::stable_sort(ids.begin(), ids.end(), [&](int a, int b) { return influence[a] > influence[b]; });
            std::vector<int> dropped(ids.begin() + maxOthers, ids.end());
            ids.resize(static_cast<size_t>(maxOthers));
            std::sort(dropped.begin(), dropped.end());
            std::string w = "light styles: face " + key + " is touched by " + std::to_string(ids.size() + dropped.size()) + " styles besides the base, only " +
                            std::to_string(maxOthers) + " fit; discarded (smallest influence):";
            for (int d : dropped) w += " " + std::to_string(d);
            res.warning = std::move(w);
            res.dropped = std::move(dropped);
        }
        std::sort(ids.begin(), ids.end());
        res.ids = std::move(ids);
    }
    });
    for (size_t fi = 0; fi < order.size(); ++fi) {
        FaceResult& res = results[fi];
        if (res.ids.empty() && res.dropped.empty()) continue;
        const std::string& key = order[fi]->first;
        if (!res.warning.empty()) {
            out.discarded[key] = std::move(res.dropped);
            out.warnings.push_back(std::move(res.warning));
        }
        out.faceStyles[key] = std::move(res.ids);
    }
    return out;
}

}  // namespace core
