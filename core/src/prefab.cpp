#include "core/prefab.h"
#include "core/entity_class.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <sstream>
#include <unordered_map>

namespace core {

namespace {

constexpr double kDeg = 3.14159265358979323846 / 180.0;

// ---- small affine algebra (3x3 + translation), double precision ---------------------------------
struct Mat3 {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Vec3 apply(const Vec3& v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
    Mat3 operator*(const Mat3& o) const {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j];
        return r;
    }
    double det() const {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    }
};

// Rotation applied to a vector: Ry(yaw) * Rx(pitch) * Rz(roll)  (same as decal.cpp)
Mat3 rotationFromAngles(const Vec3& deg) {
    const double p = deg.x * kDeg, y = deg.y * kDeg, r = deg.z * kDeg;
    const double cp = std::cos(p), sp = std::sin(p), cy = std::cos(y), sy = std::sin(y), cr = std::cos(r), sr = std::sin(r);
    Mat3 R;
    R.m[0][0] = cy * cr + sy * sp * sr; R.m[0][1] = -cy * sr + sy * sp * cr; R.m[0][2] = sy * cp;
    R.m[1][0] = cp * sr;                R.m[1][1] = cp * cr;                 R.m[1][2] = -sp;
    R.m[2][0] = -sy * cr + cy * sp * sr; R.m[2][1] = sy * sr + cy * sp * cr; R.m[2][2] = cy * cp;
    return R;
}

Vec3 anglesFromRotation(const Mat3& R) {
    const double sp = std::clamp(-R.m[1][2], -1.0, 1.0);
    const double pitch = std::asin(sp);
    double yaw, roll;
    if (std::abs(sp) < 1.0 - 1e-9) {
        yaw = std::atan2(R.m[0][2], R.m[2][2]);
        roll = std::atan2(R.m[1][0], R.m[1][1]);
    } else {  // gimbal lock: only yaw -/+ roll is defined; put it all in yaw
        roll = 0;
        yaw = sp > 0 ? std::atan2(R.m[0][1], R.m[0][0]) : std::atan2(-R.m[0][1], R.m[0][0]);
    }
    return {pitch / kDeg, yaw / kDeg, roll / kDeg};
}

struct Xf {
    Mat3 lin;   // rotation * scale (accumulated)
    Mat3 rot;   // rotation only (accumulated)
    Vec3 scale{1, 1, 1};  // accumulated scale (component-wise product)
    Vec3 t{0, 0, 0};
    Vec3 apply(const Vec3& p) const { return lin.apply(p) + t; }
};

Xf compose(const Xf& parent, const PrefabInstance& inst) {
    const Mat3 R = rotationFromAngles(inst.angles);
    const Vec3 s = inst.scale.value_or(Vec3{1, 1, 1});
    Mat3 S;
    S.m[0][0] = s.x; S.m[1][1] = s.y; S.m[2][2] = s.z;
    Xf out;
    out.lin = parent.lin * (R * S);
    out.rot = parent.rot * R;
    out.scale = {parent.scale.x * s.x, parent.scale.y * s.y, parent.scale.z * s.z};
    out.t = parent.apply(inst.origin);
    return out;
}

std::string fmt(double v) {
    std::ostringstream os;
    os.precision(9);
    os << v;
    return os.str();
}
std::string fmtVec(const Vec3& v) { return fmt(v.x) + " " + fmt(v.y) + " " + fmt(v.z); }

bool parseVec(const std::string& s, Vec3& out) {
    std::istringstream is(s);
    return static_cast<bool>(is >> out.x >> out.y >> out.z);
}

// ---- the walker ----------------------------------------------------------------------------------
struct RefSlot {
    std::string key;
    std::vector<std::string> paths;  // one target, or several that are meant to share one name
    size_t scopeLen;  // number of leading instance ids of the entity's chain the path is relative to
};

struct Node {
    Entity entity;
    std::vector<std::string> chainIds;    // instance ids from the root (without "root")
    std::vector<std::string> chainNames;  // effective instance names, same length
    Guid sourceId;
    std::vector<RefSlot> refs;
};

struct Walker {
    const DocumentLoader& loader;
    int maxDepth;
    std::vector<std::string> visited;
    std::vector<Node> out;

