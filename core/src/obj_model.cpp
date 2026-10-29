#include "core/obj_model.h"
#include "core/entity_class.h"
#include "core/prefab.h"
#include "core/settings.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace core {

namespace {

constexpr double kDeg = 3.14159265358979323846 / 180.0;
constexpr uintmax_t kMaxObjBytes = uintmax_t(256) << 20;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
bool isImageExt(const std::string& ext) {
    const std::string e = lower(ext);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga";
}

bool readFile(const fs::path& file, std::string& out, uintmax_t limit) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return false;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec || size > limit) return false;
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    out.resize(static_cast<size_t>(size));
    return size == 0 || static_cast<bool>(in.read(out.data(), static_cast<std::streamsize>(size)));
}

// ---- OBJ -------------------------------------------------------------------------------------------------------------------------------------
struct Corner { long v = 0, t = 0, n = 0; };

// "v", "v/t", "v//n", "v/t/n"; 1 based, negative = from the end. 0 = absent.
Corner parseCorner(const std::string& tok, size_t nv, size_t nt, size_t nn) {
    long idx[3] = {0, 0, 0};
    size_t pos = 0;
    for (int k = 0; k < 3 && pos <= tok.size(); ++k) {
        const size_t slash = tok.find('/', pos);
        const std::string part = tok.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (!part.empty()) idx[k] = std::strtol(part.c_str(), nullptr, 10);
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    const size_t counts[3] = {nv, nt, nn};
    for (int k = 0; k < 3; ++k) if (idx[k] < 0) idx[k] = static_cast<long>(counts[k]) + idx[k] + 1;
    return {idx[0], idx[1], idx[2]};
}

std::string restOfLine(std::istringstream& ls) {
    std::string rest;
    std::getline(ls, rest);
    return trim(rest);
}

// map_Kd with options ("-s 1 1 1 -o 0 0 file.png"): the file name
std::string mapFile(const std::string& rest) {
    std::istringstream ls(rest);
    std::vector<std::string> tok;
    for (std::string t; ls >> t;) tok.push_back(t);
    std::string name;
    for (size_t i = 0; i < tok.size(); ++i) {
        if (tok[i][0] == '-' && tok[i].size() > 1 && !std::isdigit(static_cast<unsigned char>(tok[i][1]))) {  // an option: skip its numbers
            while (i + 1 < tok.size() && (std::isdigit(static_cast<unsigned char>(tok[i + 1][0])) || tok[i + 1][0] == '.' || tok[i + 1][0] == '-')) ++i;
            continue;
        }
        name += (name.empty() ? "" : " ") + tok[i];
    }
    return name;
}

std::map<std::string, std::string> parseMtl(const std::string& text) {
    std::map<std::string, std::string> out;  // material -> map_Kd text
    std::istringstream in(text);
    std::string line, cur;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string key;
        if (!(ls >> key)) continue;
        if (key == "newmtl") { cur = restOfLine(ls); out.emplace(cur, std::string()); }
        else if (!cur.empty() && lower(key) == "map_kd") { const std::string f = mapFile(restOfLine(ls)); if (!f.empty()) out[cur] = f; }
    }
    return out;
}

std::string slashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// A file `want` (as written in the .mtl, any slashes) in `dir`: the exact relative path, else its last part in `dir`, ignoring case.
std::optional<fs::path> findImage(const fs::path& dir, const std::string& want) {
    std::error_code ec;
    const fs::path rel = pathFromUtf8(slashes(want));
    for (const fs::path& cand : {dir / rel, dir / rel.filename()}) {
        if (fs::is_regular_file(cand, ec) && isImageExt(cand.extension().string())) return cand;
    }
    const std::string wantName = lower(pathToUtf8(rel.filename()));
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !isImageExt(it->path().extension().string())) continue;
        if (lower(pathToUtf8(it->path().filename())) == wantName) return it->path();
    }
    return std::nullopt;
}

