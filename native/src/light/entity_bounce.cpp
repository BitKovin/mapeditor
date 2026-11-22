// light/entity_bounce -- see entity_bounce.h.
#include "light/entity_bounce.h"

#include <core/parallel.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace native::light {

namespace {

constexpr uint32_t kNone = 0xffffffffu;
constexpr double kPi = 3.14159265358979323846;

uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

void frameOf(const core::Vec3& n, core::Vec3& t, core::Vec3& b) {
    const core::Vec3 up = std::fabs(n.y) < 0.9 ? core::Vec3{0, 1, 0} : core::Vec3{1, 0, 0};
    t = up.cross(n);
    const double l = t.length();
    t = l > 1e-12 ? t * (1.0 / l) : core::Vec3{1, 0, 0};
    b = n.cross(t);
}

float luminance(const float w[3]) { return 0.2126f * w[0] + 0.7152f * w[1] + 0.0722f * w[2]; }

} // namespace

EntityBounce::EntityBounce(const core::OccluderMesh& mesh, const core::TriangleBvh& bvh, const core::LightScene& lights, const core::BakeOptions& direct, const BounceSettings& settings)
    : mesh_(mesh), bvh_(bvh), lights_(lights), direct_(direct), cfg_(settings) {}

void EntityBounce::cast(const std::vector<core::Vec3>& p, const std::vector<core::Vec3>& n, const std::vector<uint32_t>& seed, int rays, std::vector<RayHit>& out) const {
    const auto t0 = std::chrono::steady_clock::now();
    const size_t count = p.size();
    out.assign(count * static_cast<size_t>(rays), RayHit{});
    const bool textured = mesh_.hasAlbedo();
    core::parallelFor(count, 16, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const core::Vec3 o = p[i] + n[i] * core::kShadowBias;
            core::Vec3 t, b;
            frameOf(n[i], t, b);
            const double rot = static_cast<double>(mix(seed[i]) >> 11) / 9007199254740992.0;
            for (int k = 0; k < rays; ++k) {
                const double u = (k + 0.5) / rays;
                double v = rot + k * 0.6180339887498949;
                v -= std::floor(v);
                const double r = std::sqrt(u), phi = 2.0 * kPi * v;
                const core::Vec3 dir = t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n[i] * std::sqrt(std::max(0.0, 1.0 - u));
                const auto h = bvh_.closestHit(o, dir, 10000.0);
                if (h.t < 0) continue;
                RayHit& out1 = out[i * static_cast<size_t>(rays) + static_cast<size_t>(k)];
                const float* tr = &mesh_.tris[static_cast<size_t>(h.triangle) * 9];
                const core::Vec3 a{tr[0], tr[1], tr[2]}, bb{tr[3], tr[4], tr[5]}, c{tr[6], tr[7], tr[8]};
                core::Vec3 gn = (bb - a).cross(c - a);
                const double gl = gn.length();
                if (gl < 1e-12) continue;
                gn = gn * (1.0 / gl);
                if (gn.dot(dir) > 0) gn = gn * -1.0;
                const double w0 = 1 - h.b1 - h.b2;
                core::Vec3 colour{1, 1, 1};
                if (textured) {   // the hit surface's own colour, as the Core's gather: the texture at the hit point
                    const float* am = &mesh_.albedoMeta[static_cast<size_t>(h.triangle) * 8];
                    const int tex = static_cast<int>(std::lround(am[6]));
                    if (tex >= 0) {
                        const double au = w0 * am[0] + h.b1 * am[2] + h.b2 * am[4], av = w0 * am[1] + h.b1 * am[3] + h.b2 * am[5];
                        colour = mesh_.albedo->images[static_cast<size_t>(tex)]->sample(au, av, am[7]);
                    }
                }
                out1.hit = true;
                out1.tri = h.triangle;
                out1.p = o + dir * h.t;
                out1.n = gn;
                out1.w[0] = static_cast<float>(cfg_.albedo * colour.x);
                out1.w[1] = static_cast<float>(cfg_.albedo * colour.y);
                out1.w[2] = static_cast<float>(cfg_.albedo * colour.z);
            }
        }
    });
    castSeconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