    void emit(const Entity& src, const Xf& xf, const std::vector<const PrefabInstance*>& chain,
              const std::vector<std::string>& chainNames) {
        // overrides: inner -> outer so the outer instance wins. Path = ids below that instance + entity id.
        Entity e = src;
        std::vector<RefSlot> refs;
        for (const auto& [k, v] : src.refs) refs.push_back({k, v.paths, chain.size()});
        for (size_t j = chain.size(); j-- > 0;) {
            std::string path;
            for (size_t i = j + 1; i < chain.size(); ++i) path += chain[i]->id + "/";
            path += src.id;
            const auto it = chain[j]->overrides.find(path);
            if (it == chain[j]->overrides.end()) continue;
            const EntityOverride& ov = it->second;
            if (ov.deleted) return;
            for (const auto& [k, v] : ov.keyvalues) e.keyvalues[k] = v;
            for (const auto& [k, v] : ov.refs) {
                refs.erase(std::remove_if(refs.begin(), refs.end(), [&](const RefSlot& r) { return r.key == k; }), refs.end());
                refs.push_back({k, v.paths, j});  // written in instance j's override -> relative to j's parent scope
            }
            if (ov.origin) e.origin = *ov.origin;
            if (ov.angles) e.angles = *ov.angles;
            if (ov.scale) e.keyvalues["modelscale_vec"] = fmtVec(*ov.scale);
        }
        // transform into root space
        const Mat3 localRot = rotationFromAngles(e.angles);
        e.origin = xf.apply(e.origin);
        e.angles = anglesFromRotation(xf.rot * rotationFromAngles(e.angles));
        const bool mirrored = xf.lin.det() < 0;
        for (auto& b : e.brushes) {
            for (auto& v : b.vertices) v = xf.apply(v);
            if (mirrored)
                for (auto& f : b.faces) {
                    std::reverse(f.loop.begin(), f.loop.end());
                    std::reverse(f.uvs.begin(), f.uvs.end());
                }
        }
        const bool identityScale = std::abs(xf.scale.x - 1) < 1e-12 && std::abs(xf.scale.y - 1) < 1e-12 && std::abs(xf.scale.z - 1) < 1e-12;
        if (!identityScale && e.classname == "decal" && e.brushes.empty()) {
            // a decal's size lives in width / height / depth along its own axes (x / z / y before `angles`);
            // the instance scale stretches each axis by the length of the transformed axis vector
            const struct { const char* key; Vec3 axis; double dflt; } dims[3] = {{"width", {1, 0, 0}, 1.0}, {"height", {0, 0, 1}, 1.0}, {"depth", {0, 1, 0}, 0.5}};
            for (const auto& d : dims) {
                const Vec3 v = xf.lin.apply(localRot.apply(d.axis));
                double cur = d.dflt;
                const auto it = e.keyvalues.find(d.key);
                if (it != e.keyvalues.end()) { char* end = nullptr; const double x = std::strtod(it->second.c_str(), &end); if (end != it->second.c_str() && x > 1e-6) cur = x; }
                e.keyvalues[d.key] = fmt(cur * std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
            }
        } else if (!identityScale && e.brushes.empty()) {
            Vec3 cur{1, 1, 1};
            const auto it = e.keyvalues.find("modelscale_vec");
            if (it != e.keyvalues.end()) parseVec(it->second, cur);
            e.keyvalues["modelscale_vec"] = fmtVec({cur.x * xf.scale.x, cur.y * xf.scale.y, cur.z * xf.scale.z});
        }
        Node n;
        n.entity = std::move(e);
        for (const auto* p : chain) n.chainIds.push_back(p->id);
        n.chainNames = chainNames;
        n.sourceId = src.id;
        n.refs = std::move(refs);
        out.push_back(std::move(n));
    }

    void walk(const Document& doc, const Xf& xf, std::vector<const PrefabInstance*>& chain,
              std::vector<std::string>& chainNames) {
        if (static_cast<int>(chain.size()) + 1 > maxDepth) {
            std::string s;
            for (auto* p : chain) s += p->id + "/";
            throw PrefabResolveError("prefab inclusion exceeds max depth (" + std::to_string(maxDepth) + ") at instance path root/" + s);
        }
        for (const auto& e : doc.entities) emit(e, xf, chain, chainNames);

        const std::vector<std::string> names = effectiveInstanceNames(doc);
        for (size_t i = 0; i < doc.prefabInstances.size(); ++i) {
            const PrefabInstance& inst = doc.prefabInstances[i];
            const std::string key = normalizeAssetPath(inst.file);
            if (std::find(visited.begin(), visited.end(), key) != visited.end()) {
                std::string c;
                for (auto& p : visited) c += p + " -> ";
                throw PrefabResolveError("prefab cycle detected: " + c + key);
            }
            const Document& child = loader(key);
            visited.push_back(key);
            chain.push_back(&inst);
            chainNames.push_back(names[i]);
            walk(child, compose(xf, inst), chain, chainNames);
            chainNames.pop_back();
            chain.pop_back();
            visited.pop_back();
        }
    }
};

std::vector<Node> walkAll(const Document& root, const DocumentLoader& loader, int maxDepth, const std::string& rootPath) {
    Walker w{loader, maxDepth, {}, {}};
    if (!rootPath.empty()) w.visited.push_back(normalizeAssetPath(rootPath));
    std::vector<const PrefabInstance*> chain;
    std::vector<std::string> names;
    w.walk(root, Xf{}, chain, names);
    return std::move(w.out);
}

std::vector<std::string> pathWithRoot(const std::vector<std::string>& ids) {
    std::vector<std::string> p{"root"};
    p.insert(p.end(), ids.begin(), ids.end());
    return p;
}

std::string join(const std::vector<std::string>& v, const char* sep, size_t count) {
    std::string s;
    for (size_t i = 0; i < count && i < v.size(); ++i) s += (i ? sep : "") + v[i];
    return s;
}

}  // namespace

std::string normalizeAssetPath(const std::string& path) {
    return std::filesystem::path(path).lexically_normal().generic_string();
}

std::vector<ResolvedEntity> resolvePrefabs(const Document& root, const DocumentLoader& loader,
                                            const ResolveOptions& opts, const std::string& rootPath) {
    std::vector<ResolvedEntity> out;
    for (auto& n : walkAll(root, loader, opts.maxDepth, rootPath))
        out.push_back(ResolvedEntity{std::move(n.entity), pathWithRoot(n.chainIds), n.sourceId});
    return out;
}

FlattenResult flattenForCompile(const Document& root, const DocumentLoader& loader, const FlattenOptions& opts,
                                const std::string& rootPath) {
    std::vector<Node> nodes = walkAll(root, loader, opts.maxDepth, rootPath);
    FlattenResult res;

    // index: full id path ("inst/inst/entity") -> node
    std::unordered_map<std::string, size_t> index;
    for (size_t i = 0; i < nodes.size(); ++i) {
        std::string full = join(nodes[i].chainIds, "/", nodes[i].chainIds.size());
        full += (full.empty() ? "" : "/") + nodes[i].sourceId;
        index[full] = i;
    }

    std::vector<std::string> resolved(nodes.size());
    auto nameOf = [&](size_t i) -> const std::string& {
        Node& n = nodes[i];
        if (resolved[i].empty()) {
            const auto it = n.entity.keyvalues.find(opts.nameKey);
            const std::string own = it != n.entity.keyvalues.end() ? it->second : "";
            if (!own.empty()) {
                resolved[i] = n.chainNames.empty() ? own : join(n.chainNames, "|", n.chainNames.size()) + "|" + own;
            }
        }
        return resolved[i];
    };

    // 1) every entity's own resolved name
    for (size_t i = 0; i < nodes.size(); ++i) nameOf(i);

    // 2) refs -> resolved names of their targets
    for (size_t i = 0; i < nodes.size(); ++i) {
        Node& n = nodes[i];
        std::sort(n.refs.begin(), n.refs.end(), [](const RefSlot& a, const RefSlot& b) { return a.key < b.key; });
        for (const auto& r : n.refs) {
            const std::string who = "entity '" + (n.chainIds.empty() ? "" : join(n.chainIds, "/", n.chainIds.size()) + "/") + n.sourceId + "'";
            std::vector<size_t> targets;
            for (const std::string& path : r.paths) {
                std::string full = join(n.chainIds, "/", r.scopeLen);
                full += (full.empty() ? "" : "/") + path;
                const auto hit = index.find(full);
                if (hit == index.end()) res.problems.push_back(who + ": ref '" + r.key + "' -> '" + path + "' does not resolve (target missing or deleted)");
                else if (std::find(targets.begin(), targets.end(), hit->second) == targets.end()) targets.push_back(hit->second);
            }
            if (targets.empty()) {
                n.entity.keyvalues.erase(r.key);
                continue;
            }
            // The key holds ONE name, so several targets must share it (a name may belong to any number of entities; every one of them is triggered).
            // The first named target gives the name; an unnamed target joins it; a target with another name cannot be reached through this key.
            size_t named = targets.size();
            for (size_t k = 0; k < targets.size() && named == targets.size(); ++k) if (!nameOf(targets[k]).empty()) named = k;
            if (named == targets.size()) {
                // referenced but unnamed: give it a stable generated name
                const size_t t = targets[0];
                std::string prefix = nodes[t].chainNames.empty() ? "" : join(nodes[t].chainNames, "|", nodes[t].chainNames.size()) + "|";
                resolved[t] = prefix + nodes[t].sourceId;
                res.problems.push_back("entity '" + nodes[t].sourceId + "' is referenced but has no '" + opts.nameKey + "'; generated '" + resolved[t] + "'");
                named = 0;
            }
            const std::string chosen = resolved[targets[named]];
            for (const size_t t : targets) {
                if (resolved[t].empty()) {
                    resolved[t] = chosen;
                    res.problems.push_back("entity '" + nodes[t].sourceId + "' is referenced together with '" + chosen + "' and has no '" + opts.nameKey + "'; it takes that name");
                } else if (resolved[t] != chosen) {
                    res.problems.push_back(who + ": ref '" + r.key + "' has targets with different names ('" + chosen + "', '" + resolved[t] + "'); only '" + chosen + "' is written");
                }
            }
            n.entity.keyvalues[r.key] = chosen;
        }
    }

    // 3) write names, emit (several entities may share a name: that is how one reference triggers many)
    for (size_t i = 0; i < nodes.size(); ++i) {
        Node& n = nodes[i];
        if (!resolved[i].empty()) {
            n.entity.keyvalues[opts.nameKey] = resolved[i];
        }
        n.entity.refs.clear();
        if (EntityClassRegistry::isEditorOnly(n.entity.classname)) continue;  // editor-only volumes never reach the compiler
        res.entities.push_back(FlattenedEntity{std::move(n.entity), resolved[i], n.chainNames, pathWithRoot(n.chainIds), n.sourceId});
    }
    return res;
}

}  // namespace core
