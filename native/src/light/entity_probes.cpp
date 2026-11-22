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
#include "light/entity_bounce.h"

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
    cfg.bounces = false;   // the Core's atlas gather is not used: entity_bounce gathers from surfels instead (06_LIGHTING.md section 11)
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

    // ---- bake: what the shadow rays need (the occluders of the classes that block light and a BVH over them), then the probes around every point
    core::LightmapLayout noLayout;   // an empty layout: the occluders then carry the albedo data of the bounce rays, not lightmap coordinates
    core::OccluderMesh mesh = core::buildOccluders(surfaces, classes, in.bounces ? &noLayout : nullptr);
    std::shared_ptr<core::AlbedoLibrary> library = in.albedo;
    if (in.bounces) {
        if (!library) library = std::make_shared<core::AlbedoLibrary>(in.assetsRoot, *in.profile);
        core::attachAlbedo(mesh, [library](const std::string& name) { return library->get(name); });
    }
    const core::TriangleBvh bvh(mesh);
    core::BakeOptions direct;
    direct.shadows = !mesh.empty();
    direct.areaSamples = cfg.areaSamples;
    direct.skySamples = cfg.skySamples;
    direct.superSamples = 1;
    direct.bounces = 0;
    direct.classes = classes;
    const std::vector<core::Vec3> normals = core::sphereNormals(cfg.normals);
    const size_t K = normals.size();
    std::unique_ptr<EntityBounce> bounce;
    if (in.bounces && !mesh.empty()) {
        BounceSettings bs;
        bs.raysPerSample = cfg.giSamples;
        bounce = std::make_unique<EntityBounce>(mesh, bvh, lights, direct, bs);
    }

    // a block of probes per point: the lattice around it (grids[i]), the probes still to bake first and the rest
    struct Block {
        core::LightvolGrid grid;
        std::vector<char> solid;
        std::vector<size_t> corners, rest;
    };
    std::vector<Block> blocks(points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        const core::Vec3& p = points[i].p;
        const auto cellOf = [&](double v, double o) { return static_cast<int>(std::floor((v - o) / cell)); };
        const int ix = cellOf(p.x, origin.x) - (kBlock / 2 - 1), iy = cellOf(p.y, origin.y) - (kBlock / 2 - 1), iz = cellOf(p.z, origin.z) - (kBlock / 2 - 1);
        Block& b = blocks[i];
        b.grid.cell = cell;
        b.grid.nx = b.grid.ny = b.grid.nz = kBlock;
        b.grid.origin = origin + core::Vec3{ix * cell, iy * cell, iz * cell};
        b.grid.probes.assign(static_cast<size_t>(kBlock) * kBlock * kBlock, core::Probe{});
        b.solid = core::solidProbes(b.grid, solids.get());
        for (int z = 0; z < kBlock; ++z)
            for (int y = 0; y < kBlock; ++y)
                for (int x = 0; x < kBlock; ++x) {
                    const size_t idx = b.grid.index(x, y, z);
                    if (b.solid[idx]) continue;
                    const bool corner = x >= kBlock / 2 - 1 && x <= kBlock / 2 && y >= kBlock / 2 - 1 && y <= kBlock / 2 && z >= kBlock / 2 - 1 && z <= kBlock / 2;
                    (corner ? b.corners : b.rest).push_back(idx);
                }
    }
    // Bakes the listed probes of every block: V(n_k) = direct + gathered bounce light, fitted by the Core's own fit (fitProbeDirectFirst).
    const auto bakeList = [&](std::vector<size_t> Block::*list) {
        core::TexelSamples s;
        std::vector<std::pair<size_t, size_t>> who;   // block, probe
        for (size_t bi = 0; bi < blocks.size(); ++bi)
            for (size_t idx : blocks[bi].*list) {
                who.emplace_back(bi, idx);
                const core::Vec3 pp = blocks[bi].grid.position(idx);
                for (const core::Vec3& n : normals) {
                    s.pos.insert(s.pos.end(), {static_cast<float>(pp.x), static_cast<float>(pp.y), static_cast<float>(pp.z), 1.f});
                    s.nrm.insert(s.nrm.end(), {static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z), 0.f});
                }
            }
        if (who.empty()) return;
        s.texel.assign(who.size() * K, 0u);
        const std::vector<float> d = core::evaluateDirectCpu(s, lights, direct.shadows ? &bvh : nullptr, direct);
        std::vector<float> ind;
        if (bounce) ind = bounce->gather(s);
        std::vector<core::Vec3> v(K), dv(K);
        for (size_t j = 0; j < who.size(); ++j) {
            for (size_t k = 0; k < K; ++k) {
                const size_t t = (j * K + k) * 4;
                dv[k] = v[k] = {d[t], d[t + 1], d[t + 2]};
                if (!ind.empty()) v[k] = v[k] + core::Vec3{ind[t], ind[t + 1], ind[t + 2]};
                v[k] = core::clampLight(v[k]);
                dv[k] = core::clampLight(dv[k]);
            }
            blocks[who[j].first].grid.probes[who[j].second] = core::fitProbeDirectFirst(core::probeMoments(normals, v, &dv), cfg.bounceDirectionWeight);
        }
        out.probes += who.size();
    };
    bakeList(&Block::corners);
    // a point whose own cell has no valid corner (a door inside a wall) falls back to the probes around it: bake the rest of its block
    for (size_t i = 0; i < blocks.size(); ++i) {
        points[i].probe = blocks[i].grid.sample(points[i].p);
        if (points[i].probe.valid) blocks[i].rest.clear();
    }
    bakeList(&Block::rest);
    for (size_t i = 0; i < blocks.size(); ++i)
        if (!points[i].probe.valid) points[i].probe = blocks[i].grid.sample(points[i].p);
    if (bounce) {
        out.surfels = bounce->surfels();
        out.rays = bounce->rays();
        out.castSeconds = bounce->castSeconds();
        out.directSeconds = bounce->directSeconds();
    }
    for (size_t i = 0; i < out.brushEntities.size(); ++i) out.brushEntities[i].light = valueOf(points[i].probe);
    out.models.reserve(in.modelCentres.size());
    for (size_t i = 0; i < in.modelCentres.size(); ++i) out.models.push_back(valueOf(points[out.brushEntities.size() + i].probe));
    out.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    return out;
}

} // namespace native::light