uint32_t EntityBounce::surfelFor(const RayHit& h, int depth, std::vector<uint32_t>& fresh) {
    const double q = std::max(cfg_.cellSize, 0.01);
    uint64_t key = mix(h.tri);
    key = mix(key ^ static_cast<uint64_t>(static_cast<int64_t>(std::floor(h.p.x / q))));
    key = mix(key ^ static_cast<uint64_t>(static_cast<int64_t>(std::floor(h.p.y / q))));
    key = mix(key ^ static_cast<uint64_t>(static_cast<int64_t>(std::floor(h.p.z / q))));
    const auto it = index_.find(key);
    if (it != index_.end()) {
        Surfel& s = surfels_[it->second];
        if (depth < s.depth) s.depth = depth;
        return it->second;
    }
    if (surfels_.size() >= cfg_.maxSurfels) return kNone;
    Surfel s;
    s.p = h.p;
    s.n = h.n;
    s.depth = depth;
    surfels_.push_back(s);
    const uint32_t id = static_cast<uint32_t>(surfels_.size() - 1);
    index_.emplace(key, id);
    fresh.push_back(id);
    return id;
}

void EntityBounce::evaluateDirect(const std::vector<uint32_t>& ids) {
    if (ids.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();
    core::TexelSamples ts;
    ts.atlas = 0;
    ts.pageCount = 0;
    ts.pos.reserve(ids.size() * 4);
    ts.nrm.reserve(ids.size() * 4);
    for (uint32_t id : ids) {
        const Surfel& s = surfels_[id];
        ts.pos.insert(ts.pos.end(), {static_cast<float>(s.p.x), static_cast<float>(s.p.y), static_cast<float>(s.p.z), 1.f});
        ts.nrm.insert(ts.nrm.end(), {static_cast<float>(s.n.x), static_cast<float>(s.n.y), static_cast<float>(s.n.z), 0.f});
    }
    ts.texel.assign(ids.size(), 0u);
    ts.seed = ids;
    std::vector<float> db;
    core::BakeOptions o = direct_;
    o.bounces = 0;
    o.areaSamples = std::min(o.areaSamples, cfg_.surfelAreaSamples);
    o.skySamples = std::min(o.skySamples, cfg_.surfelSkySamples);
    core::evaluateDirectWithBounceCpu(ts, lights_, o.shadows ? &bvh_ : nullptr, o, &db);
    for (size_t i = 0; i < ids.size(); ++i)
        for (int c = 0; c < 3; ++c) surfels_[ids[i]].db[c] = db[i * 4 + static_cast<size_t>(c)];
    directSeconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

std::vector<float> EntityBounce::gather(const core::TexelSamples& samples) {
    const size_t n = samples.count();
    std::vector<float> out(n * 4, 0.f);
    if (n == 0 || mesh_.empty()) return out;
    const int R = std::max(1, cfg_.raysPerSample), Rs = std::max(1, cfg_.raysPerSurfel);

    std::vector<uint32_t> fresh, pending;
    std::vector<char> queued(surfels_.size(), 0);
    const auto touch = [&](const RayHit& h, int depth) -> uint32_t {
        const uint32_t id = surfelFor(h, depth, fresh);
        if (id == kNone) return id;
        if (queued.size() < surfels_.size()) queued.resize(surfels_.size(), 0);
        const Surfel& s = surfels_[id];
        if (!s.expanded && !queued[id] && s.depth <= cfg_.depth) {
            queued[id] = 1;
            pending.push_back(id);
        }
        return id;
    };

    // ---- the probe rays
    std::vector<core::Vec3> p(n), nr(n);
    std::vector<uint32_t> seed(n);
    for (size_t i = 0; i < n; ++i) {
        p[i] = {samples.pos[i * 4], samples.pos[i * 4 + 1], samples.pos[i * 4 + 2]};
        nr[i] = {samples.nrm[i * 4], samples.nrm[i * 4 + 1], samples.nrm[i * 4 + 2]};
        seed[i] = static_cast<uint32_t>(i) + 0x51ed270bu;
    }
    std::vector<RayHit> hits;
    cast(p, nr, seed, R, hits);
    rays_ += hits.size();
    std::vector<Edge> sampleEdges(hits.size(), Edge{kNone, {0, 0, 0}});
    for (size_t i = 0; i < hits.size(); ++i) {
        if (!hits[i].hit) continue;
        const uint32_t id = touch(hits[i], 1);
        if (id == kNone) continue;
        sampleEdges[i].surfel = id;
        for (int c = 0; c < 3; ++c) sampleEdges[i].w[c] = hits[i].w[c];
    }
    evaluateDirect(fresh);
    fresh.clear();

    // ---- the surfels' own rays, level by level
    while (!pending.empty()) {
        const std::vector<uint32_t> cur = std::move(pending);
        pending.clear();
        std::vector<core::Vec3> sp(cur.size()), sn(cur.size());
        std::vector<uint32_t> ss(cur.size());
        for (size_t i = 0; i < cur.size(); ++i) {
            sp[i] = surfels_[cur[i]].p;
            sn[i] = surfels_[cur[i]].n;
            ss[i] = cur[i] + 0x2545f491u;
        }
        std::vector<RayHit> sh;
        cast(sp, sn, ss, Rs, sh);
        rays_ += sh.size();
        for (size_t i = 0; i < cur.size(); ++i) {
            const uint32_t id = cur[i];
            const int depth = surfels_[id].depth;
            const uint32_t first = static_cast<uint32_t>(edges_.size());
            for (int k = 0; k < Rs; ++k) {
                const RayHit& h = sh[i * static_cast<size_t>(Rs) + static_cast<size_t>(k)];
                if (!h.hit) continue;
                const uint32_t child = touch(h, depth + 1);
                if (child == kNone) continue;
                Edge e;
                e.surfel = child;
                for (int c = 0; c < 3; ++c) e.w[c] = h.w[c];
                edges_.push_back(e);
            }
            Surfel& s = surfels_[id];
            s.first = first;
            s.count = static_cast<uint32_t>(edges_.size()) - first;
            s.expanded = true;
        }
        evaluateDirect(fresh);
        fresh.clear();
    }

    // ---- the loop gain (how much of the light a surfel gathers comes back round) and the tail it stands for
    double sum = 0;
    size_t expanded = 0;
    for (const Surfel& s : surfels_) {
        if (!s.expanded) continue;
        ++expanded;
        for (uint32_t e = s.first; e < s.first + s.count; ++e) sum += luminance(edges_[e].w);
    }
    gain_ = expanded > 0 ? std::clamp(sum / (static_cast<double>(expanded) * Rs), 0.0, cfg_.maxLoopGain) : 0.0;
    const float tail = static_cast<float>(1.0 / (1.0 - gain_));
    for (Surfel& s : surfels_)
        for (int c = 0; c < 3; ++c) s.s[c] = s.db[c] * tail;

    // ---- sweeps S = Db + I(S) (the leaves keep Db * tail)
    std::vector<std::array<float, 3>> next(surfels_.size());
    for (int sweep = 0; sweep < std::max(1, cfg_.sweeps); ++sweep) {
        core::parallelFor(surfels_.size(), 256, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const Surfel& s = surfels_[i];
                float acc[3] = {0, 0, 0};
                for (uint32_t e = s.first; e < s.first + s.count; ++e) {
                    const Edge& ed = edges_[e];
                    const Surfel& c = surfels_[ed.surfel];
                    for (int k = 0; k < 3; ++k) acc[k] += ed.w[k] * c.s[k];
                }
                for (int k = 0; k < 3; ++k) next[i][static_cast<size_t>(k)] = s.expanded ? s.db[k] + acc[k] / static_cast<float>(Rs) : s.db[k] * tail;
            }
        });
        for (size_t i = 0; i < surfels_.size(); ++i)
            for (int k = 0; k < 3; ++k) surfels_[i].s[k] = next[i][static_cast<size_t>(k)];
    }

    // ---- what the samples gathered
    for (size_t i = 0; i < n; ++i) {
        float acc[3] = {0, 0, 0};
        for (int k = 0; k < R; ++k) {
            const Edge& e = sampleEdges[i * static_cast<size_t>(R) + static_cast<size_t>(k)];
            if (e.surfel == kNone) continue;
            const Surfel& s = surfels_[e.surfel];
            for (int c = 0; c < 3; ++c) acc[c] += e.w[c] * s.s[c];
        }
        for (int c = 0; c < 3; ++c) out[i * 4 + static_cast<size_t>(c)] = acc[c] / static_cast<float>(R);
    }
    return out;
}

} // namespace native::light
