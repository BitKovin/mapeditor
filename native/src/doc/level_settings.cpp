// doc/level_settings -- see level_settings.h.
#include "doc/level_settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "doc/edit.h"

namespace native::doc {

namespace {

const core::Entity* findWorld(const DocumentVersion& v) {
    for (const auto& e : v.entities)
        if (e->typed.classname == "worldspawn") return &e->typed;
    return nullptr;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// A whole string as a finite number; nullopt otherwise.
std::optional<double> parseNumber(const std::string& text) {
    const std::string t = trim(text);
    if (t.empty()) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end != t.c_str() + t.size() || !std::isfinite(v)) return std::nullopt;
    return v;
}

const std::string* keyOf(const core::Entity* world, const std::string& key) {
    if (!world) return nullptr;
    const auto it = world->keyvalues.find(key);
    return it == world->keyvalues.end() ? nullptr : &it->second;
}

} // namespace

const std::vector<LevelSection>& levelSections() {
    static const std::vector<LevelSection> sections = {
    {"Light", "", {
        {"_light_scale", "General brightness", 1, 0, 100, 0.1, false, "Multiplies the brightness of every light entity (not the ambient light)"},
    }},
    {"Colour correction", "Display only: how the Lit view (and the engine) shows the baked light. Changing it re-bakes nothing.", {
        {"_cc_exposure", "Exposure", std::nullopt, 0.05, 20, 0.05, false, "Multiplies the shown light; empty = the Core's lighting.json exposure"},
        {"_cc_brightness", "Brightness", 0, -1, 1, 0.01, false, "Added to the shown light after the exposure (linear)"},
    }},
    {"Ambient occlusion (baked)", "Darkens light in corners and creases. Radius 0 = off. Darker: raise Strength or Exponent, or lower Distance falloff.", {
        {"_ao_radius", "Radius (m)", 0, 0, 64, 0.1, false, "How far rays look for nearby geometry"},
        {"_ao_power", "Exponent", 1, 0.1, 10, 0.1, false, "Curve of the darkening: higher = darker, wider shadows; below 1 = softer"},
        {"_ao_intensity", "Intensity", 1, 0, 1, 0.05, false, "0 = no darkening, 1 = full"},
        {"_ao_strength", "Strength", 1, 0, 16, 0.1, false, "Multiplies the occlusion before the exponent: 2 or more turns tight corners fully dark"},
        {"_ao_falloff", "Distance falloff", 1, 0, 8, 0.1, false, "How a hit fades with its distance: 0 = every hit within the radius counts fully (strongest), 1 = linear, higher = only very near geometry"},
        {"_ao_rays", "Rays", 32, 1, 1024, 8, true, "Occlusion rays per texel (ray traced on the GPU when it supports ray queries): more = smoother"},
    }},
    {"Quality falloff (outside the playable area)", "Needs an editor_playable_area volume (Place tab). Lightmap detail drops in cascades with the distance from it: full density up to Radius, then each cascade reaches Range times further and has Density times the texels per metre of the one before. The light volume only covers the area plus the margin.", {
        {"_lod_radius", "Full detail radius (m)", 16, 1, 1024, 1, false, "Surfaces this close to the playable area keep the full lightmap density"},
        {"_lod_levels", "Cascades", 4, 0, 8, 1, true, "Number of lower-detail cascades beyond the full-detail one; 0 = no falloff. Surfaces further out stay in the last cascade"},
        {"_lod_range", "Range factor", 2, 1.1, 8, 0.1, false, "Each cascade reaches this many times further than the previous one (cascade k ends at Radius x Range^k)"},
        {"_lod_density", "Density per cascade", 0.5, 0.05, 1, 0.05, false, "Lightmap texels per metre of a cascade relative to the one before (0.5 = half, so a quarter in the second)"},
        {"_lod_cull_unseen", "Cull unseen surfaces", 1, 0, 1, 1, true, "1 = surfaces that nothing in the playable area can see (inside far closed buildings, backs of far walls) get the lowest cascade straight away"},
        {"_relief_distance", "Texture relief distance (m)", 48, 0, 100000, 1, false, "Export only: surfaces further than this from the playable area are written flat, without their texture relief (hundreds of thousands of triangles on a big level); 0 = relief everywhere. Needs a playable area"},
        {"_lightvol_margin", "Light volume margin (m)", 2, 0, 256, 0.5, false, "The light volume covers the playable area grown by this much"},
    }},
    {"Export", "Only the export uses these.", {
        {"_lightmap_jpg", "Lightmaps as .jpg when they fit", 1, 0, 1, 1, true, "1 (default) = pages whose light is nothing above 1 and survives 8 bits are written as lm_NNNN.jpg, grouped apart from the .exr pages (the engine has to find .jpg pages)"},
        {"_lightmap_jpg_tolerance", ".jpg error allowed (levels)", 8, 0.5, 64, 0.5, false, "How far a chart may differ in a .jpg, in 8-bit display levels after the exposure; a chart that differs more stays in the .exr pages. Near black one byte of linear light is already 12 levels: lower this and the dark charts stay .exr"},
        {"_lightmap_scale", "Light scale (HDR + dark light in 8 bits)", 1, 0, 1, 1, true, "1 (default) = every chart is stored divided by its brightest light and the vertex colour carries that (rgb = mantissa, alpha = exponent, x1..x32): bright light fits .jpg pages and dark charts use all 8 bits. 0 = pages hold the light as baked, vertex colour white. Needs the engine shader that decodes the vertex alpha"},
        {"_vertex_lit_area", "Vertex lit below (m2)", 0, 0, 1000, 0.1, false, "Surfaces (a smooth brush counts as one) with a total area up to this keep no lightmap texels: their light is only in the vertex colour. 0 (default) = off; an entity with _vertex_lit 1 is always vertex lit. Only light style 0 is kept"},
        {"_mesh_optimize", "Reduce relief vertices", 1, 0, 1, 1, true, "1 (default) = surfaces made of many small triangles (texture relief, texture group layouts) are written with far fewer vertices: duplicate points are welded, flat parts collapse to their outline, and the seams to other surfaces, texture seams and light changes are kept. 0 = write every vertex"},
        {"_mesh_tolerance", "Vertex reduction error (mm)", 1, 0, 100, 0.5, false, "How far the reduction may move a surface: detail smaller than this is thinned out of dense reliefs. 0 = only lossless merges (flat parts and straight edges). Raise it to shrink the .bsp of noisy or high-resolution reliefs; 1 mm is invisible"},
        {"_lightmap_jpg_quality", ".jpg quality", 98, 50, 100, 1, true, "JPEG quality of the lightmap pages"},
    }},
    {"Visibility (export)", "Baked visibility (PVS) of the exported map. Cells exist only inside the playable zones; everything else is always visible. Needs an editor_playable_area volume for the best result.", {
        {"_vis_enabled", "Bake visibility", 1, 0, 1, 1, true, "0 = every cluster sees every cluster (nothing is culled)"},
        {"_vis_cell_x", "Cell width X (m)", 5, 0.25, 1000, 0.25, false, "Size of a visibility cell along X; it grows on its own until the visibility table fits the size limit"},
        {"_vis_cell_y", "Cell height Y (m)", 2, 0.25, 1000, 0.25, false, "Size of a visibility cell along the vertical axis"},
        {"_vis_cell_z", "Cell depth Z (m)", 5, 0.25, 1000, 0.25, false, "Size of a visibility cell along Z"},
        {"_vis_max_mb", "Table size limit (MB)", 50, 1, 512, 1, false, "The visibility lump may be this big (clusters squared / 8); the cells grow to fit"},
        {"_vis_split_min", "Splitting wall size (m)", 2, 0.5, 64, 0.5, false, "A wall splits the cells it passes through when its (merged) face has no side shorter than this"},
        {"_vis_splits_per_cell", "Splits per cell", 16, 0, 32, 1, true, "The most walls that cut one cell"},
        {"_vis_samples", "Sample points per cluster", 32, 2, 256, 2, true, "Points the segments between clusters start and end at"},
        {"_vis_segments", "Segments per cluster pair", 512, 1, 1024, 1, true, "More segments find narrower views but take longer"},
        {"_vis_aperture", "Narrow opening width (m)", 0.5, 0, 5, 0.05, false, "Rays are aimed through gaps up to this wide (cracks, slits); 0 = off"},
        {"_vis_dilate", "Neighbour propagation", 0, 0, 1, 1, true, "1 = a cluster also sees what the clusters it opens into see (one ring, never through a wall). Safer at cell borders, but shows more"},
    }},
    {"Optimisation", "Empty = the Core's lighting.json value.", {
        {"_lightvol_grid", "Lightvol grid size (m)", std::nullopt, 0.25, 64, 0.25, false, "Spacing of the light volume probes"},
        {"_rays_per_texel", "Rays per lightmap pixel", std::nullopt, 1, 65536, 16, true, "Bounce-light gather rays per texel over the whole bake"},
        {"_rays_per_probe", "Rays per lightvol probe", std::nullopt, 1, 1024, 8, true, "Gather rays per light volume probe"},
        {"_supersample", "Supersampling per lightmap pixel", std::nullopt, 1, 8, 1, true, "Sub-texel positions for the direct light (anti-aliased shadow edges)"},
        {"_bake_quality", "Bake quality multiplier", 2, 0.25, 16, 0.25, false, "Multiplies the ray counts of the final bake (export); the live view bakes at x1"},
    }},
    };
    return sections;
}

const LevelField* levelField(const std::string& key) {
    for (const LevelSection& s : levelSections())
        for (const LevelField& f : s.fields)
            if (f.key == key) return &f;
    return nullptr;
}

const core::Entity* worldspawnOf(const DocumentVersion& version) { return findWorld(version); }

std::optional<double> levelValue(const core::Entity* world, const LevelField& field) {
    const std::string* raw = keyOf(world, field.key);
    return raw ? parseNumber(*raw) : std::nullopt;
}

std::string levelNumber(double value) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f", value);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s == "-0" ? "0" : s;
}

