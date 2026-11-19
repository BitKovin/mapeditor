// scene/arrows -- see arrows.h.
#include "scene/arrows.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace native::scene {

namespace {

V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 scale(const V3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double length(const V3& a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
V3 normalize(const V3& a) {
    const double l = length(a);
    return l < 1e-12 ? V3{0, 0, 0} : scale(a, 1 / l);
}

V3 centreOf(const Box& b) { return {(b.min[0] + b.max[0]) / 2, (b.min[1] + b.max[1]) / 2, (b.min[2] + b.max[2]) / 2}; }

struct Span {
    double tin, tout;
};
// Distances along the ray o + t * dir where it enters and leaves the box.
std::optional<Span> rayBoxSpan(const V3& o, const V3& dir, const Box& b) {
    double tin = -1e300, tout = 1e300;
    for (size_t i = 0; i < 3; ++i) {
        if (std::fabs(dir[i]) < 1e-12) {
            if (o[i] < b.min[i] || o[i] > b.max[i]) return std::nullopt;
            continue;
        }
        const double a = (b.min[i] - o[i]) / dir[i], c = (b.max[i] - o[i]) / dir[i];
        tin = std::max(tin, std::min(a, c));
        tout = std::min(tout, std::max(a, c));
    }
    if (tin <= tout) return Span{tin, tout};
    return std::nullopt;
}

void push(std::vector<double>& out, const V3& a, const V3& b) {
    out.insert(out.end(), {a[0], a[1], a[2], b[0], b[1], b[2]});
}

std::string pathOf(const std::vector<std::string>& ids, const std::string& last) {
    std::string s;
    for (const std::string& i : ids) s += i + "/";
    return s + last;
}

std::string colorKey(const Rgb& c) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%.3f,%.3f,%.3f", c[0], c[1], c[2]);
    return buf;
}

Rgb brighten(const Rgb& c) { return {c[0] + (1 - c[0]) * 0.6, c[1] + (1 - c[1]) * 0.6, c[2] + (1 - c[2]) * 0.6}; }

} // namespace

const Rgb kLinkColor = {0.72, 0.5, 0.95};
const Rgb kLinkColorSelected = {1, 0.95, 0.35};

std::vector<Placed> placedEntities(const std::vector<const core::Entity*>& docEntities, const std::vector<core::ResolvedEntity>& prefab) {
    std::vector<Placed> out;
    for (const core::Entity* e : docEntities) out.push_back({e, {}, e->id, {}});
    for (const core::ResolvedEntity& r : prefab) {
        std::vector<std::string> ids;
        if (r.instancePath.size() > 1) ids.assign(r.instancePath.begin() + 1, r.instancePath.end());
        out.push_back({&r.entity, std::move(ids), r.sourceEntityId, r.refScopes});
    }
    return out;
}

std::string pathOfPlaced(const Placed& p) { return pathOf(p.ids, p.sourceId); }

std::vector<double> arrowLines(const V3& from, const V3& tip, double headLen) {
    const V3 d = sub(tip, from);
    const double len = length(d);
    if (len < 1e-9) return {};
    const V3 dir = scale(d, 1 / len);
    const V3 u = normalize(std::fabs(dir[1]) < 0.9 ? cross(dir, {0, 1, 0}) : cross(dir, {1, 0, 0}));
    const V3 v = cross(dir, u);
    const V3 back = sub(tip, scale(dir, headLen));
    const double w = headLen * 0.4;
    std::vector<double> out;
    push(out, from, tip);
    for (const V3& s : {u, v, scale(u, -1), scale(v, -1)}) push(out, tip, add(back, scale(s, w)));
    return out;
}

Box boundsOf(const core::Entity& e, const BoxOf& boxOf) {
    if (e.isBrushEntity()) {
        Box b{{1e300, 1e300, 1e300}, {-1e300, -1e300, -1e300}};
        for (const core::Brush& br : e.brushes)
            for (const core::Vec3& v : br.vertices) {
                const V3 p{v.x, v.y, v.z};
                for (size_t i = 0; i < 3; ++i) {
                    b.min[i] = std::min(b.min[i], p[i]);
                    b.max[i] = std::max(b.max[i], p[i]);
                }
            }
        if (b.min[0] <= b.max[0]) return b;
    }
    return boundsOfBox(pointBox(e, boxOf));
}

DirectionSpan directionSpan(const core::Entity& e, const BoxOf& boxOf) {
    const OrientedBox ob = pointBox(e, boxOf);
    const Box local{ob.min, ob.max};
    const V3 lc = centreOf(local);
    const V3 c = toWorld(ob, lc);
    // forward = R * (0, 0, 1) is +Z in the box's own frame, so the span through the box is measured there
    const auto span = rayBoxSpan(lc, {0, 0, 1}, local);
    const V3 dir = apply3(ob.rot, {0, 0, 1});
    const double size = std::max({ob.max[0] - ob.min[0], ob.max[1] - ob.min[1], ob.max[2] - ob.min[2]});
    return {c, dir, std::max((span ? span->tout : 0) + 0.6 * size, 0.3)};
}

std::vector<double> directionArrow(const core::Entity& e, const BoxOf& boxOf) {
    const DirectionSpan s = directionSpan(e, boxOf);
    return arrowLines(s.from, add(s.from, scale(s.dir, s.len)), std::max(s.len * 0.25, 0.08));
}

std::vector<double> linkArrow(const Box& from, const Box& to) {
    const V3 a = centreOf(from), b = centreOf(to);
    const V3 d = sub(b, a);
    const double dist = length(d);
    if (dist < 1e-6) return {};
    const V3 dir = scale(d, 1 / dist);
    const auto leave = rayBoxSpan(a, dir, from);
    const auto enter = rayBoxSpan(a, dir, to);
    double t0 = leave ? std::max(leave->tout, 0.0) : 0, t1 = enter ? enter->tin : dist;
    if (!(t1 > t0 + 1e-6)) {   // overlapping or touching boxes: centre to centre
        t0 = 0;
        t1 = dist;
    }
    const double len = t1 - t0;
    return arrowLines(add(a, scale(dir, t0)), add(a, scale(dir, t1)), std::min(0.4, std::max(0.1, len * 0.2)));
}

std::vector<Link> resolveLinks(const std::vector<Placed>& items, const RefKeys& refKeys, const ResolvedName& resolvedName) {
    std::map<std::string, size_t> byPath;
    for (size_t i = 0; i < items.size(); ++i) byPath[pathOfPlaced(items[i])] = i;
    const auto nameOf = [&](const Placed& p) -> std::string {
        if (resolvedName) return resolvedName(p);
        const auto it = p.entity->keyvalues.find("targetname");
        return it == p.entity->keyvalues.end() ? std::string() : it->second;
    };
    std::map<std::string, std::vector<size_t>> byName;
    bool haveNames = false;
    const auto names = [&]() -> const std::map<std::string, std::vector<size_t>>& {
        if (!haveNames) {
            haveNames = true;
            for (size_t i = 0; i < items.size(); ++i) {
                const std::string n = nameOf(items[i]);
                if (!n.empty()) byName[n].push_back(i);
            }
        }
        return byName;
    };
    std::vector<Link> out;
    for (size_t fi = 0; fi < items.size(); ++fi) {
        const Placed& from = items[fi];
        const core::Entity& e = *from.entity;
        for (const std::string& key : refKeys(e.classname)) {
            const auto rit = e.refs.find(key);
            if (rit != e.refs.end() && !rit->second.paths.empty()) {
                const auto sit = from.refScopes.find(key);
                const size_t scope = std::min(sit != from.refScopes.end() ? sit->second : from.ids.size(), from.ids.size());
                for (const std::string& ref : rit->second.paths) {
                    std::string full;
                    for (size_t i = 0; i < scope; ++i) full += from.ids[i] + "/";
                    full += ref;
                    const auto hit = byPath.find(full);
                    if (hit != byPath.end() && hit->second != fi) out.push_back({fi, hit->second, key});
                }
            } else {
                const auto kv = e.keyvalues.find(key);
                if (kv == e.keyvalues.end() || kv->second.empty()) continue;
                const std::string& name = kv->second;
                // a plain name reaches EVERY entity with that name: those of the same scope first (prefab-local names), else the ones with that global resolved name
                std::vector<size_t> sameScope;
                for (size_t qi = 0; qi < items.size(); ++qi) {
                    const Placed& q = items[qi];
                    if (q.ids != from.ids) continue;
                    const auto t = q.entity->keyvalues.find("targetname");
                    if (t != q.entity->keyvalues.end() && t->second == name) sameScope.push_back(qi);
                }
                std::vector<size_t> targets = sameScope;
                if (targets.empty()) {
                    const auto& idx = names();
                    const auto it = idx.find(name);
                    if (it != idx.end()) targets = it->second;
                }
                for (size_t to : targets)
                    if (to != fi) out.push_back({fi, to, key});
            }
        }
    }
    return out;
}

std::vector<Overlay> entityArrows(const std::vector<Placed>& items, const ArrowOptions& o) {
    std::vector<Overlay> groups;
    std::map<std::string, size_t> index;
    const auto addLines = [&](const Rgb& color, const std::vector<double>& lines) {
        if (lines.empty()) return;
        const std::string k = colorKey(color);
        auto it = index.find(k);
        if (it == index.end()) {
            it = index.emplace(k, groups.size()).first;
            groups.push_back({color, {}});
        }
        groups[it->second].lines.insert(groups[it->second].lines.end(), lines.begin(), lines.end());
    };
    static const Rgb kDefaultColor = {1, 0.85, 0.25};
    const auto selected = [&](const Placed& p) { return o.isSelected && o.isSelected(p); };
    if (o.showDirections) {
        for (const Placed& p : items) {
            const core::Entity& e = *p.entity;
            if (e.isBrushEntity() || e.classname == "worldspawn" || e.classname == "decal") continue;
            const std::optional<Rgb> c = o.colorOf ? o.colorOf(e.classname) : std::nullopt;
            const Rgb base = c ? *c : kDefaultColor;
            addLines(selected(p) ? brighten(base) : base, directionArrow(e, o.boxOf));
        }
    }
    for (const Link& l : resolveLinks(items, o.refKeys, o.resolvedName)) {
        const bool sel = selected(items[l.from]) || selected(items[l.to]);
        if (!sel && !o.showAllLinks) continue;
        addLines(sel ? kLinkColorSelected : kLinkColor, linkArrow(boundsOf(*items[l.from].entity, o.boxOf), boundsOf(*items[l.to].entity, o.boxOf)));
    }
    return groups;
}

} // namespace native::scene
