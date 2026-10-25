#include "core/parallel.h"
#include "core/derived.h"
#include "core/bsp_compile.h"
#include "core/csg.h"
#include "core/decal.h"
#include "core/smoothing.h"
#include <unordered_set>
#include <cstring>
#include <map>

using nlohmann::json;

namespace core {

namespace {

struct Item {
    const Entity* entity;
    std::string instance;
};

std::vector<Item> allEntities(const Document& doc, const std::vector<ResolvedEntity>& prefabs) {
    std::vector<Item> items;
    // editor-only volumes (entity_class.h) are not part of the level: no surfaces, no carving, no solids
    for (const auto& e : doc.entities) if (!EntityClassRegistry::isEditorOnly(e.classname)) items.push_back({&e, ""});
    for (const auto& r : prefabs) if (!EntityClassRegistry::isEditorOnly(r.entity.classname)) items.push_back({&r.entity, instanceString(r.instancePath)});
    return items;
}

json vertsJson(const std::vector<Vec3>& v) {
    json a = json::array();
    for (const auto& p : v) { a.push_back(p.x); a.push_back(p.y); a.push_back(p.z); }
    return a;
}

// A surface piece of a target brush, resolved to the authored face it lies on.
struct Piece {
    std::string brush;
    std::string instance;  // the instance that owns `brush` (a cut wall belongs to its subtractor, which may live in a prefab)
    uint32_t face;
    const Brush* brushPtr = nullptr;  // the authored brush that owns `face`
    std::vector<Vec3> verts;
};

// surfaces per (instance, entity, brush): the carved result when there is one, else the authored faces
struct Target { const Item* owner; const Brush* brush; std::vector<Piece> pieces; };

// Shared by the derived_geometry message and the lightmap code: resolves every additive brush to its final surface pieces.
void buildTargets(const std::vector<Item>& items, std::vector<Target>& targets, json& carved, json& skipped);

}  // namespace

std::string instanceString(const std::vector<std::string>& path) {
    std::string out;
    for (size_t i = 1; i < path.size(); ++i) {
        if (i > 1) out += '/';
        out += path[i];
    }
    return out;
}

bool hasDerivedWork(const Document& doc, const std::vector<ResolvedEntity>& prefabs) {
    for (const auto& it : allEntities(doc, prefabs)) {
        if (isDecalEntity(*it.entity)) return true;
        for (const auto& b : it.entity->brushes) if (b.op == BrushOp::Subtract) return true;
    }
    return false;
}

namespace {
void buildTargets(const std::vector<Item>& items, std::vector<Target>& targets, json& carved, json& skipped) {
    struct Sub { const Item* owner; const Brush* brush; };
    std::vector<Sub> subs;
    for (const auto& it : items)
        for (const auto& b : it.entity->brushes)
            if (b.op == BrushOp::Subtract) subs.push_back({&it, &b});

    for (const auto& target : items) {
        for (const auto& b : target.entity->brushes) {
            if (b.op != BrushOp::Add) continue;
            Target t{&target, &b, {}};
            std::vector<const Sub*> active;
            json ids = json::array(), idInstances = json::array();
            for (const auto& s : subs) {
                // Partitions: the root document's subtractors carve anything (per scope); a prefab's subtractors carve
                // their own instance and the root document (the world around the prefab), never a sibling prefab.
                if (s.owner->instance != target.instance && !s.owner->instance.empty() && !target.instance.empty()) continue;
                const bool same = s.owner->entity->id == target.entity->id && s.owner->instance == target.instance;
                if (!carveScopeApplies(s.brush->carve, same, *target.entity) || !brushesMayOverlap(b, *s.brush)) continue;
                active.push_back(&s);
                ids.push_back(s.brush->id);
                idInstances.push_back(s.owner->instance);
            }
            bool useRaw = active.empty();
            if (!useRaw) {
                std::vector<CsgFragment> frags;
                std::vector<const Brush*> cutters;
                for (const Sub* s : active) cutters.push_back(s->brush);
                if (!csgSubtract(b, cutters, frags)) {
                    skipped.push_back(json{{"entityId", target.entity->id}, {"instance", target.instance}, {"brushId", b.id}});
                    useRaw = true;
                } else {
                    json fj = json::array();
                    for (const auto& f : frags) {
                        const Sub* cutter = f.source == 0 ? nullptr : active[static_cast<size_t>(f.source) - 1];
                        const std::string& owner = cutter ? cutter->brush->id : b.id;
                        const std::string& ownerInstance = cutter ? cutter->owner->instance : target.instance;
                        fj.push_back(json{{"brush", owner}, {"instance", ownerInstance}, {"face", f.faceId}, {"v", vertsJson(f.verts)}});
                        t.pieces.push_back({owner, ownerInstance, f.faceId, cutter ? cutter->brush : &b, f.verts});
                    }
                    carved.push_back(json{{"entityId", target.entity->id}, {"instance", target.instance}, {"brushId", b.id},
                                          {"subtractors", ids}, {"subtractorInstances", idInstances}, {"fragments", fj}});
                }
            }
            if (useRaw) for (auto& p : brushSurfaces(b)) t.pieces.push_back({b.id, target.instance, p.faceId, &b, std::move(p.verts)});
            targets.push_back(std::move(t));
        }
    }
}
}  // namespace

struct SolidSet::Cache {
    std::unordered_map<uint64_t, std::vector<Volume>> volumes;  // brush shape -> its volumes
    std::unordered_set<uint64_t> used;                          // asked for since the last sweep
    void sweep() {
        for (auto it = volumes.begin(); it != volumes.end();) it = used.count(it->first) ? std::next(it) : volumes.erase(it);
        used.clear();
    }
};

std::shared_ptr<SolidSet::Cache> SolidSet::makeCache() { return std::make_shared<Cache>(); }

namespace {
uint64_t brushShapeHash(const Brush& b, bool subtractive) {
    uint64_t h = subtractive ? 1469598103934665603ull : 7809847782465536322ull;
    auto mix = [&](const void* p, size_t n) { const unsigned char* c = static_cast<const unsigned char*>(p); for (size_t i = 0; i < n; ++i) h = (h ^ c[i]) * 1099511628211ull; };
    for (const Vec3& v : b.vertices) { const double d[3] = {v.x, v.y, v.z}; mix(d, sizeof d); }
    for (const auto& f : b.faces) { for (uint32_t i : f.loop) mix(&i, sizeof i); const uint32_t sep = 0xFFFFFFFFu; mix(&sep, sizeof sep); }
    return h;
}
}  // namespace

void SolidSet::addAll(const std::vector<std::pair<const Brush*, bool>>& list, Cache* cache) {
    std::vector<uint64_t> keys(list.size());
    std::vector<std::vector<Volume>> per(list.size());
    std::vector<char> have(list.size(), 0);
    for (size_t i = 0; i < list.size(); ++i) {
        keys[i] = brushShapeHash(*list[i].first, list[i].second);
        if (!cache) continue;
        cache->used.insert(keys[i]);
        const auto it = cache->volumes.find(keys[i]);
        if (it != cache->volumes.end()) { per[i] = it->second; have[i] = 1; }
    }
    parallelFor(list.size(), 1, [&](size_t b0, size_t e0) {
        for (size_t i = b0; i < e0; ++i) {
            if (have[i]) continue;
            SolidSet one;
            one.add(*list[i].first, list[i].second, nullptr);
            per[i] = std::move(one.volumes_);
        }
    });
    for (size_t i = 0; i < list.size(); ++i) {
        if (cache && !have[i]) cache->volumes[keys[i]] = per[i];
        volumes_.insert(volumes_.end(), per[i].begin(), per[i].end());
    }
}

void SolidSet::add(const Brush& b, bool subtractive, Cache* cache) {
    if (cache) {
        const uint64_t key = brushShapeHash(b, subtractive);
        cache->used.insert(key);
        const auto hit = cache->volumes.find(key);
        if (hit != cache->volumes.end()) { volumes_.insert(volumes_.end(), hit->second.begin(), hit->second.end()); return; }
        const size_t before = volumes_.size();
        add(b, subtractive, nullptr);
        cache->volumes[key] = std::vector<Volume>(volumes_.begin() + static_cast<std::ptrdiff_t>(before), volumes_.end());
        return;
    }
    auto grow = [](Volume& v, const Vec3& p) {
        v.mn = {std::min(v.mn.x, p.x), std::min(v.mn.y, p.y), std::min(v.mn.z, p.z)};
        v.mx = {std::max(v.mx.x, p.x), std::max(v.mx.y, p.y), std::max(v.mx.z, p.z)};
    };
    auto fresh = [&] {
        Volume v;
        v.subtractive = subtractive;
        v.mn = {1e300, 1e300, 1e300};
        v.mx = {-1e300, -1e300, -1e300};
        return v;
    };
    // A volume is the intersection of its planes, which is the brush only when it is convex. A concave brush (allowed where the class does not
    // require convex brushes, e.g. a mesh-edited hollow building) is added as its convex pieces: the intersection of all its face planes would
    // miss most of its solid (texels buried in it would be baked, probes in it counted as open air).
    bool convex = true;
    for (const auto& f : b.faces) {
        if (f.loop.size() < 3 || !convex) continue;
        const Vec3 n = b.faceNormal(f);
        if (n.length() < 0.5) continue;
        const double d = n.dot(b.vertices[f.loop[0]]);
        for (const Vec3& p : b.vertices) if (n.dot(p) - d > 1e-4) { convex = false; break; }
    }
    if (!convex) {
        const size_t before = volumes_.size();
        for (const ConvexPiece& piece : convexPieces(b)) {
            Volume v = fresh();
            for (const auto& w : convexWindings(piece.sides)) for (const Vec3& p : w) grow(v, p);
            for (const HalfSpace& h : piece.sides) v.planes.push_back({h.normal, h.dist});
            if (v.planes.size() >= 4 && v.mn.x <= v.mx.x) volumes_.push_back(std::move(v));
        }
        if (volumes_.size() > before) return;  // else (no pieces: not a closed solid) the plane set below, as before
    }
    Volume v = fresh();
    for (const auto& p : b.vertices) grow(v, p);
    for (const auto& f : b.faces) {
        if (f.loop.size() < 3) continue;
        const Vec3 n = b.faceNormal(f);
        if (n.length() < 0.5) continue;
        v.planes.push_back({n, n.dot(b.vertices[f.loop[0]])});
    }
    if (v.planes.size() >= 4) volumes_.push_back(std::move(v));
}

static int64_t cellKey(int64_t x, int64_t y, int64_t z) { return ((x + 100000) << 42) ^ ((y + 100000) << 21) ^ (z + 100000); }

void SolidSet::finish() {
    grid_.clear();
    for (size_t i = 0; i < volumes_.size(); ++i) {
        const Volume& v = volumes_[i];
        const auto c = [&](double a) { return static_cast<int64_t>(std::floor(a / kCell)); };
        for (int64_t x = c(v.mn.x); x <= c(v.mx.x); ++x)
            for (int64_t y = c(v.mn.y); y <= c(v.mx.y); ++y)
                for (int64_t z = c(v.mn.z); z <= c(v.mx.z); ++z) grid_[cellKey(x, y, z)].push_back(static_cast<int>(i));
    }
}

bool SolidSet::contains(const Volume& v, const Vec3& p, double margin) const {
    if (p.x < v.mn.x - margin || p.y < v.mn.y - margin || p.z < v.mn.z - margin || p.x > v.mx.x + margin || p.y > v.mx.y + margin || p.z > v.mx.z + margin) return false;
    for (const auto& pl : v.planes) if (pl.first.dot(p) - pl.second >= -margin) return false;  // not strictly inside by `margin`
    return true;
}

bool SolidSet::inside(const Vec3& p, Probe& probe, double margin) const {
    const int64_t cx = static_cast<int64_t>(std::floor(p.x / kCell)), cy = static_cast<int64_t>(std::floor(p.y / kCell)), cz = static_cast<int64_t>(std::floor(p.z / kCell));
    if (probe.cell[0] != cx || probe.cell[1] != cy || probe.cell[2] != cz || probe.cell[0] == INT64_MIN) {
        probe.cell[0] = cx; probe.cell[1] = cy; probe.cell[2] = cz;
        const auto it = grid_.find(cellKey(cx, cy, cz));
        probe.volumes = it == grid_.end() ? nullptr : &it->second;
    }
    if (!probe.volumes) return false;
    bool solid = false;
    for (int i : *probe.volumes) {
        const Volume& v = volumes_[static_cast<size_t>(i)];
        if (v.subtractive) {
            if (contains(v, p, -margin)) return false;
        } else if (!solid && contains(v, p, margin)) {
            solid = true;
        }
    }
    return solid;
}

bool SolidSet::inside(const Vec3& p, double margin) const {
    const auto it = grid_.find(cellKey(static_cast<int64_t>(std::floor(p.x / kCell)), static_cast<int64_t>(std::floor(p.y / kCell)), static_cast<int64_t>(std::floor(p.z / kCell))));
    if (it == grid_.end()) return false;
    bool solid = false;
    for (int i : it->second) {
        const Volume& v = volumes_[static_cast<size_t>(i)];
        if (v.subtractive) {
            if (contains(v, p, -margin)) return false;  // inside a cutter: air (a point on its surface counts as inside it)
        } else if (!solid && contains(v, p, margin)) {
            solid = true;
        }
    }
    return solid;
}

uint64_t SolidSet::fingerprint() const {
    uint64_t h = 1469598103934665603ull;
    auto mixIn = [&](double d) {
        uint64_t b;
        std::memcpy(&b, &d, sizeof b);
        h ^= b + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h *= 0xFF51AFD7ED558CCDull;
    };
    for (const Volume& v : volumes_) {
        mixIn(v.subtractive ? 1.0 : 0.0);
        for (double d : {v.mn.x, v.mn.y, v.mn.z, v.mx.x, v.mx.y, v.mx.z}) mixIn(d);
        for (const auto& pl : v.planes) for (double d : {pl.first.x, pl.first.y, pl.first.z, pl.second}) mixIn(d);
    }
    return h;
}

SolidSet collectSolids(const Document& doc, const std::vector<ResolvedEntity>& prefabs, const std::function<bool(const std::string&)>& blocks,
                       SolidSet::Cache* cache) {
    SolidSet set;
    std::vector<std::pair<const Brush*, bool>> list;
    for (const auto& it : allEntities(doc, prefabs)) {
        if (!blocks(it.entity->classname)) continue;
        for (const auto& b : it.entity->brushes) list.push_back({&b, b.op == BrushOp::Subtract});
    }
    set.addAll(list, cache);
    set.finish();
    if (cache) cache->sweep();
    return set;
}

Vec2 albedoUvAt(const LightSurface& s, const Vec3& p) {
    if (!s.hasUvMap) return projectAlbedoUv(s.normal, p, s.projection);
    const UvMap& m = s.uvMap;
    return {m.u[0] * p.x + m.u[1] * p.y + m.u[2] * p.z + m.u[3], m.v[0] * p.x + m.v[1] * p.y + m.v[2] * p.z + m.v[3]};
}

std::vector<LightSurface> collectSurfaces(const Document& doc, const std::vector<ResolvedEntity>& prefabs) {
    const std::vector<Item> items = allEntities(doc, prefabs);
    std::vector<Target> targets;
    json carved = json::array(), skipped = json::array();
    buildTargets(items, targets, carved, skipped);
    std::vector<LightSurface> out;
    std::map<const Brush*, std::shared_ptr<const BrushSmoothing>> smoothingOf;
    for (const auto& t : targets) {
        for (const auto& p : t.pieces) {
            if (p.verts.size() < 3 || !p.brushPtr) continue;
            const BrushFace* face = nullptr;
            for (const auto& f : p.brushPtr->faces) if (f.id == p.face) { face = &f; break; }
            if (!face) continue;
            Vec3 n;  // Newell normal of the piece
            for (size_t i = 0; i < p.verts.size(); ++i) {
                const Vec3 &a = p.verts[i], &b = p.verts[(i + 1) % p.verts.size()];
                n = n + Vec3{(a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)};
            }
            n = n.normalized();
            if (n.length() < 0.5) continue;
            LightSurface s;
            s.instance = t.owner->instance;
            s.entityId = t.owner->entity->id;
            s.classname = t.owner->entity->classname;
            s.brushId = p.brush;
            s.brushInstance = p.instance;
            s.faceId = p.face;
            s.side = n.dot(p.brushPtr->faceNormal(*face)) >= 0 ? FaceSide::Front : FaceSide::Back;
            s.texture = face->texture;
            s.projection = face->projection;
            s.lookHash = face->lookHash;
            if (faceUvMap(*p.brushPtr, *face, s.uvMap)) {
                // only when the piece lies on the face's plane (a cut wall of a subtractor does; a transformed copy might not)
                const Vec3 fn = p.brushPtr->faceNormal(*face), fo = p.brushPtr->vertices[face->loop[0]];
                bool onPlane = true;
                for (const Vec3& v : p.verts) if (std::fabs((v - fo).dot(fn)) > 1e-3) { onPlane = false; break; }
                s.hasUvMap = onPlane;
            }
            s.lightmapScale = face->lightmapScale;
            s.normal = n;
            s.verts = p.verts;
            if (!p.brushPtr->smoothEdges.empty()) {
                auto [sit, fresh] = smoothingOf.try_emplace(p.brushPtr, nullptr);
                if (fresh) sit->second = brushSmoothing(*p.brushPtr);
                s.smoothing = sit->second;
                if (s.smoothing && s.side == FaceSide::Front) {
                    const auto fit = s.smoothing->faces.find(face->id);
                    if (fit != s.smoothing->faces.end()) {
                        const Vec3 fn = p.brushPtr->faceNormal(*face), fo = p.brushPtr->vertices[face->loop[0]];
                        bool onPlane = true;
                        for (const Vec3& v : p.verts) if (std::fabs((v - fo).dot(fn)) > 1e-3) { onPlane = false; break; }
                        if (onPlane) s.shading = &fit->second;
                    }
                }
            }
            out.push_back(std::move(s));
        }
    }
    return out;
}

std::vector<DecalFragment> collectDecals(const Document& doc, const std::vector<ResolvedEntity>& prefabs) {
    std::vector<DecalFragment> out;
    const std::vector<Item> items = allEntities(doc, prefabs);
    std::vector<Target> targets;
    json carved = json::array(), skipped = json::array();
    buildTargets(items, targets, carved, skipped);
    for (const auto& it : items) {
        if (!isDecalEntity(*it.entity)) continue;
        const DecalProjector d = decalFromEntity(*it.entity);
        const double r = d.width + d.height + d.depth;
        const Brush reach = Brush::makeBox("", d.origin - Vec3{r, r, r}, d.origin + Vec3{r, r, r});
        for (const auto& t : targets) {
            if (!brushesMayOverlap(*t.brush, reach)) continue;
            for (const auto& piece : t.pieces) {
                const DecalPolygon poly = clipToDecal(d, SurfaceInput{piece.verts});
                if (poly.verts.size() < 3) continue;
                DecalFragment f;
                f.decalId = d.id;
                f.decalInstance = it.instance;
                f.texture = d.texture;
                f.instance = t.owner->instance;
                f.entityId = t.owner->entity->id;
                f.classname = t.owner->entity->classname;
                f.brushInstance = piece.instance;
                f.brushId = piece.brush;
                f.faceId = piece.face;
                Vec3 nn;
                for (size_t k = 0; k + 2 < poly.verts.size(); ++k) nn = nn + (poly.verts[k + 1] - poly.verts[0]).cross(poly.verts[k + 2] - poly.verts[0]);
                f.normal = nn.normalized();
                f.verts = poly.verts;
                for (const auto& c : poly.uv) f.uv.push_back({c[0], c[1]});
                out.push_back(std::move(f));
            }
        }
    }
    return out;
}

json deriveGeometry(const Document& doc, const std::vector<ResolvedEntity>& prefabs, uint64_t rev) {
    json carved = json::array(), skipped = json::array(), decals = json::array();
    const std::vector<Item> items = allEntities(doc, prefabs);
    std::vector<Target> targets;
    buildTargets(items, targets, carved, skipped);

    for (const auto& it : items) {
        if (!isDecalEntity(*it.entity)) continue;
        const DecalProjector d = decalFromEntity(*it.entity);
        json frags = json::array();
        for (const auto& t : targets) {
            if (!brushesMayOverlap(*t.brush, Brush::makeBox("", d.origin - Vec3{1, 1, 1} * (d.width + d.height + d.depth), d.origin + Vec3{1, 1, 1} * (d.width + d.height + d.depth)))) continue;
            for (const auto& piece : t.pieces) {
                const DecalPolygon poly = clipToDecal(d, SurfaceInput{piece.verts});
                if (poly.verts.size() < 3) continue;
                json uv = json::array();
                for (const auto& c : poly.uv) { uv.push_back(c[0]); uv.push_back(c[1]); }
                frags.push_back(json{{"entityId", t.owner->entity->id}, {"instance", piece.instance}, {"brush", piece.brush},
                                     {"face", piece.face}, {"v", vertsJson(poly.verts)}, {"uv", uv}});
            }
        }
        decals.push_back(json{{"decalId", d.id}, {"instance", it.instance}, {"texture", d.texture}, {"fragments", frags}});
    }
    return json{{"type", "derived_geometry"}, {"rev", rev}, {"carved", carved}, {"skipped", skipped}, {"decals", decals}};
}

}  // namespace core