std::optional<std::string> levelKeyFor(const LevelField& field, const std::string& input) {
    const std::optional<double> parsed = parseNumber(input);
    if (!parsed) return std::nullopt;
    double v = *parsed;
    if (field.isInt) v = std::round(v);
    v = std::min(field.max, std::max(field.min, v));
    if (field.dflt && std::fabs(v - *field.dflt) < 1e-12) return std::nullopt;
    return levelNumber(v);
}

Ambient ambientOf(const core::Entity* world) {
    Ambient out;
    const std::string* raw = keyOf(world, "_ambient");
    if (!raw) return out;
    std::istringstream in(*raw);
    std::array<double, 3> p{};
    for (double& x : p) {
        std::string t;
        if (!(in >> t)) return out;
        const auto v = parseNumber(t);
        if (!v || *v < 0) return out;
        x = *v;
    }
    std::string extra;
    if (in >> extra) return out;
    const double k = std::max({p[0], p[1], p[2]});
    if (k <= 0) return out;
    out.color = {p[0] / k, p[1] / k, p[2] / k};
    out.intensity = k;
    return out;
}

std::optional<std::string> ambientKey(const std::array<double, 3>& color, double intensity) {
    if (!(intensity > 0)) return std::nullopt;
    std::string s;
    for (size_t i = 0; i < 3; ++i) s += (i ? " " : "") + levelNumber(std::max(0.0, color[i]) * intensity);
    return s;
}