// An image in `dir` named like the material: the same stem, else the shortest stem that starts with the material's name.
std::optional<fs::path> findImageByMaterial(const fs::path& dir, const std::string& material) {
    if (material.empty()) return std::nullopt;
    const std::string want = lower(material);
    std::error_code ec;
    std::optional<fs::path> best;
    size_t bestLen = ~size_t(0);
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !isImageExt(it->path().extension().string())) continue;
        const std::string stem = lower(pathToUtf8(it->path().stem()));
        if (stem == want) return it->path();
        if (stem.rfind(want, 0) == 0 && stem.size() < bestLen) { best = it->path(); bestLen = stem.size(); }
    }
    return best;
}

// "/models/props/crate" for <assets>/models/props/crate.png; "" when the file is not inside the assets root.
std::string textureName(const fs::path& assetsRoot, const fs::path& file) {
    std::error_code ec;
    const fs::path rel = fs::relative(fs::weakly_canonical(file, ec), fs::weakly_canonical(assetsRoot, ec), ec);
    if (ec || rel.empty() || rel.is_absolute()) return {};
    for (const auto& part : rel) if (part == "..") return {};
    fs::path noExt = rel;
    noExt.replace_extension();
    return "/" + noExt.generic_string();
}

struct Cached {
    std::shared_ptr<const ObjModel> model;
    fs::file_time_type objTime{};
    uintmax_t objSize = 0;
    std::vector<std::pair<fs::path, fs::file_time_type>> mtls;
    std::chrono::steady_clock::time_point checked{};
};
std::mutex gCacheM;
std::unordered_map<std::string, Cached> gCache;

}  // namespace

bool ObjModel::complete() const {
    for (const auto& t : textures) if (t.empty()) return false;
    return true;
}

ObjModel parseObj(const std::string& text, std::vector<std::string>* mtllibs) {
    ObjModel m;
    std::vector<Vec3> pos, nrm;
    std::vector<Vec2> uvs;
    std::map<std::string, int> materialOf;
    int current = -1;
    auto materialIndex = [&](const std::string& name) {
        const auto it = materialOf.find(name);
        if (it != materialOf.end()) return it->second;
        const int i = static_cast<int>(m.materials.size());
        m.materials.push_back(name);
        materialOf[name] = i;
        return i;
    };
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string key;
        if (!(ls >> key)) continue;
        if (key == "v") { double x = 0, y = 0, z = 0; ls >> x >> y >> z; pos.push_back({x, y, z}); }
        else if (key == "vn") { double x = 0, y = 0, z = 0; ls >> x >> y >> z; nrm.push_back(Vec3{x, y, z}.normalized()); }
        else if (key == "vt") { double u = 0, v = 0; ls >> u >> v; uvs.push_back({u, 1.0 - v}); }
        else if (key == "usemtl") current = materialIndex(restOfLine(ls));
        else if (key == "mtllib") { if (mtllibs) { const std::string f = restOfLine(ls); if (!f.empty()) mtllibs->push_back(f); } }
        else if (key == "f") {
            std::vector<Corner> c;
            for (std::string tok; ls >> tok;) {
                const Corner k = parseCorner(tok, pos.size(), uvs.size(), nrm.size());
                if (k.v < 1 || static_cast<size_t>(k.v) > pos.size()) { c.clear(); break; }
                c.push_back(k);
            }
            if (c.size() < 3) continue;
            if (current < 0) current = materialIndex(std::string());
            for (size_t i = 1; i + 1 < c.size(); ++i) {
                ObjTriangle t;
                const Corner* k[3] = {&c[0], &c[i], &c[i + 1]};
                t.material = current;
                t.hasUv = true;
                for (int q = 0; q < 3; ++q) {
                    t.p[q] = pos[static_cast<size_t>(k[q]->v - 1)];
                    if (k[q]->t >= 1 && static_cast<size_t>(k[q]->t) <= uvs.size()) t.uv[q] = uvs[static_cast<size_t>(k[q]->t - 1)];
                    else t.hasUv = false;
                }
                const Vec3 fn = (t.p[1] - t.p[0]).cross(t.p[2] - t.p[0]).normalized();
                for (int q = 0; q < 3; ++q)
                    t.n[q] = k[q]->n >= 1 && static_cast<size_t>(k[q]->n) <= nrm.size() ? nrm[static_cast<size_t>(k[q]->n - 1)] : fn;
                m.tris.push_back(t);
            }
        }
    }
    m.mn = {1e300, 1e300, 1e300};
    m.mx = {-1e300, -1e300, -1e300};
    for (const auto& t : m.tris)
        for (const Vec3& p : t.p) {
            m.mn = {std::min(m.mn.x, p.x), std::min(m.mn.y, p.y), std::min(m.mn.z, p.z)};
            m.mx = {std::max(m.mx.x, p.x), std::max(m.mx.y, p.y), std::max(m.mx.z, p.z)};
        }
    if (m.tris.empty()) m.mn = m.mx = {0, 0, 0};
    return m;
}

