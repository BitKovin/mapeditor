// render/picking -- see picking.h.
#include "render/picking.h"

#include "scene/render_mesh.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace native::render {

namespace {

constexpr double kPointBoxHalf = 0.25;   // the default class box: 16 Quake units on a side, centred on the origin
constexpr uint32_t kNoFace = 0xFFFFFFFFu;

double rayTriangle(const Ray& r, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 e1 = b - a, e2 = c - a;
    const Vec3 p = cross(r.dir, e2);
    const double det = dot(e1, p);
    if (std::fabs(det) < 1e-12) return -1;
    const double inv = 1.0 / det;
    const Vec3 tv = r.origin - a;
    const double u = dot(tv, p) * inv;
    if (u < 0 || u > 1) return -1;
    const Vec3 q = cross(tv, e1);
    const double v = dot(r.dir, q) * inv;
    if (v < 0 || u + v > 1) return -1;
    const double t = dot(e2, q) * inv;
    return t > 0 ? t : -1;
}

// Slab test; the entry distance (0 when the origin is inside), or -1.
double rayBox(const Ray& r, const double mn[3], const double mx[3], double tMax) {
    double t0 = 0, t1 = tMax;
    for (int i = 0; i < 3; ++i) {
        const double o = r.origin[i], d = r.dir[i];
        if (std::fabs(d) < 1e-12) {
            if (o < mn[i] || o > mx[i]) return -1;
            continue;
        }
        double a = (mn[i] - o) / d, b = (mx[i] - o) / d;
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return -1;
    }
    return t0;
}

Vec3 toV(const core::Vec3& v) { return {v.x, v.y, v.z}; }

std::string firstSegment(const std::string& instance) {
    const size_t s = instance.find('/');
    return s == std::string::npos ? instance : instance.substr(0, s);
}

} // namespace

doc::SelRef PickHit::toSelRef(bool face) const {
    if (!instance.empty()) return doc::SelRef::instance(firstSegment(instance));
    if (pointEntity || brushId.empty()) return doc::SelRef::entity(entityId);
    if (face && faceId) return doc::SelRef::face(entityId, brushId, *faceId);
    return doc::SelRef::brush(entityId, brushId);
}

Ray rayThrough(const Mat4& viewProj, double ndcX, double ndcY) {
    const Mat4 inv = mat4Invert(viewProj);
    const Vec3 a = mat4Transform(inv, {ndcX, ndcY, 0.0});
    const Vec3 b = mat4Transform(inv, {ndcX, ndcY, 1.0});
    return {a, normalize(b - a)};
}

void Picker::setSnapshot(std::shared_ptr<const scene::SceneSnapshot> snapshot) {
    snap_ = std::move(snapshot);
    items_.clear();
    order_.clear();
    nodes_.clear();
    if (!snap_) return;
    items_.reserve(snap_->objects.size() + 64);
    for (const auto& [key, p] : snap_->objects) {
        if (!p.bounds.valid()) continue;
        Item it;
        it.key = key;
        it.products = &p;
        it.mn[0] = p.bounds.mn.x; it.mn[1] = p.bounds.mn.y; it.mn[2] = p.bounds.mn.z;
        it.mx[0] = p.bounds.mx.x; it.mx[1] = p.bounds.mx.y; it.mx[2] = p.bounds.mx.z;
        it.subtractive = !p.additive;
        items_.push_back(std::move(it));
    }
    if (snap_->doc) {
        for (const auto& e : snap_->doc->entities) {
            if (!e->brushes.empty() || e->typed.classname == "worldspawn") continue;
            Item it;
            it.pointEntity = e->typed.id;
            const core::Vec3& o = e->typed.origin;
            it.mn[0] = o.x - kPointBoxHalf; it.mn[1] = o.y - kPointBoxHalf; it.mn[2] = o.z - kPointBoxHalf;
            it.mx[0] = o.x + kPointBoxHalf; it.mx[1] = o.y + kPointBoxHalf; it.mx[2] = o.z + kPointBoxHalf;
            items_.push_back(std::move(it));
        }
    }
    order_.resize(items_.size());
    for (size_t i = 0; i < order_.size(); ++i) order_[i] = static_cast<int>(i);
    nodes_.reserve(items_.size() / 2 + 2);
    if (!items_.empty()) build(0, static_cast<int>(items_.size()));
}

