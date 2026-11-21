// tools/parametric -- see parametric.h.
#include "tools/parametric.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "tools/texturing/surface.h"

namespace native::tools::parametric {

namespace {

constexpr ShapeId kKinds[] = {ShapeId::Cylinder, ShapeId::Wedge, ShapeId::Arch, ShapeId::Stairs, ShapeId::Sphere};

bool isVec(const Json& j) {
    if (!j.is_array() || j.size() != 3) return false;
    for (const Json& n : j)
        if (!n.is_number() || !std::isfinite(n.get<double>())) return false;
    return true;
}
Vec3 toVec(const Json& j) { return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()}; }
Json fromVec(const Vec3& v) { return Json::array({v.x, v.y, v.z}); }
bool isNum(const Json& j) { return j.is_number() && std::isfinite(j.get<double>()); }

Json frameJson(const Frame& f) {
    return Json{{"o", fromVec(f.o)}, {"u", fromVec(f.u)}, {"v", fromVec(f.v)}, {"n", fromVec(f.n)}, {"du", f.du}, {"dv", f.dv}, {"h", f.h}};
}

bool close(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }
bool closeVec(const Vec3& a, const Vec3& b) { return close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z); }
bool sameFrame(const Frame& a, const Frame& b) {
    return closeVec(a.o, b.o) && closeVec(a.u, b.u) && closeVec(a.v, b.v) && closeVec(a.n, b.n) && close(a.du, b.du) && close(a.dv, b.dv) && close(a.h, b.h);
}
bool sameGeometry(const core::Brush& a, const core::Brush& b) {
    if (a.vertices.size() != b.vertices.size()) return false;
    for (size_t i = 0; i < a.vertices.size(); ++i)
        if (!(close(a.vertices[i].x, b.vertices[i].x) && close(a.vertices[i].y, b.vertices[i].y) && close(a.vertices[i].z, b.vertices[i].z))) return false;
    return true;
}

// The old brush's look on the rebuilt one: a face with the same id keeps its texture and projection (stored UVs dropped, so the projection applies); a new face copies face 0.
core::Brush restyle(const core::Brush& fresh, const core::Brush* old, const core::texplan::InfoFn& info) {
    core::Brush out = fresh;
    if (old && !old->faces.empty()) {
        for (core::BrushFace& f : out.faces) {
            const core::BrushFace* was = &old->faces[0];
            for (const core::BrushFace& o : old->faces)
                if (o.id == f.id) {
                    was = &o;
                    break;
                }
            core::BrushFace n = *was;
            n.id = f.id;
            n.loop = f.loop;
            n.uvs.clear();
            f = std::move(n);
        }
    }
    return info ? tex::alignBrush(out, info) : out;
}

} // namespace

bool isParametric(ShapeId id) { return id != ShapeId::Box; }

std::optional<ShapeId> kindFromName(const std::string& name) {
    for (ShapeId k : kKinds)
        if (name == shapeName(k)) return k;
    return std::nullopt;
}

Json paramsFor(ShapeId kind, const ShapeParams& p) {
    switch (kind) {
        case ShapeId::Cylinder: return Json{{"sides", p.sides}, {"smooth", p.smooth}};
        case ShapeId::Sphere: return Json{{"segments", p.segments}, {"rings", p.rings}, {"smooth", p.smooth}};
        case ShapeId::Stairs: return Json{{"steps", p.steps}};
        case ShapeId::Arch: return Json{{"archSegments", p.archSegments}, {"archThickness", p.archThickness}};
        default: return Json::object();
    }
}

ShapeParams paramsFrom(const Json& stored, Turn turn) {
    ShapeParams p;
    p.turn = turn;
    if (!stored.is_object()) return p;
    const auto num = [&](const char* key, auto& out) {
        const auto it = stored.find(key);
        if (it != stored.end() && it->is_number()) out = static_cast<std::decay_t<decltype(out)>>(it->get<double>());
    };
    num("sides", p.sides);
    num("steps", p.steps);
    num("archSegments", p.archSegments);
    num("archThickness", p.archThickness);
    num("segments", p.segments);
    num("rings", p.rings);
    if (const auto it = stored.find("smooth"); it != stored.end() && it->is_boolean()) p.smooth = it->get<bool>();
    return p;
}

Json shapeRecord(const ShapeSpec& s) {
    return Json{{"group", s.group}, {"kind", shapeName(s.kind)}, {"part", s.part}, {"parts", s.parts}, {"frame", frameJson(s.frame)}, {"params", s.params}, {"turn", s.turn}};
}

