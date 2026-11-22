// light/entity_probes -- see entity_probes.h.
#include "light/entity_probes.h"

#include <core/lighting_classes.h>
#include <core/lights.h>
#include <core/raytrace.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

#include <core/bsp_compile.h>

#include "doc/document.h"

namespace native::light {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kBlock = 6;   // probes a sample point gets per axis: the 2 x 2 x 2 of its cell and the ring around it (the fallback to the nearest valid probe)

EntityLightValue valueOf(const core::Probe& p) {
    EntityLightValue v;
    if (!p.valid) return v;
    v.valid = true;
    v.ambient[0] = static_cast<float>(p.ambient.x);
    v.ambient[1] = static_cast<float>(p.ambient.y);
    v.ambient[2] = static_cast<float>(p.ambient.z);
    v.directed[0] = static_cast<float>(p.directed.x);
    v.directed[1] = static_cast<float>(p.directed.y);
    v.directed[2] = static_cast<float>(p.directed.z);
    v.dir[0] = static_cast<float>(p.dir.x);
    v.dir[1] = static_cast<float>(p.dir.y);
    v.dir[2] = static_cast<float>(p.dir.z);
    return v;
}

} // namespace

void entityLightAt(const EntityLightValue& v, const float n[3], float out[3]) {
    const float d = std::max(0.0f, n[0] * v.dir[0] + n[1] * v.dir[1] + n[2] * v.dir[2]);
    for (int c = 0; c < 3; ++c) out[c] = v.ambient[c] + v.directed[c] * d;
}