int Picker::build(int first, int count) {
    const int index = static_cast<int>(nodes_.size());
    nodes_.emplace_back();
    Node n;
    for (int i = 0; i < 3; ++i) {
        n.mn[i] = 1e300;
        n.mx[i] = -1e300;
    }
    for (int k = first; k < first + count; ++k) {
        const Item& it = items_[static_cast<size_t>(order_[static_cast<size_t>(k)])];
        for (int i = 0; i < 3; ++i) {
            n.mn[i] = std::min(n.mn[i], it.mn[i]);
            n.mx[i] = std::max(n.mx[i], it.mx[i]);
        }
    }
    if (count <= 4) {
        n.first = first;
        n.count = count;
        nodes_[static_cast<size_t>(index)] = n;
        return index;
    }
    int axis = 0;
    for (int i = 1; i < 3; ++i)
        if (n.mx[i] - n.mn[i] > n.mx[axis] - n.mn[axis]) axis = i;
    const int mid = first + count / 2;
    std::nth_element(order_.begin() + first, order_.begin() + mid, order_.begin() + first + count, [&](int a, int b) {
        const Item& ia = items_[static_cast<size_t>(a)];
        const Item& ib = items_[static_cast<size_t>(b)];
        return ia.mn[axis] + ia.mx[axis] < ib.mn[axis] + ib.mx[axis];
    });
    const int l = build(first, mid - first);
    const int r = build(mid, first + count - mid);
    n.left = l;
    n.right = r;
    nodes_[static_cast<size_t>(index)] = n;
    return index;
}

void Picker::hitItem(const Item& item, const Ray& ray, const PickOptions& opts, std::optional<PickHit>& best) const {
    const auto consider = [&](double t, const Vec3& normal) {
        PickHit h;
        h.t = t;
        h.point = ray.origin + ray.dir * t;
        h.normal = dot(normal, ray.dir) > 0 ? normal * -1.0 : normal;   // the normal that faces the viewer
        return h;
    };
    if (!item.pointEntity.empty()) {
        const double t = rayBox(ray, item.mn, item.mx, best ? best->t : 1e300);
        if (t < 0 || (best && t >= best->t)) return;
        PickHit h = consider(t, {0, 1, 0});
        h.entityId = item.pointEntity;
        h.pointEntity = true;
        best = h;
        return;
    }
    if (item.subtractive && !opts.includeSubtractive) return;
    const scene::ObjectProducts& p = *item.products;
    const doc::DocumentVersion* doc = snap_->doc.get();

    // the owner of the brush a surface piece lies on (a cut wall: the subtractor, which may belong to another entity)
    const auto ownerOf = [&](const core::LightSurface& s) -> std::string {
        if (doc && s.brushInstance.empty() && s.instance.empty()) {
            if (doc->findBrush(s.entityId, s.brushId) >= 0) return s.entityId;
            for (const auto& e : doc->entities)
                if (doc->findBrush(e->typed.id, s.brushId) >= 0) return e->typed.id;
        }
        return s.entityId;
    };

    if (p.surfaces && !p.surfaces->empty()) {
        for (const core::LightSurface& s : *p.surfaces) {
            if (s.verts.size() < 3) continue;
            const Vec3 a = toV(s.verts[0]);
            for (size_t k = 1; k + 1 < s.verts.size(); ++k) {
                const double t = rayTriangle(ray, a, toV(s.verts[k]), toV(s.verts[k + 1]));
                if (t < 0 || (best && t >= best->t)) continue;
                PickHit h = consider(t, toV(s.normal));
                h.instance = s.brushInstance.empty() ? s.instance : s.brushInstance;
                h.entityId = ownerOf(s);
                h.brushId = s.brushId;
                h.faceId = s.faceId;
                best = h;
            }
        }
        return;
    }
    if (p.additive && p.surfaces) return;   // fully carved away: nothing to click

    // a subtractive brush, or a brush whose pieces failed: the authored brush from the document
    const core::Brush* brush = nullptr;
    if (doc && item.key.instance.empty()) {
        const int ei = doc->findEntity(item.key.entityId);
        const int bi = doc->findBrush(item.key.entityId, item.key.brushId);
        if (ei >= 0 && bi >= 0) brush = &doc->entities[static_cast<size_t>(ei)]->brushes[static_cast<size_t>(bi)]->brush;
    }
    if (brush) {
        for (const core::BrushFace& f : brush->faces) {
            if (f.loop.size() < 3) continue;
            bool ok = true;
            for (uint32_t i : f.loop) ok = ok && i < brush->vertices.size();
            if (!ok) continue;
            const Vec3 a = toV(brush->vertices[f.loop[0]]);
            const Vec3 n = toV(brush->faceNormal(f));
            for (size_t k = 1; k + 1 < f.loop.size(); ++k) {
                const double t = rayTriangle(ray, a, toV(brush->vertices[f.loop[k]]), toV(brush->vertices[f.loop[k + 1]]));
                if (t < 0 || (best && t >= best->t)) continue;
                PickHit h = consider(t, n);
                h.entityId = item.key.entityId;
                h.brushId = item.key.brushId;
                h.faceId = f.id;
                h.subtractive = item.subtractive;
                best = h;
            }
        }
        return;
    }
    // prefab content that is not carved (or no document): the raw mesh, face unknown
    const std::shared_ptr<const scene::RenderMesh>& mesh = p.render ? p.render : p.raw;
    if (!mesh) return;
    for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
        const scene::RenderVertex& va = mesh->vertices[mesh->indices[i]];
        const scene::RenderVertex& vb = mesh->vertices[mesh->indices[i + 1]];
        const scene::RenderVertex& vc = mesh->vertices[mesh->indices[i + 2]];
        const Vec3 a{va.pos[0], va.pos[1], va.pos[2]}, b{vb.pos[0], vb.pos[1], vb.pos[2]}, c{vc.pos[0], vc.pos[1], vc.pos[2]};
        const double t = rayTriangle(ray, a, b, c);
        if (t < 0 || (best && t >= best->t)) continue;
        float n[3];
        scene::octDecode(va.normal, n);
        PickHit h = consider(t, {n[0], n[1], n[2]});
        h.instance = item.key.instance;
        h.entityId = item.key.entityId;
        h.brushId = item.key.brushId;
        h.subtractive = item.subtractive;
        best = h;
    }
    (void)kNoFace;
}