std::optional<ShapeSpec> shapeOf(const Json& rawBrush) {
    if (!rawBrush.is_object()) return std::nullopt;
    const auto it = rawBrush.find("shape");
    if (it == rawBrush.end() || !it->is_object()) return std::nullopt;
    const Json& s = *it;
    ShapeSpec out;
    if (!s.contains("group") || !s["group"].is_string()) return std::nullopt;
    out.group = s["group"].get<std::string>();
    if (!s.contains("kind") || !s["kind"].is_string()) return std::nullopt;
    const auto kind = kindFromName(s["kind"].get<std::string>());
    if (!kind) return std::nullopt;
    out.kind = *kind;
    if (!s.contains("part") || !s["part"].is_number_integer() || !s.contains("parts") || !s["parts"].is_number_integer()) return std::nullopt;
    out.part = s["part"].get<int>();
    out.parts = s["parts"].get<int>();
    if (out.part < 0 || out.part >= out.parts) return std::nullopt;
    if (!s.contains("frame") || !s["frame"].is_object()) return std::nullopt;
    const Json& f = s["frame"];
    for (const char* k : {"o", "u", "v", "n"})
        if (!f.contains(k) || !isVec(f[k])) return std::nullopt;
    for (const char* k : {"du", "dv", "h"})
        if (!f.contains(k) || !isNum(f[k])) return std::nullopt;
    out.frame.o = toVec(f["o"]);
    out.frame.u = toVec(f["u"]);
    out.frame.v = toVec(f["v"]);
    out.frame.n = toVec(f["n"]);
    out.frame.du = f["du"].get<double>();
    out.frame.dv = f["dv"].get<double>();
    out.frame.h = f["h"].get<double>();
    out.turn = 0;
    if (s.contains("turn") && s["turn"].is_number_integer()) {
        const int t = s["turn"].get<int>();
        if (t >= 0 && t <= 3) out.turn = t;
    }
    out.params = s.contains("params") && s["params"].is_object() ? s["params"] : Json::object();
    return out;
}

Frame mapFrame(const Frame& f, const std::function<Vec3(const Vec3&)>& map) {
    Frame g;
    g.o = map(f.o);
    const auto axis = [&](const Vec3& dir, double len, Vec3& outDir, double& outLen) {
        const Vec3 d = map(f.o + dir * len) - g.o;
        const double l = render::length(d);
        if (l > 1e-12) {
            outDir = d * (1 / l);
            outLen = l;
        } else {
            outDir = dir;
            outLen = len;
        }
    };
    axis(f.u, f.du, g.u, g.du);
    axis(f.v, f.dv, g.v, g.dv);
    axis(f.n, f.h, g.n, g.h);
    return g;
}

Json mapShapeRecord(const Json& rawShape, const std::function<Vec3(const Vec3&)>& map) {
    Json holder{{"shape", rawShape}};
    const auto s = shapeOf(holder);
    if (!s) return rawShape;
    ShapeSpec m = *s;
    m.frame = mapFrame(s->frame, map);
    return shapeRecord(m);
}

ShapeOutcome partsOf(ShapeId kind, const Frame& frame, const Json& params, Turn turn) {
    int i = 0;
    return buildInFrame(kind, turned(frame, turn), "", paramsFrom(params, turn), [&i] { return "p" + std::to_string(i++); });
}

bool addShape(doc::VersionEditor& ed, const std::string& entityId, const std::vector<core::Brush>& brushes, ShapeId kind, const Frame& frame, const ShapeParams& params, Turn turn,
              const std::string& group) {
    const Json stored = paramsFor(kind, params);
    for (size_t i = 0; i < brushes.size(); ++i) {
        ShapeSpec s;
        s.group = group;
        s.kind = kind;
        s.part = static_cast<int>(i);
        s.parts = static_cast<int>(brushes.size());
        s.frame = frame;
        s.params = stored;
        s.turn = turn;
        if (!ed.addBrush(entityId, brushes[i]) || !ed.setBrushKey(entityId, brushes[i].id, "shape", shapeRecord(s))) return false;
    }
    return true;
}