std::shared_ptr<const ObjModel> loadObjModel(const fs::path& assetsRoot, const std::string& relIn, std::string* error) {
    auto fail = [&](const std::string& why) -> std::shared_ptr<const ObjModel> { if (error) *error = why; return nullptr; };
    const std::string rel = slashes(trim(relIn));
    if (rel.empty()) return fail("no model path");
    if (rel.front() == '/' || rel.find(':') != std::string::npos) return fail("the model path must be relative to the game folder: " + rel);
    for (const auto& part : fs::path(pathFromUtf8(rel))) if (part == "..") return fail("the model path leaves the game folder: " + rel);
    const fs::path file = assetsRoot / pathFromUtf8(rel);
    if (lower(file.extension().string()) != ".obj") return fail("only .obj models are baked into the level: " + rel);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return fail("model not found: " + rel);
    const auto objTime = fs::last_write_time(file, ec);
    const uintmax_t objSize = fs::file_size(file, ec);
    const std::string key = pathToUtf8(file);
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(gCacheM);
        const auto it = gCache.find(key);
        if (it != gCache.end() && it->second.objTime == objTime && it->second.objSize == objSize) {
            bool same = true;
            for (const auto& [mtl, t] : it->second.mtls) { std::error_code e2; if (fs::last_write_time(mtl, e2) != t) same = false; }
            // a model with missing images is looked at again now and then: the image may have been added
            if (same && (it->second.model->complete() || now - it->second.checked < std::chrono::seconds(2))) return it->second.model;
        }
    }

    std::string text;
    if (!readFile(file, text, kMaxObjBytes)) return fail("cannot read " + rel + " (too big or unreadable)");
    std::vector<std::string> libs;
    auto model = std::make_shared<ObjModel>(parseObj(text, &libs));
    if (model->tris.empty()) return fail(rel + " has no triangles");
    const fs::path dir = file.parent_path();

    // material -> map_Kd of every .mtl the .obj names; without any, the .obj's own name is tried (crate.obj -> crate.mtl)
    std::map<std::string, std::string> mapKd;
    std::vector<std::pair<fs::path, fs::file_time_type>> mtls;
    if (libs.empty()) libs.push_back(pathToUtf8(file.stem()) + ".mtl");
    for (const std::string& lib : libs) {
        const fs::path mtl = dir / pathFromUtf8(slashes(lib));
        std::string mtext;
        if (!readFile(mtl, mtext, uintmax_t(16) << 20)) continue;
        std::error_code e2;
        mtls.push_back({mtl, fs::last_write_time(mtl, e2)});
        for (auto& [k, v] : parseMtl(mtext)) mapKd.emplace(k, v);
    }
    for (const std::string& name : model->materials) {
        std::optional<fs::path> img;
        const auto it = mapKd.find(name);
        if (it != mapKd.end() && !it->second.empty()) img = findImage(dir, it->second);
        if (!img) img = findImageByMaterial(dir, name);
        std::string tex = img ? textureName(assetsRoot, *img) : std::string();
        if (tex.empty() && !name.empty()) model->warnings.push_back("no image found for material '" + name + "' of " + rel);
        model->textures.push_back(tex);
    }
    Cached c;
    c.model = model;
    c.objTime = objTime;
    c.objSize = objSize;
    c.mtls = std::move(mtls);
    c.checked = now;
    std::lock_guard<std::mutex> lock(gCacheM);
    gCache[key] = std::move(c);
    return model;
}

// ---- surfaces ----------------------------------------------------------------------------------------------------------------------------------

