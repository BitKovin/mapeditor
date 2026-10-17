#include <set>
#include "core/lights.h"
#include "core/derived.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace core {

namespace {
constexpr double kDeg = 3.14159265358979323846 / 180.0;

// Rotation applied to a vector: Ry(yaw) * Rx(pitch) * Rz(roll)  (same convention as decal.cpp / prefab.cpp)
Vec3 rotate(const Vec3& v, const Vec3& anglesDeg) {
    const double p = anglesDeg.x * kDeg, y = anglesDeg.y * kDeg, r = anglesDeg.z * kDeg;
    const Vec3 a{v.x * std::cos(r) - v.y * std::sin(r), v.x * std::sin(r) + v.y * std::cos(r), v.z};
    const Vec3 b{a.x, a.y * std::cos(p) - a.z * std::sin(p), a.y * std::sin(p) + a.z * std::cos(p)};
    return {b.x * std::cos(y) + b.z * std::sin(y), b.y, -b.x * std::sin(y) + b.z * std::cos(y)};
}

bool numberKey(const Entity& e, const char* key, double& out) {
    const auto it = e.keyvalues.find(key);
    if (it == e.keyvalues.end()) return false;
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    if (end == it->second.c_str() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool colorKey(const Entity& e, const char* key, Vec3& out) {
    const auto it = e.keyvalues.find(key);
    if (it == e.keyvalues.end()) return false;
    std::istringstream in(it->second);
    double r, g, b;
    if (!(in >> r >> g >> b) || !std::isfinite(r + g + b)) return false;
    out = {std::max(0.0, r), std::max(0.0, g), std::max(0.0, b)};
    return true;
}
}  // namespace

LevelSettings levelSettingsOf(const Entity& w, std::vector<std::string>* warnings) {
    LevelSettings s;
    auto num = [&](const char* key, double& out, double lo, double hi) {
        if (!w.keyvalues.count(key)) return;
        double v;
        if (!numberKey(w, key, v)) { if (warnings) warnings->push_back(std::string("worldspawn: ") + key + " is not a number; ignored"); return; }
        out = std::clamp(v, lo, hi);
    };
    auto integer = [&](const char* key, int& out, int lo, int hi) { double v = out; num(key, v, lo, hi); out = static_cast<int>(std::lround(v)); };
    num("_light_scale", s.lightScale, 0.0, 100.0);
    num("_ao_radius", s.ao.radius, 0.0, 64.0);
    num("_ao_power", s.ao.power, 0.1, 10.0);
    num("_ao_intensity", s.ao.intensity, 0.0, 1.0);
    integer("_ao_rays", s.ao.rays, 1, 1024);
    num("_ao_strength", s.ao.strength, 0.0, 16.0);
    num("_ao_falloff", s.ao.falloff, 0.0, 8.0);
    num("_cc_exposure", s.exposure, 0.0, 20.0);
    if (s.exposure > 0 && s.exposure < 0.05) s.exposure = 0.05;
    num("_cc_brightness", s.brightness, -1.0, 1.0);
    integer("_rays_per_texel", s.raysPerTexel, 0, 65536);
    integer("_supersample", s.supersample, 0, 8);  // 8 = kMaxSuperSamples (lightbake.h)
    integer("_rays_per_probe", s.raysPerProbe, 0, 1024);
    num("_lightvol_grid", s.lightvolGrid, 0.0, 64.0);
    if (s.lightvolGrid > 0 && s.lightvolGrid < 0.25) s.lightvolGrid = 0.25;
    num("_bake_quality", s.bakeQuality, 0.25, 16.0);
    return s;
}

bool isLightEntity(const Entity& e) { return e.brushes.empty() && e.classname.rfind("light_", 0) == 0; }

LightScene collectLights(const Document& doc, const std::vector<ResolvedEntity>& prefabs) {
    LightScene out;
    auto visit = [&](const Entity& e, const std::string& instance) {
        if (e.classname == "worldspawn" && instance.empty()) {
            Vec3 a;
            if (colorKey(e, "_ambient", a)) out.ambient = a;
            else if (e.keyvalues.count("_ambient")) out.warnings.push_back("worldspawn: _ambient is not \"r g b\"; ignored");
            out.level = levelSettingsOf(e, &out.warnings);
            return;
        }
        if (!isLightEntity(e)) return;
        Light l;
        l.id = e.id;
        l.instance = instance;
        if (e.classname == "light_point") l.type = LightType::Point;
        else if (e.classname == "light_spot") l.type = LightType::Spot;
        else if (e.classname == "light_directional") l.type = LightType::Directional;
        else if (e.classname == "light_rect") l.type = LightType::Rect;
        else if (e.classname == "light_sky") l.type = LightType::Sky;
        else {
            out.warnings.push_back(e.id + ": unknown light class " + e.classname + "; ignored");
            return;
        }
        l.position = e.origin;
        l.forward = rotate({0, 0, 1}, e.angles).normalized();
        l.right = rotate({1, 0, 0}, e.angles).normalized();
        l.up = rotate({0, 1, 0}, e.angles).normalized();
        colorKey(e, "color", l.color);
        double v;
        if (numberKey(e, "brightness", v)) l.brightness = std::max(0.0, v);
        if (numberKey(e, "radius", v)) l.radius = std::max(1e-3, v);
        if (numberKey(e, "bounce", v)) l.bounce = std::max(0.0, v);
        if (numberKey(e, "cast_shadows", v)) l.castShadows = v != 0;
        if (numberKey(e, "style", v)) l.style = std::clamp(static_cast<int>(v), 0, 63);
        if (numberKey(e, "source_size", v)) l.sourceSize = std::max(0.0, v);
        if (numberKey(e, "inner_angle", v)) l.innerAngle = std::clamp(v, 0.0, 89.0);
        if (numberKey(e, "outer_angle", v)) l.outerAngle = std::clamp(v, 0.1, 89.9);
        if (l.innerAngle >= l.outerAngle) l.innerAngle = l.outerAngle * 0.5;
        if (numberKey(e, "width", v)) l.width = std::max(1e-3, v);
        if (numberKey(e, "height", v)) l.height = std::max(1e-3, v);
        out.lights.push_back(std::move(l));
    };
    for (const auto& e : doc.entities) visit(e, "");
    for (const auto& r : prefabs) visit(r.entity, instanceString(r.instancePath));
    if (out.level.lightScale != 1) for (Light& l : out.lights) l.brightness *= out.level.lightScale;  // the level's general brightness
    return out;
}


LightScene lightsOfStyle(const LightScene& all, int style) {
    LightScene out;
    out.warnings = all.warnings;
    if (style == 0) out.ambient = all.ambient;
    out.level = all.level;  // AO and the quality settings apply to every layer
    for (const Light& l : all.lights) if (l.style == style) out.lights.push_back(l);
    return out;
}

std::vector<int> lightStyles(const LightScene& scene) {
    std::set<int> s{0};
    for (const Light& l : scene.lights) s.insert(l.style);
    return std::vector<int>(s.begin(), s.end());
}

}  // namespace core