EntityProbes bakeEntityProbes(const EntityProbeInputs& in, core::SkyTableCache* skyCache, core::SolidSet::Cache* solidCache) {
    const auto t0 = Clock::now();
    EntityProbes out;
    if (!in.snapshot || !in.snapshot->doc || !in.profile) {
        out.error = "no scene to light";
        return out;
    }
    const scene::SceneSnapshot& snap = *in.snapshot;
    const core::LightingClasses classes = core::LightingClasses::fromProfile(*in.profile);

    // ---- the inputs the live bake uses: light entities of the base style layer, light blocking solids, the surfaces without sky
    core::Document coreDoc = doc::toCoreDocument(*snap.doc);
    const std::vector<core::ResolvedEntity> resolved = snap.prefabs ? snap.prefabs->flat() : std::vector<core::ResolvedEntity>();
    const core::LightingSource source{coreDoc, resolved, *in.profile, in.assetsRoot};
    core::SkyTableCache localSky;
    core::LightScene all = core::lightSceneOf(source, skyCache ? *skyCache : localSky);
    if (in.filter) in.filter->apply(all.lights);
    core::LightScene lights = core::lightsOfStyle(all, 0);
    lights.playable = all.playable;
    for (core::Light& l : lights.lights) {
        if (l.sourceSize > 0) continue;
        if ((l.type == core::LightType::Point || l.type == core::LightType::Spot) && in.defaultSourceSize > 0) l.sourceSize = in.defaultSourceSize;
        else if (l.type == core::LightType::Directional && in.defaultSunSize > 0) l.sourceSize = in.defaultSunSize;
    }
    std::shared_ptr<core::SolidSet::Cache> localSolids;
    if (!solidCache) {
        localSolids = core::SolidSet::makeCache();
        solidCache = localSolids.get();
    }
    const std::shared_ptr<core::SolidSet> solids = core::lightSolidsOf(source, solidCache);
    std::vector<core::LightSurface> surfaces;
    {
        std::vector<const scene::ObjectKey*> keys;
        for (const auto& [k, p] : snap.objects)
            if (p.surfaces && !p.surfaces->empty()) keys.push_back(&k);
        std::sort(keys.begin(), keys.end(), [](const scene::ObjectKey* a, const scene::ObjectKey* b) { return *a < *b; });
        const core::BspContentRules rules = core::BspContentRules::fromJson(in.profile->bsp);
        for (const scene::ObjectKey* k : keys)
            for (const core::LightSurface& s : *snap.objects.at(*k).surfaces)
                if (!rules.isSky(s.texture)) surfaces.push_back(s);
    }
    if (surfaces.empty()) {
        out.error = "the scene has no surfaces";
        return out;
    }

    // ---- the lattice of the level's light volume (core::buildLightvolGrid without allocating its probes: a level can have tens of millions)
    core::Aabb bounds;
    for (const core::LightSurface& s : surfaces)
        for (const core::Vec3& v : s.verts) bounds.add(v);
    core::LightvolSettings cfg;
    if (all.level.lightvolGrid > 0) cfg.gridSize = all.level.lightvolGrid;
    cfg.bounces = false;   // the bounce source is the Core's lightmap atlas; the editor has none (06_LIGHTING.md section 11)
    cfg.useGpu = false;
    cfg.classes = classes;
    if (all.level.raysPerProbe > 0) cfg.giSamples = all.level.raysPerProbe;
    const double cell = std::max(cfg.gridSize, 0.05);
    const core::Vec3 origin = bounds.mn - core::Vec3{cell, cell, cell};

    // ---- the sample points: the middle of every brush entity that has no lightmaps, then the models
    struct Point {
        core::Vec3 p;
        core::Probe probe;
    };
    std::map<std::pair<std::string, std::string>, core::Aabb> entityBounds;
    for (const core::LightSurface& s : surfaces) {
        if (classes.isLightmapped(s.classname)) continue;
        core::Aabb& b = entityBounds[{s.instance, s.entityId}];
        for (const core::Vec3& v : s.verts) b.add(v);
    }
    std::vector<Point> points;
    for (const auto& [key, b] : entityBounds) {
        BrushEntityLight e;
        e.key = scene::ObjectKey{key.first, key.second, ""};
        const core::Vec3 c = b.center();
        e.centre[0] = c.x;
        e.centre[1] = c.y;
        e.centre[2] = c.z;
        out.brushEntities.push_back(e);
        points.push_back({c, {}});
    }
    for (const auto& m : in.modelCentres) points.push_back({core::Vec3{m[0], m[1], m[2]}, {}});
    if (points.empty()) {
        out.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
        return out;
    }

    // ---- bake: what the shadow rays need (the occluders of the classes that block light and a BVH over them), then a block of probes around every point
    const core::OccluderMesh mesh = core::buildOccluders(surfaces, classes);
    const core::TriangleBvh bvh(mesh);
    core::LightvolSource src;
    src.lights = &lights;
    src.mesh = &mesh;
    src.bvh = &bvh;
    for (Point& pt : points) {
        const auto cellOf = [&](double v, double o) { return static_cast<int>(std::floor((v - o) / cell)); };
        const int ix = cellOf(pt.p.x, origin.x) - (kBlock / 2 - 1), iy = cellOf(pt.p.y, origin.y) - (kBlock / 2 - 1), iz = cellOf(pt.p.z, origin.z) - (kBlock / 2 - 1);
        core::LightvolGrid g;
        g.cell = cell;
        g.nx = g.ny = g.nz = kBlock;
        g.origin = origin + core::Vec3{ix * cell, iy * cell, iz * cell};
        g.probes.assign(static_cast<size_t>(kBlock) * kBlock * kBlock, core::Probe{});
        const std::vector<char> solid = core::solidProbes(g, solids.get());
        std::vector<char> baked;
        core::bakeProbesCpu(g, solid, baked, 0, g.count(), src, cfg);
        out.probes += g.count();
        pt.probe = g.sample(pt.p);
    }
    for (size_t i = 0; i < out.brushEntities.size(); ++i) out.brushEntities[i].light = valueOf(points[i].probe);
    out.models.reserve(in.modelCentres.size());
    for (size_t i = 0; i < in.modelCentres.size(); ++i) out.models.push_back(valueOf(points[out.brushEntities.size() + i].probe));
    out.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    return out;
}

} // namespace native::light