Regenerated regenerateShape(const std::shared_ptr<const doc::DocumentVersion>& v, const std::string& entityId, const std::string& group, const ShapePatch& patch,
                            const std::function<std::string()>& newId, const core::texplan::InfoFn& info) {
    Regenerated r;
    const int ei = v ? v->findEntity(entityId) : -1;
    struct Old {
        const doc::BrushNode* node;
        ShapeSpec spec;
    };
    std::vector<Old> old;
    if (ei >= 0)
        for (const auto& b : v->entities[static_cast<size_t>(ei)]->brushes)
            if (const auto s = shapeOf(*b); s && s->group == group) old.push_back({b.get(), *s});
    if (old.empty()) {
        r.error = "That shape no longer exists";
        return r;
    }
    std::sort(old.begin(), old.end(), [](const Old& a, const Old& b) { return a.spec.part < b.spec.part; });
    const ShapeSpec& s = old[0].spec;
    Frame frame = s.frame;
    if (patch.du) frame.du = *patch.du;
    if (patch.dv) frame.dv = *patch.dv;
    if (patch.h) frame.h = *patch.h;
    if (!(frame.du > 1e-4 && frame.dv > 1e-4 && frame.h > 1e-4)) {
        r.error = "A shape needs a size above zero on every side";
        return r;
    }
    Json params = s.params;
    if (patch.params.is_object())
        for (auto it = patch.params.begin(); it != patch.params.end(); ++it) params[it.key()] = it.value();
    const Turn turn = patch.turn.value_or(s.turn);
    const ShapeOutcome built = partsOf(s.kind, frame, params, turn);
    if (!built.ok()) {
        r.error = built.error;
        return r;
    }
    // the stored parameters are the ones of the kind (a patch key that does not belong is dropped)
    const Json kept = paramsFor(s.kind, paramsFrom(params, turn));
    doc::VersionEditor ed(v);
    const size_t n = built.brushes.size();
    for (size_t i = 0; i < n; ++i) {
        const Old* was = i < old.size() ? &old[i] : nullptr;
        core::Brush part = built.brushes[i];
        part.id = was ? was->node->brush.id : newId();
        if (was) {
            part.op = was->node->brush.op;
            part.carve = was->node->brush.carve;
        }
        part = restyle(part, &(was ? was->node : old[0].node)->brush, info);
        ShapeSpec rec;
        rec.group = group;
        rec.kind = s.kind;
        rec.part = static_cast<int>(i);
        rec.parts = static_cast<int>(n);
        rec.frame = frame;
        rec.params = kept;
        rec.turn = turn;
        if (was) {
            if (!ed.replaceBrush(entityId, part) || !ed.setBrushKey(entityId, part.id, "shape", shapeRecord(rec))) break;
        } else {
            if (!ed.addBrush(entityId, part) || !ed.setBrushKey(entityId, part.id, "shape", shapeRecord(rec))) break;
        }
    }
    for (size_t i = n; i < old.size() && ed.ok(); ++i) ed.removeBrush(entityId, old[i].node->brush.id);
    if (!ed.ok()) {
        r.error = ed.error();
        return r;
    }
    r.version = ed.finish();
    if (!r.version) r.error = ed.error().empty() ? "The shape could not be rebuilt" : ed.error();
    return r;
}