std::string hexOfColor(const std::array<double, 3>& color) {
    char buf[8];
    const auto byte = [](double x) { return static_cast<int>(std::lround(std::min(1.0, std::max(0.0, x)) * 255)); };
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", byte(color[0]), byte(color[1]), byte(color[2]));
    return buf;
}

std::array<double, 3> colorOfHex(const std::string& hex) {
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    if (h.size() != 6 || !std::all_of(h.begin(), h.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) return {1, 1, 1};
    const auto part = [&](size_t i) { return std::stoi(h.substr(i * 2, 2), nullptr, 16) / 255.0; };
    return {part(0), part(1), part(2)};
}

const std::vector<AutoSkyOption>& autoSkyOptions() {
    static const std::vector<AutoSkyOption> options = {
        {AutoSky::Off, "", "Off", "No automatic sky"},
        {AutoSky::Box, "box", "Box", "One sky box around the whole level plus the margin (the floor is sky too)"},
        {AutoSky::Areas, "areas", "Box per area", "One sky box per group of touching geometry, plus the margin; boxes that meet are merged"},
    };
    return options;
}

AutoSky autoSkyOf(const core::Entity* world) {
    const std::string* v = keyOf(world, kSkyAutoKey);
    if (!v) return AutoSky::Off;
    return *v == "box" ? AutoSky::Box : *v == "areas" ? AutoSky::Areas : AutoSky::Off;
}

double skyMarginOf(const core::Entity* world) {
    const std::string* raw = keyOf(world, kSkyMarginKey);
    const auto v = raw ? parseNumber(*raw) : std::nullopt;
    return v ? std::min(kSkyMarginMax, std::max(0.0, *v)) : kSkyMarginDefault;
}

std::optional<std::string> skyMarginKey(const std::string& input) {
    const auto v = parseNumber(input);
    if (!v) return std::nullopt;
    const double c = std::min(kSkyMarginMax, std::max(0.0, *v));
    if (c == kSkyMarginDefault) return std::nullopt;
    return levelNumber(c);
}

bool isCubemapPath(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const std::string last = path.substr(slash == std::string::npos ? 0 : slash + 1);
    return last.size() >= 4 && last.compare(last.size() - 4, 4, "cube") == 0;
}

bool isEnvSkybox(const std::string& path) { return path.rfind("env/", 0) == 0 && isCubemapPath(path); }

std::shared_ptr<const DocumentVersion> setLevelKeys(const std::shared_ptr<const DocumentVersion>& base, const std::vector<LevelChange>& changes) {
    if (!base) return nullptr;
    const core::Entity* world = findWorld(*base);
    const bool sets = std::any_of(changes.begin(), changes.end(), [](const LevelChange& c) { return c.second.has_value(); });
    core::Entity next;
    if (world) {
        next = *world;
    } else if (sets) {
        next.id = "e_worldspawn";
        next.classname = "worldspawn";
    } else {
        return nullptr;
    }
    bool changed = !world;
    for (const LevelChange& c : changes) {
        const auto it = next.keyvalues.find(c.first);
        if (!c.second) {
            if (it != next.keyvalues.end()) {
                next.keyvalues.erase(it);
                changed = true;
            }
        } else if (it == next.keyvalues.end() || it->second != *c.second) {
            next.keyvalues[c.first] = *c.second;
            changed = true;
        }
    }
    if (!changed) return nullptr;
    VersionEditor ed(base);
    if (!(world ? ed.replaceEntity(next) : ed.addEntity(next))) return nullptr;
    return ed.finish();
}

} // namespace native::doc