namespace {

bool parseNumbers(const std::string& s, double* out, int n) {
    const char* p = s.c_str();
    for (int i = 0; i < n; ++i) {
        char* end = nullptr;
        out[i] = std::strtod(p, &end);
        if (end == p || !std::isfinite(out[i])) return false;
        p = end;
    }
    return true;
}

bool flagOn(const Entity& e, const char* key, bool dflt) {
    const auto it = e.keyvalues.find(key);
    if (it == e.keyvalues.end() || it->second.empty()) return dflt;
    const std::string v = lower(trim(it->second));
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return true;
}

struct Rot { double m[3][3]; };
Rot rotationFromAngles(const Vec3& deg) {  // Ry(yaw) * Rx(pitch) * Rz(roll), as prefab.cpp and the Shell's rotation.ts
    const double p = deg.x * kDeg, y = deg.y * kDeg, r = deg.z * kDeg;
    const double cp = std::cos(p), sp = std::sin(p), cy = std::cos(y), sy = std::sin(y), cr = std::cos(r), sr = std::sin(r);
    return {{{cy * cr + sy * sp * sr, -cy * sr + sy * sp * cr, sy * cp}, {cp * sr, cp * cr, -sp}, {-sy * cr + cy * sp * sr, sy * sr + cy * sp * cr, cy * cp}}};
}
Vec3 apply(const Rot& r, const Vec3& v) {
    return {r.m[0][0] * v.x + r.m[0][1] * v.y + r.m[0][2] * v.z, r.m[1][0] * v.x + r.m[1][1] * v.y + r.m[1][2] * v.z, r.m[2][0] * v.x + r.m[2][1] * v.y + r.m[2][2] * v.z};
}

// One entity's surfaces. Cached between calls (the lighting asks after every edit; a 50 000 triangle model is not rebuilt for a moved crate elsewhere).
struct EntityCache {
    std::string signature;
    std::vector<LightSurface> surfaces;
};
std::mutex gSurfaceM;
std::unordered_map<std::string, EntityCache> gSurfaces;

std::string signatureOf(const Entity& e, const ObjModel* model) {
    std::ostringstream o;
    o.precision(17);
    o << static_cast<const void*>(model) << '|' << e.origin.x << ',' << e.origin.y << ',' << e.origin.z << '|' << e.angles.x << ',' << e.angles.y << ',' << e.angles.z;
    for (const char* k : {"model_scale", "modelscale", "modelscale_vec", "_cs"}) { const auto it = e.keyvalues.find(k); o << '|' << (it == e.keyvalues.end() ? "-" : it->second); }
    return o.str();
}

}  // namespace

Vec3 modelScaleOf(const Entity& e) {
    Vec3 s{1, 1, 1};
    auto vec = [&](const char* key) {
        const auto it = e.keyvalues.find(key);
        if (it == e.keyvalues.end()) return;
        double v[3];
        if (parseNumbers(it->second, v, 3)) s = s.mul({v[0], v[1], v[2]});
        else if (parseNumbers(it->second, v, 1) && v[0] != 0) s = s * v[0];
    };
    vec("model_scale");
    vec("modelscale");
    vec("modelscale_vec");
    return s;
}