std::shared_ptr<const doc::DocumentVersion> reconcileShapes(const std::shared_ptr<const doc::DocumentVersion>& prev, const std::shared_ptr<const doc::DocumentVersion>& next,
                                                            const std::function<std::string()>& freshGroup) {
    if (!next) return next;
    const auto carries = [](const doc::BrushNode& b) { return b.raw.is_object() && b.raw.contains("shape"); };
    bool any = false;   // the common case: no shape anywhere, nothing to do (this runs on every commit, also every step of a drag)
    for (const auto& e : next->entities) {
        for (const auto& b : e->brushes)
            if (carries(*b)) {
                any = true;
                break;
            }
        if (any) break;
    }
    if (!any) return next;
    std::map<std::string, std::set<std::string>> prevMembers;   // group -> "entity|brush"
    std::map<std::string, const doc::BrushNode*> prevBrush;
    if (prev)
        for (const auto& e : prev->entities)
            for (const auto& b : e->brushes) {
                if (!carries(*b)) continue;
                const std::string key = e->typed.id + "|" + b->brush.id;
                prevBrush[key] = b.get();
                if (const auto s = shapeOf(*b)) prevMembers[s->group].insert(key);
            }
    struct Change {
        std::string entityId, brushId;
        std::optional<std::string> regroup;   // nullopt = drop the record
    };
    std::vector<Change> changes;
    for (const auto& e : next->entities) {
        const std::string& eid = e->typed.id;
        std::map<std::string, std::vector<const doc::BrushNode*>> groups;
        std::set<std::string> strip;
        std::map<std::string, std::string> regroup;
        bool has = false;
        for (const auto& b : e->brushes) {
            if (!carries(*b)) continue;
            has = true;
            const auto s = shapeOf(*b);
            if (!s) {
                strip.insert(b->brush.id);
                continue;
            }
            groups[s->group].push_back(b.get());
        }
        if (!has) continue;
        const auto valid = [&](const std::vector<const doc::BrushNode*>& list) {
            if (list.empty()) return false;
            std::vector<ShapeSpec> specs;
            for (const doc::BrushNode* b : list) specs.push_back(*shapeOf(*b));
            const ShapeSpec& s0 = specs[0];
            std::set<int> seen;
            for (const ShapeSpec& s : specs) {
                if (s.kind != s0.kind || s.parts != s0.parts) return false;
                seen.insert(s.part);
            }
            if (static_cast<int>(list.size()) != s0.parts || seen.size() != list.size()) return false;
            // one box, one set of parameters, one orientation: a part moved or edited on its own breaks the shape
            for (const ShapeSpec& s : specs)
                if (s.turn != s0.turn || !sameFrame(s.frame, s0.frame) || s.params != s0.params) return false;
            std::optional<ShapeOutcome> expected;
            for (size_t i = 0; i < list.size(); ++i) {
                const auto it = prevBrush.find(eid + "|" + list[i]->brush.id);
                if (it != prevBrush.end() && it->second == list[i]) continue;   // untouched by this edit
                if (!expected) {
                    expected = partsOf(specs[i].kind, specs[i].frame, specs[i].params, specs[i].turn);
                    if (!expected->ok()) return false;
                }
                const size_t part = static_cast<size_t>(specs[i].part);
                if (part >= expected->brushes.size() || !sameGeometry(expected->brushes[part], list[i]->brush)) return false;
            }
            return true;
        };
        for (const auto& [g, list] : groups) {
            const auto was = prevMembers.find(g);
            std::vector<const doc::BrushNode*> own, copies;
            for (const doc::BrushNode* b : list) {
                if (was != prevMembers.end() && !was->second.count(eid + "|" + b->brush.id)) copies.push_back(b);
                else own.push_back(b);
            }
            // a group that is wholly new here (drawn, pasted, moved in from another entity) counts as its own
            const auto& mine = own.empty() ? copies : own;
            const std::vector<const doc::BrushNode*> extra = own.empty() ? std::vector<const doc::BrushNode*>{} : copies;
            if (!valid(mine))
                for (const doc::BrushNode* b : mine) strip.insert(b->brush.id);
            if (!extra.empty()) {
                if (valid(extra)) {
                    const std::string ng = freshGroup();
                    for (const doc::BrushNode* b : extra) regroup[b->brush.id] = ng;
                } else {
                    for (const doc::BrushNode* b : extra) strip.insert(b->brush.id);
                }
            }
        }
        for (const std::string& b : strip) changes.push_back({eid, b, std::nullopt});
        for (const auto& [b, g] : regroup) changes.push_back({eid, b, g});
    }
    if (changes.empty()) return next;
    doc::VersionEditor ed(next);
    for (const Change& c : changes) {
        if (!c.regroup) {
            ed.setBrushKey(c.entityId, c.brushId, "shape", Json());
            continue;
        }
        const int ei = next->findEntity(c.entityId);
        const int bi = ei >= 0 ? next->findBrush(c.entityId, c.brushId) : -1;
        if (bi < 0) continue;
        auto spec = shapeOf(*next->entities[static_cast<size_t>(ei)]->brushes[static_cast<size_t>(bi)]);
        if (!spec) continue;
        spec->group = *c.regroup;
        ed.setBrushKey(c.entityId, c.brushId, "shape", shapeRecord(*spec));
    }
    const auto out = ed.finish();
    return out ? out : next;
}

std::shared_ptr<const doc::DocumentVersion> dissolveShape(const std::shared_ptr<const doc::DocumentVersion>& v, const std::string& entityId, const std::string& group) {
    const int ei = v ? v->findEntity(entityId) : -1;
    if (ei < 0) return v;
    doc::VersionEditor ed(v);
    for (const auto& b : v->entities[static_cast<size_t>(ei)]->brushes)
        if (const auto s = shapeOf(*b); s && s->group == group) ed.setBrushKey(entityId, b->brush.id, "shape", Json());
    const auto out = ed.finish();
    return out ? out : v;
}

std::vector<std::string> shapeMembers(const doc::DocumentVersion& v, const std::string& entityId, const std::string& brushId) {
    const int ei = v.findEntity(entityId);
    if (ei < 0) return {brushId};
    const auto& brushes = v.entities[static_cast<size_t>(ei)]->brushes;
    std::string group;
    for (const auto& b : brushes)
        if (b->brush.id == brushId) {
            if (const auto s = shapeOf(*b)) group = s->group;
            break;
        }
    if (group.empty()) return {brushId};
    std::vector<std::pair<int, std::string>> parts;
    for (const auto& b : brushes)
        if (const auto s = shapeOf(*b); s && s->group == group) parts.emplace_back(s->part, b->brush.id);
    std::sort(parts.begin(), parts.end());
    std::vector<std::string> out;
    for (const auto& p : parts) out.push_back(p.second);
    return out;
}

} // namespace native::tools::parametric
