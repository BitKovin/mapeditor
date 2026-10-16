// Reliefs over faces and the polygon entry point: ports of shell/src/model/reliefApply.ts and the fragment texturing of model/derived.ts
// (buildCarvedMesh), plus the environment that loads textures and texture groups from the assets folder (texplan.h).
#include "core/albedo.h"
#include "core/profile.h"
#include "core/texgroups.h"
#include "core/textures.h"
#include "core/texplan.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>

namespace core::texplan {

namespace {
constexpr double kEps = 1e-9;
V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
std::optional<V3> unit(const V3& a) { const double l = std::sqrt(dot(a, a)); if (l < 1e-12) return std::nullopt; return V3{a[0] / l, a[1] / l, a[2] / l}; }

struct P { double u, v; };
double polyArea(const std::vector<P>& p) { double s = 0; for (size_t i = 0; i < p.size(); ++i) { const P& a = p[i]; const P& b = p[(i + 1) % p.size()]; s += a.u * b.v - b.u * a.v; } return std::fabs(s) / 2; }
std::vector<P> clip(const std::vector<P>& poly, double a, double b, double c) {
    std::vector<P> out;
    for (size_t i = 0; i < poly.size(); ++i) {
        const P& p = poly[i];
        const P& q = poly[(i + 1) % poly.size()];
        const double fp = a * p.u + b * p.v + c, fq = a * q.u + b * q.v + c;
        if (fp >= 0) out.push_back(p);
        if ((fp >= 0) != (fq >= 0)) { const double t = fp / (fp - fq); out.push_back({p.u + (q.u - p.u) * t, p.v + (q.v - p.v) * t}); }
    }
    return out;
}

struct RTri { std::array<V3, 3> v; bool wall; };
std::vector<RTri> reliefTris(const Relief& r) {
    Brush b;
    b.id = "relief";
    for (const V3& v : r.vertices) b.vertices.push_back({v[0], v[1], v[2]});
    for (size_t i = 0; i < r.faces.size(); ++i) {
        BrushFace f;
        f.id = static_cast<uint32_t>(i);
        for (int k : r.faces[i]) f.loop.push_back(static_cast<uint32_t>(k));
        b.faces.push_back(f);
    }
    std::vector<RTri> out;
    for (const auto& f : b.faces)
        for (const auto& t : triangulateFace(b, f)) {
            const V3 a = v3(b.vertices[t[0]]), c = v3(b.vertices[t[1]]), d = v3(b.vertices[t[2]]);
            const double area = (c[0] - a[0]) * (d[1] - a[1]) - (d[0] - a[0]) * (c[1] - a[1]);
            out.push_back({{a, c, d}, std::fabs(area) < 1e-9});
        }
    return out;
}
}  // namespace

std::shared_ptr<const Relief> reliefOf(const InfoFn& info, const std::string& texture) {
    const TexInfo* i = info ? info(texture.empty() ? std::string("dev/grey") : texture) : nullptr;
    if (!i || !i->props.relief || isFlatRelief(*i->props.relief)) return nullptr;
    return i->props.relief;
}

std::optional<std::vector<TexturedTri>> reliefTriangle(const std::array<V3, 3>& tri, const std::array<V2, 3>& uv, const V3& n, const Relief& relief,
                                                       const std::string& texture, ReliefBudget* budget) {
    const V2 a0 = uv[0], a1 = uv[1], a2 = uv[2];
    const double det = (a1[0] - a0[0]) * (a2[1] - a0[1]) - (a2[0] - a0[0]) * (a1[1] - a0[1]);
    if (std::fabs(det) < kEps) return std::nullopt;
    const double sb = det > 0 ? 1 : -1;
    const double u0 = std::min({a0[0], a1[0], a2[0]}), u1 = std::max({a0[0], a1[0], a2[0]}), v0 = std::min({a0[1], a1[1], a2[1]}), v1 = std::max({a0[1], a1[1], a2[1]});
    const long long i0 = static_cast<long long>(std::floor(u0 + 1e-9)), i1 = static_cast<long long>(std::ceil(u1 - 1e-9)) - 1;
    const long long j0 = static_cast<long long>(std::floor(v0 + 1e-9)), j1 = static_cast<long long>(std::ceil(v1 - 1e-9)) - 1;
    if ((i1 - i0 + 1) * (j1 - j0 + 1) > kMaxReliefCells) return std::nullopt;
    const V3 e1 = sub(tri[1], tri[0]), e2 = sub(tri[2], tri[0]);
    const V2 du{a1[0] - a0[0], a1[1] - a0[1]}, dv{a2[0] - a0[0], a2[1] - a0[1]};
    auto world = [&](double u, double v) -> V3 {
        const double x = u - a0[0], y = v - a0[1];
        const double s = (x * dv[1] - y * dv[0]) / det, t = (du[0] * y - du[1] * x) / det;
        return {tri[0][0] + e1[0] * s + e2[0] * t, tri[0][1] + e1[1] * s + e2[1] * t, tri[0][2] + e1[2] * s + e2[2] * t};
    };
    auto at = [&](double u, double v, double z) -> V3 { const V3 w = world(u, v); return {w[0] + n[0] * z, w[1] + n[1] * z, w[2] + n[2] * z}; };
    const V3 wu = sub(world(1, 0), world(0, 0)), wv = sub(world(0, 1), world(0, 0));
    const double mapSign = dot(cross(wu, {-wv[0], -wv[1], -wv[2]}), n) >= 0 ? 1 : -1;
    const V2 U[3] = {a0, a1, a2};
    std::array<double, 3> edges[3];
    for (int k = 0; k < 3; ++k) {
        const V2 p = U[k], q = U[(k + 1) % 3];
        const double ex = q[0] - p[0], ey = q[1] - p[1];
        edges[k] = {-ey * sb, ex * sb, (ey * p[0] - ex * p[1]) * sb};
    }
    static std::mutex cacheM;
    static std::map<const Relief*, std::shared_ptr<const std::vector<RTri>>> cache;
    std::shared_ptr<const std::vector<RTri>> rtsPtr;
    {
        std::lock_guard<std::mutex> lock(cacheM);
        auto& slot = cache[&relief];
        if (!slot) slot = std::make_shared<const std::vector<RTri>>(reliefTris(relief));
        rtsPtr = slot;
    }
    const auto& rts = *rtsPtr;
    std::vector<TexturedTri> out;
    auto push3 = [&](const std::array<V3, 3>& p, const V3& nn, const std::array<V2, 3>& t) {
        TexturedTri tt;
        tt.texture = texture;
        tt.pos = p;
        tt.uv = t;
        tt.normal = {nn, nn, nn};
        out.push_back(tt);
    };
    const std::vector<P> base = {{a0[0], a0[1]}, {a1[0], a1[1]}, {a2[0], a2[1]}};
    for (long long j = j0; j <= j1; ++j)
        for (long long i = i0; i <= i1; ++i) {
            const double id = static_cast<double>(i), jd = static_cast<double>(j);
            std::vector<P> cell = clip(clip(clip(clip(base, 1, 0, -id), -1, 0, id + 1), 0, 1, -jd), 0, -1, jd + 1);
            if (cell.size() < 3) cell.clear();
            for (const RTri& rt : rts) {
                std::array<V3, 3> Q;
                for (int m = 0; m < 3; ++m) Q[static_cast<size_t>(m)] = {id + rt.v[static_cast<size_t>(m)][0], jd + 1 - rt.v[static_cast<size_t>(m)][1], rt.v[static_cast<size_t>(m)][2]};
                if (!rt.wall) {
                    if (cell.empty()) continue;
                    const double so = ((Q[1][0] - Q[0][0]) * (Q[2][1] - Q[0][1]) - (Q[2][0] - Q[0][0]) * (Q[1][1] - Q[0][1])) > 0 ? 1 : -1;
                    std::vector<P> poly = cell;
                    for (int k = 0; k < 3 && poly.size() >= 3; ++k) {
                        const V3 p = Q[static_cast<size_t>(k)], q = Q[static_cast<size_t>((k + 1) % 3)];
                        const double ex = q[0] - p[0], ey = q[1] - p[1];
                        poly = clip(poly, -ey * so, ex * so, (ey * p[0] - ex * p[1]) * so);
                    }
                    if (poly.size() < 3 || polyArea(poly) < 1e-12) continue;
                    const double d = (Q[1][0] - Q[0][0]) * (Q[2][1] - Q[0][1]) - (Q[2][0] - Q[0][0]) * (Q[1][1] - Q[0][1]);
                    auto zAt = [&](double u, double v) {
                        const double x = u - Q[0][0], y = v - Q[0][1];
                        const double s = (x * (Q[2][1] - Q[0][1]) - y * (Q[2][0] - Q[0][0])) / d, t = ((Q[1][0] - Q[0][0]) * y - (Q[1][1] - Q[0][1]) * x) / d;
                        return Q[0][2] + (Q[1][2] - Q[0][2]) * s + (Q[2][2] - Q[0][2]) * t;
                    };
                    const V3 c0 = at(Q[0][0], Q[0][1], Q[0][2]), c1 = at(Q[1][0], Q[1][1], Q[1][2]), c2 = at(Q[2][0], Q[2][1], Q[2][2]);
                    const auto raw = unit(cross(sub(c1, c0), sub(c2, c0)));
                    if (!raw) continue;
                    const V3 nn = so * sb > 0 ? *raw : V3{-(*raw)[0], -(*raw)[1], -(*raw)[2]};
                    const long long tris = static_cast<long long>(poly.size()) - 2;
                    if (budget) { if (budget->left < tris) return std::nullopt; budget->left -= tris; }
                    for (size_t k = 1; k + 1 < poly.size(); ++k) {
                        const P q3[3] = {poly[0], poly[k], poly[k + 1]};
                        std::array<V3, 3> pos;
                        std::array<V2, 3> uvs;
                        for (int m = 0; m < 3; ++m) { pos[static_cast<size_t>(m)] = at(q3[m].u, q3[m].v, zAt(q3[m].u, q3[m].v)); uvs[static_cast<size_t>(m)] = {q3[m].u, q3[m].v}; }
                        push3(pos, nn, uvs);
                    }
                } else {
                    int L0 = 0, L1 = 0;
                    double best = -1;
                    for (int x = 0; x < 3; ++x)
                        for (int y = x + 1; y < 3; ++y) {
                            const double l = std::hypot(Q[static_cast<size_t>(x)][0] - Q[static_cast<size_t>(y)][0], Q[static_cast<size_t>(x)][1] - Q[static_cast<size_t>(y)][1]);
                            if (l > best) { best = l; L0 = x; L1 = y; }
                        }
                    if (best < 1e-9) continue;
                    const V3 P0 = Q[static_cast<size_t>(L0)], P1 = Q[static_cast<size_t>(L1)];
                    double slo = 0, shi = 1;
                    for (const auto& e : edges) {
                        const double f0 = e[0] * P0[0] + e[1] * P0[1] + e[2], f1 = e[0] * P1[0] + e[1] * P1[1] + e[2];
                        if (std::fabs(f1 - f0) < 1e-12) { if (f0 < 0) { slo = 1; shi = 0; } continue; }
                        const double s = -f0 / (f1 - f0);
                        if (f1 > f0) slo = std::max(slo, s); else shi = std::min(shi, s);
                    }
                    if (shi - slo < 1e-9) continue;
                    std::vector<P> poly;
                    for (const V3& q : Q) poly.push_back({((q[0] - P0[0]) * (P1[0] - P0[0]) + (q[1] - P0[1]) * (P1[1] - P0[1])) / (best * best), q[2]});
                    poly = clip(clip(poly, 1, 0, -slo), -1, 0, shi);
                    if (poly.size() < 3) continue;
                    const long long tris = static_cast<long long>(poly.size()) - 2;
                    if (budget) { if (budget->left < tris) return std::nullopt; budget->left -= tris; }
                    auto pos = [&](const P& p) { return at(P0[0] + (P1[0] - P0[0]) * p.u, P0[1] + (P1[1] - P0[1]) * p.u, p.v); };
                    for (size_t k = 1; k + 1 < poly.size(); ++k) {
                        std::array<P, 3> t3 = {poly[0], poly[k], poly[k + 1]};
                        if (mapSign < 0) std::swap(t3[1], t3[2]);
                        const std::array<V3, 3> w = {pos(t3[0]), pos(t3[1]), pos(t3[2])};
                        const auto nn = unit(cross(sub(w[1], w[0]), sub(w[2], w[0])));
                        if (!nn) continue;
                        std::array<V2, 3> uvs;
                        for (int m = 0; m < 3; ++m) uvs[static_cast<size_t>(m)] = {P0[0] + (P1[0] - P0[0]) * t3[static_cast<size_t>(m)].u, P0[1] + (P1[1] - P0[1]) * t3[static_cast<size_t>(m)].u};
                        push3(w, *nn, uvs);
                    }
                }
            }
        }
    return out;
}

namespace {
V3 lerp3(const std::array<V3, 3>& t, const V3& w) {
    return {t[0][0] * w[0] + t[1][0] * w[1] + t[2][0] * w[2], t[0][1] * w[0] + t[1][1] * w[1] + t[2][1] * w[2], t[0][2] * w[0] + t[1][2] * w[1] + t[2][2] * w[2]};
}
bool planHasRelief(const FacePlan& plan, const InfoFn& info) {
    for (const auto& t : plan.textures) if (reliefOf(info, t)) return true;
    return false;
}
void flatTri(std::vector<TexturedTri>& out, const std::string& tex, const std::array<V3, 3>& p, const std::array<V2, 3>& uv, const V3& n) {
    TexturedTri t;
    t.texture = tex;
    t.pos = p;
    t.uv = uv;
    t.normal = {n, n, n};
    out.push_back(t);
}
}  // namespace

std::vector<TexturedTri> texturePolygon(const std::vector<V3>& pts, const V3& normal, const Brush* pb, const BrushFace* pf, const InfoFn& info,
                                        const GroupFn& groups, ReliefBudget* budget, const std::string& defaultTexture) {
    std::vector<TexturedTri> out;
    const size_t n = pts.size();
    if (n < 3) return out;
    const bool parent = pb && pf;
    const std::string tex = parent && !pf->texture.empty() ? pf->texture : defaultTexture;
    const auto relief = reliefOf(info, tex);
    const PlanPtr plan = parent ? planForFace(*pb, *pf, info, groups) : nullptr;
    std::vector<V2> uv;
    if (!plan) {
        if (parent) { const auto at = faceUvAt(*pb, *pf, info); for (const V3& p : pts) uv.push_back(at(p)); }
        else { const Projection def; for (const V3& p : pts) uv.push_back(projectPoint(normal, p, def)); }
    }
    const bool withRelief = plan && planHasRelief(*plan, info);
    for (size_t i = 1; i + 1 < n; ++i) {
        const std::array<V3, 3> tri = {pts[0], pts[i], pts[i + 1]};
        if (relief && !plan) {
            if (auto got = reliefTriangle(tri, {uv[0], uv[i], uv[i + 1]}, normal, *relief, tex, budget)) { out.insert(out.end(), got->begin(), got->end()); continue; }
        }
        if (plan) {
            for (const Piece& piece : plan->pieces(tri)) {
                for (size_t k = 1; k + 1 < piece.verts.size(); ++k) {
                    const PieceVert* v[3] = {&piece.verts[0], &piece.verts[k], &piece.verts[k + 1]};
                    const std::array<V3, 3> pos = {lerp3(tri, v[0]->w), lerp3(tri, v[1]->w), lerp3(tri, v[2]->w)};
                    const std::array<V2, 3> tuv = {v[0]->uv, v[1]->uv, v[2]->uv};
                    if (withRelief) {
                        const auto r = reliefOf(info, piece.texture);
                        if (r) if (auto got = reliefTriangle(pos, tuv, normal, *r, piece.texture, budget)) { out.insert(out.end(), got->begin(), got->end()); continue; }
                    }
                    flatTri(out, piece.texture, pos, tuv, normal);
                }
            }
        } else {
            flatTri(out, tex, tri, {uv[0], uv[i], uv[i + 1]}, normal);
        }
    }
    return out;
}

// ---- the environment --------------------------------------------------------------------------------------------------------------------------------
struct TexEnvironment::Impl {
    std::filesystem::path root;
    GameProfile profile;
    std::mutex m;
    std::unordered_map<std::string, std::unique_ptr<TexInfo>> infos;  // null = not available
    std::unordered_map<std::string, GroupPtr> groups;
    std::unordered_map<std::string, bool> groupTried;
};

TexEnvironment::TexEnvironment(std::filesystem::path assetsRoot, const GameProfile& profile) : d_(std::make_unique<Impl>()) {
    d_->root = std::move(assetsRoot);
    d_->profile = profile;
}
TexEnvironment::~TexEnvironment() = default;

const TexInfo* TexEnvironment::info(const std::string& path) {
    std::lock_guard<std::mutex> lock(d_->m);
    const auto it = d_->infos.find(path);
    if (it != d_->infos.end()) return it->second.get();
    std::unique_ptr<TexInfo> ti;
    if (const auto file = readTexture(d_->root, d_->profile, path)) {
        if (const auto img = decodeImage(file->bytes, file->ext)) {
            ti = std::make_unique<TexInfo>();
            ti->path = path;
            ti->width = img->width;
            ti->height = img->height;
            const auto props = readTextureProps(d_->root, d_->profile, path);
            ti->props = props ? parseProps(*props, path) : defaultProps();
            ti->ppm = ti->props.pixelsPerMeter ? *ti->props.pixelsPerMeter : d_->profile.pixelsPerMeter;
            ti->seamless = analyseSeams(img->rgba.data(), img->width, img->height);
        }
    }
    const TexInfo* out = ti.get();
    d_->infos[path] = std::move(ti);
    return out;
}

GroupPtr TexEnvironment::group(const std::string& name) {
    std::lock_guard<std::mutex> lock(d_->m);
    if (d_->groupTried[name]) { const auto it = d_->groups.find(name); return it == d_->groups.end() ? nullptr : it->second; }
    d_->groupTried[name] = true;
    if (!readTexGroup(d_->root, d_->profile, name)) return nullptr;  // validates the name and the file
    std::ifstream in(d_->root / d_->profile.texGroupRoot / (name + ".texgroup.json"), std::ios::binary);
    const nlohmann::ordered_json j = nlohmann::ordered_json::parse(in, nullptr, false);
    if (j.is_discarded()) return nullptr;
    const GroupParse p = parseGroup(j);
    if (p.group) d_->groups[name] = p.group;
    return p.group;
}

void TexEnvironment::setInfo(const TexInfo& i) {
    std::lock_guard<std::mutex> lock(d_->m);
    d_->infos[i.path] = std::make_unique<TexInfo>(i);
}
void TexEnvironment::setGroup(const std::string& name, GroupPtr g) {
    std::lock_guard<std::mutex> lock(d_->m);
    d_->groups[name] = std::move(g);
    d_->groupTried[name] = true;
}

}  // namespace core::texplan