std::optional<PickHit> Picker::pick(const Ray& ray, const PickOptions& opts) const {
    std::optional<PickHit> best;
    if (nodes_.empty()) return best;
    int stack[128];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const Node& n = nodes_[static_cast<size_t>(stack[--sp])];
        const double tMax = best ? best->t : 1e300;
        if (rayBox(ray, n.mn, n.mx, tMax) < 0) continue;
        if (n.left < 0) {
            for (int k = n.first; k < n.first + n.count; ++k) hitItem(items_[static_cast<size_t>(order_[static_cast<size_t>(k)])], ray, opts, best);
            continue;
        }
        if (sp + 2 <= 128) {
            stack[sp++] = n.left;
            stack[sp++] = n.right;
        }
    }
    return best;
}

std::vector<doc::SelRef> Picker::boxSelect(const Mat4& m, double x0, double y0, double x1, double y1, const PickOptions& opts) const {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    const auto row = [&](int r) { return std::array<double, 4>{m[static_cast<size_t>(r)], m[static_cast<size_t>(4 + r)], m[static_cast<size_t>(8 + r)], m[static_cast<size_t>(12 + r)]}; };
    const auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    std::array<double, 4> planes[6];
    for (int i = 0; i < 4; ++i) {
        planes[0][static_cast<size_t>(i)] = r0[static_cast<size_t>(i)] - x0 * r3[static_cast<size_t>(i)];   // x >= x0 * w
        planes[1][static_cast<size_t>(i)] = x1 * r3[static_cast<size_t>(i)] - r0[static_cast<size_t>(i)];   // x <= x1 * w
        planes[2][static_cast<size_t>(i)] = r1[static_cast<size_t>(i)] - y0 * r3[static_cast<size_t>(i)];
        planes[3][static_cast<size_t>(i)] = y1 * r3[static_cast<size_t>(i)] - r1[static_cast<size_t>(i)];
        planes[4][static_cast<size_t>(i)] = r2[static_cast<size_t>(i)];                                       // z >= 0
        planes[5][static_cast<size_t>(i)] = r3[static_cast<size_t>(i)] - r2[static_cast<size_t>(i)];         // z <= w
    }
    std::vector<const Item*> inside;
    for (const Item& it : items_) {
        if (it.subtractive && !opts.includeSubtractive) continue;
        bool all = true;
        for (const auto& p : planes) {
            // the corner that is most outside this plane must still be inside
            const double x = p[0] >= 0 ? it.mn[0] : it.mx[0];
            const double y = p[1] >= 0 ? it.mn[1] : it.mx[1];
            const double z = p[2] >= 0 ? it.mn[2] : it.mx[2];
            if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0) {
                all = false;
                break;
            }
        }
        if (all) inside.push_back(&it);
    }
    std::sort(inside.begin(), inside.end(), [](const Item* a, const Item* b) {
        if (a->pointEntity != b->pointEntity) return a->pointEntity < b->pointEntity;
        return a->key < b->key;
    });
    std::vector<doc::SelRef> out;
    std::set<std::string> instances;
    for (const Item* it : inside) {
        if (!it->pointEntity.empty()) {
            out.push_back(doc::SelRef::entity(it->pointEntity));
        } else if (!it->key.instance.empty()) {
            const std::string id = firstSegment(it->key.instance);
            if (instances.insert(id).second) out.push_back(doc::SelRef::instance(id));
        } else {
            out.push_back(doc::SelRef::brush(it->key.entityId, it->key.brushId));
        }
    }
    return out;
}

} // namespace native::render