std::vector<LightSurface> collectModelSurfaces(const Document& doc, const std::vector<ResolvedEntity>& prefabs, const fs::path& assetsRoot) {
    struct Item { const Entity* e; std::string instance; };
    std::vector<Item> items;
    for (const auto& e : doc.entities) if (e.classname == "misc_model") items.push_back({&e, ""});
    for (const auto& r : prefabs) if (r.entity.classname == "misc_model") items.push_back({&r.entity, instanceString(r.instancePath)});
    std::vector<LightSurface> out;
    std::vector<std::string> used;
    for (const Item& it : items) {
        const Entity& e = *it.e;
        auto mp = e.keyvalues.find("model_path");
        if (mp == e.keyvalues.end() || trim(mp->second).empty()) mp = e.keyvalues.find("model");  // the classic key when model_path is unset or empty
        if (mp == e.keyvalues.end() || trim(mp->second).empty()) continue;
        const auto model = loadObjModel(assetsRoot, mp->second);
        if (!model) continue;
        const std::string key = it.instance + "|" + e.id;
        used.push_back(key);
        const std::string sig = signatureOf(e, model.get());
        {
            std::lock_guard<std::mutex> lock(gSurfaceM);
            const auto c = gSurfaces.find(key);
            if (c != gSurfaces.end() && c->second.signature == sig) { out.insert(out.end(), c->second.surfaces.begin(), c->second.surfaces.end()); continue; }
        }
        const Vec3 scale = modelScaleOf(e);
        const Rot R = rotationFromAngles(e.angles);
        const bool mirrored = scale.x * scale.y * scale.z < 0;
        const bool casts = flagOn(e, "_cs", true);
        std::vector<LightSurface> mine;
        mine.reserve(model->tris.size());
        for (size_t ti = 0; ti < model->tris.size(); ++ti) {
            const ObjTriangle& t = model->tris[ti];
            Vec3 p[3];
            Vec2 uv[3];
            for (int q = 0; q < 3; ++q) { p[q] = e.origin + apply(R, t.p[q].mul(scale)); uv[q] = t.uv[q]; }
            if (mirrored) { std::swap(p[1], p[2]); std::swap(uv[1], uv[2]); }
            const Vec3 e1 = p[1] - p[0], e2 = p[2] - p[0], N = e1.cross(e2);
            const double n2 = N.dot(N);
            if (n2 < 1e-14) continue;  // a sliver: no chart, no light
            LightSurface s;
            s.instance = it.instance;
            s.entityId = e.id;
            s.classname = "misc_model";
            s.brushId = "model";
            s.brushInstance = it.instance;
            s.faceId = static_cast<uint32_t>(ti);
            s.side = FaceSide::Front;
            s.texture = static_cast<size_t>(t.material) < model->textures.size() && !model->textures[static_cast<size_t>(t.material)].empty() ? model->textures[static_cast<size_t>(t.material)] : "dev/grey";
            s.normal = N.normalized();
            s.verts = {p[0], p[1], p[2]};
            s.castsShadow = casts;
            if (t.hasUv) {  // the triangle's affine map: u(p) = u0 + g . (p - p0), g in the plane with g . e1 = du1, g . e2 = du2
                const double du1 = uv[1].u - uv[0].u, du2 = uv[2].u - uv[0].u, dv1 = uv[1].v - uv[0].v, dv2 = uv[2].v - uv[0].v;
                const Vec3 a = e2.cross(N), b = N.cross(e1);
                const Vec3 gu = (a * du1 + b * du2) * (1.0 / n2), gv = (a * dv1 + b * dv2) * (1.0 / n2);
                s.uvMap.u[0] = gu.x; s.uvMap.u[1] = gu.y; s.uvMap.u[2] = gu.z; s.uvMap.u[3] = uv[0].u - gu.dot(p[0]);
                s.uvMap.v[0] = gv.x; s.uvMap.v[1] = gv.y; s.uvMap.v[2] = gv.z; s.uvMap.v[3] = uv[0].v - gv.dot(p[0]);
                s.hasUvMap = true;
                uint64_t h = 1469598103934665603ull;
                for (const double d : {s.uvMap.u[0], s.uvMap.u[1], s.uvMap.u[2], s.uvMap.u[3], s.uvMap.v[0], s.uvMap.v[1], s.uvMap.v[2], s.uvMap.v[3]}) {
                    uint64_t bits;
                    std::memcpy(&bits, &d, sizeof bits);
                    h = (h ^ bits) * 1099511628211ull;
                }
                s.lookHash = h;
            }
            mine.push_back(std::move(s));
        }
        out.insert(out.end(), mine.begin(), mine.end());
        std::lock_guard<std::mutex> lock(gSurfaceM);
        gSurfaces[key] = {sig, std::move(mine)};
    }
    // forget the entities that are gone
    std::lock_guard<std::mutex> lock(gSurfaceM);
    if (gSurfaces.size() > used.size()) {
        std::sort(used.begin(), used.end());
        for (auto i = gSurfaces.begin(); i != gSurfaces.end();) i = std::binary_search(used.begin(), used.end(), i->first) ? std::next(i) : gSurfaces.erase(i);
    }
    return out;
}

}  // namespace core
