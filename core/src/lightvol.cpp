#include "core/lightvol.h"
#include "gpu_bake.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace core {

namespace {
constexpr double kPi = 3.14159265358979323846;
double luminance(const Vec3& c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }
}  // namespace

PackedProbe packProbe(const Probe& p) {
    PackedProbe o;
    o.ambient[0] = lightvolToByte(p.ambient.x); o.ambient[1] = lightvolToByte(p.ambient.y); o.ambient[2] = lightvolToByte(p.ambient.z);
    o.directed[0] = lightvolToByte(p.directed.x); o.directed[1] = lightvolToByte(p.directed.y); o.directed[2] = lightvolToByte(p.directed.z);
    const Vec3 d = p.dir.normalized();
    o.lat = static_cast<uint8_t>(std::lround(std::acos(std::clamp(d.y, -1.0, 1.0)) / kPi * 255.0));
    o.lng = static_cast<uint8_t>(std::lround((std::atan2(d.z, d.x) + kPi) / (2 * kPi) * 255.0));
    o.flags = p.valid ? kProbeValid : 0;
    return o;
}

Probe unpackProbe(const PackedProbe& p) {
    Probe o;
    o.ambient = {lightvolFromByte(p.ambient[0]), lightvolFromByte(p.ambient[1]), lightvolFromByte(p.ambient[2])};
    o.directed = {lightvolFromByte(p.directed[0]), lightvolFromByte(p.directed[1]), lightvolFromByte(p.directed[2])};
    const double lat = p.lat / 255.0 * kPi, lng = p.lng / 255.0 * 2 * kPi - kPi;
    o.dir = {std::sin(lat) * std::cos(lng), std::cos(lat), std::sin(lat) * std::sin(lng)};
    o.valid = (p.flags & kProbeValid) != 0;
    return o;
}

std::vector<Vec3> sphereNormals(int count) {
    // Antipodal pairs (count rounded up to even): the mean normal is exactly zero, so a constant V has no first moment.
    const int half = std::max(1, (count + 1) / 2);
    std::vector<Vec3> n;
    n.reserve(static_cast<size_t>(half) * 2);
    const double golden = kPi * (3.0 - std::sqrt(5.0));
    for (int i = 0; i < half; ++i) {
        const double y = 1.0 - 2.0 * (i + 0.5) / half, r = std::sqrt(std::max(0.0, 1.0 - y * y)), a = golden * i;
        n.push_back({r * std::cos(a), y, r * std::sin(a)});
    }
    for (int i = 0; i < half; ++i) n.push_back(n[static_cast<size_t>(i)] * -1.0);
    return n;
}

Vec3 probeLight(const Probe& p, const Vec3& n) { return p.ambient + p.directed * std::max(0.0, n.dot(p.dir)); }

ProbeMoments probeMoments(const std::vector<Vec3>& normals, const std::vector<Vec3>& values, const std::vector<Vec3>* direct) {
    ProbeMoments m;
    const size_t k = std::min(normals.size(), values.size());
    if (k == 0) return m;
    for (size_t i = 0; i < k; ++i) {
        m.mean = m.mean + values[i];
        m.first[0] = m.first[0] + normals[i] * values[i].x;
        m.first[1] = m.first[1] + normals[i] * values[i].y;
        m.first[2] = m.first[2] + normals[i] * values[i].z;
        if (direct && i < direct->size()) {
            const Vec3& d = (*direct)[i];
            m.directMean = m.directMean + d;
            m.directFirst[0] = m.directFirst[0] + normals[i] * d.x;
            m.directFirst[1] = m.directFirst[1] + normals[i] * d.y;
            m.directFirst[2] = m.directFirst[2] + normals[i] * d.z;
        }
    }
    const double inv = 1.0 / static_cast<double>(k);
    m.mean = m.mean * inv;
    for (auto& g : m.first) g = g * inv;
    m.directMean = m.directMean * inv;
    for (auto& g : m.directFirst) g = g * inv;
    m.set = true;
    return m;
}

Probe fitProbeAlong(const ProbeMoments& m, const Vec3& dir) {
    Probe out;
    out.valid = true;
    out.dir = dir.normalized();
    const double a[3] = {m.mean.x, m.mean.y, m.mean.z};
    double dc[3], amb[3];
    for (int c = 0; c < 3; ++c) {
        const double along = std::max(m.first[c].dot(out.dir), m.directFirst[c].dot(out.dir));
        dc[c] = std::min(std::max(0.0, 6.0 * along), 4.0 * std::max(0.0, a[c]));
        amb[c] = std::max(0.0, a[c] - dc[c] / 4.0);
    }
    out.directed = {dc[0], dc[1], dc[2]};
    out.ambient = {amb[0], amb[1], amb[2]};
    return out;
}

Probe fitProbe(const std::vector<Vec3>& normals, const std::vector<Vec3>& values) {
    if (std::min(normals.size(), values.size()) == 0) return Probe{};
    const ProbeMoments m = probeMoments(normals, values);
    const Vec3 gl = m.first[0] * 0.2126 + m.first[1] * 0.7152 + m.first[2] * 0.0722;
    if (gl.length() < 1e-12) {
        Probe out;
        out.valid = true;
        out.ambient = {std::max(0.0, m.mean.x), std::max(0.0, m.mean.y), std::max(0.0, m.mean.z)};
        return out;
    }
    return fitProbeAlong(m, gl);
}

Probe fitProbeDirectFirst(const ProbeMoments& m, double bounceWeight) {
    const Vec3 up{0, 1, 0};
    const Vec3 total = m.first[0] * 0.2126 + m.first[1] * 0.7152 + m.first[2] * 0.0722, direct = m.directFirstLuminance();
    const Vec3 v = direct + (total - direct) * bounceWeight;
    // the direct share of the probe's mean light: from 12% up the weighted direction alone, below 2% light from above alone, smooth in between
    const double share = luminance(m.directMean) / std::max(luminance(m.mean), 1e-12);
    const double t = std::clamp((share - 0.02) / 0.10, 0.0, 1.0), keep = t * t * (3 - 2 * t);
    Vec3 dir = v.length() > 1e-12 ? v.normalized() * keep + up * (1 - keep) : up;
    dir = dir.length() > 1e-6 ? dir.normalized() : up;
    return fitProbeAlong(m, dir);
}

void shareDirection(const std::vector<Probe*>& layers) {
    Vec3 sum;
    int valid = 0;
    for (const Probe* p : layers) {
        if (!p || !p->valid) continue;
        ++valid;
        const double w = p->directed.x * 0.2126 + p->directed.y * 0.7152 + p->directed.z * 0.0722;
        sum = sum + p->dir * w;
    }
    if (valid == 0) return;
    Vec3 dir = sum.length() > 1e-12 ? sum.normalized() : Vec3{0, 1, 0};
    if (sum.length() <= 1e-12)
        for (const Probe* p : layers) if (p && p->valid) { dir = p->dir; break; }  // no directed light anywhere: keep the first layer's
    for (Probe* p : layers) {
        if (!p || !p->valid) continue;
        const double c = std::max(0.0, p->dir.dot(dir));
        const Vec3 d2 = p->directed * c;
        p->ambient = p->ambient + (p->directed - d2) * 0.25;
        p->directed = d2;
        p->dir = dir;
    }
}

void shareProbeDirections(std::map<int, LightvolGrid>& grids) {
    if (grids.size() < 2) return;
    const size_t n = grids.begin()->second.count();
    std::vector<Probe*> layers;
    for (size_t i = 0; i < n; ++i) {
        layers.clear();
        for (auto& [style, g] : grids) if (g.count() == n) layers.push_back(&g.probes[i]);
        shareDirection(layers);
    }
}

namespace {
constexpr int kNeighbour[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
}  // namespace

void fillInvalidProbes(LightvolGrid& grid) {
    std::vector<char> filled(grid.count());
    for (size_t i = 0; i < grid.count(); ++i) filled[i] = grid.probes[i].valid;
    for (bool changed = true; changed;) {
        changed = false;
        std::vector<std::pair<size_t, Probe>> ring;
        for (int z = 0; z < grid.nz; ++z)
            for (int y = 0; y < grid.ny; ++y)
                for (int x = 0; x < grid.nx; ++x) {
                    const size_t i = grid.index(x, y, z);
                    if (filled[i]) continue;
                    Probe sum;
                    Vec3 dir;
                    int count = 0;
                    for (const auto& o : kNeighbour) {
                        const int xx = x + o[0], yy = y + o[1], zz = z + o[2];
                        if (xx < 0 || yy < 0 || zz < 0 || xx >= grid.nx || yy >= grid.ny || zz >= grid.nz) continue;
                        const size_t j = grid.index(xx, yy, zz);
                        if (!filled[j]) continue;
                        const Probe& q = grid.probes[j];
                        sum.ambient = sum.ambient + q.ambient;
                        sum.directed = sum.directed + q.directed;
                        dir = dir + q.dir * (luminance(q.directed) + 1e-6);
                        ++count;
                    }
                    if (count == 0) continue;
                    sum.ambient = sum.ambient * (1.0 / count);
                    sum.directed = sum.directed * (1.0 / count);
                    sum.dir = dir.length() > 1e-12 ? dir.normalized() : Vec3{0, 1, 0};
                    sum.valid = true;
                    ring.push_back({i, sum});
                }
        for (auto& [i, p] : ring) { grid.probes[i] = p; filled[i] = 1; changed = true; }
    }
}

Vec3 LightvolGrid::position(size_t i) const {
    const int x = static_cast<int>(i % static_cast<size_t>(nx)), y = static_cast<int>((i / static_cast<size_t>(nx)) % static_cast<size_t>(ny)), z = static_cast<int>(i / (static_cast<size_t>(nx) * ny));
    return origin + Vec3{x * cell, y * cell, z * cell};
}

Probe LightvolGrid::sample(const Vec3& p) const {
    Probe out;
    if (probes.empty() || nx <= 0) return out;
    const Vec3 g = (p - origin) * (1.0 / cell);
    const int x0 = static_cast<int>(std::floor(g.x)), y0 = static_cast<int>(std::floor(g.y)), z0 = static_cast<int>(std::floor(g.z));
    const double fx = g.x - x0, fy = g.y - y0, fz = g.z - z0;
    double wsum = 0;
    Vec3 amb, dc, dir;
    double dirW = 0;
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                const int x = std::clamp(x0 + dx, 0, nx - 1), y = std::clamp(y0 + dy, 0, ny - 1), z = std::clamp(z0 + dz, 0, nz - 1);
                const Probe& q = probes[index(x, y, z)];
                if (!q.valid) continue;
                const double w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * (dz ? fz : 1 - fz);
                if (w <= 0) continue;
                amb = amb + q.ambient * w;
                dc = dc + q.directed * w;
                const double dw = w * (luminance(q.directed) + 1e-6);
                dir = dir + q.dir * dw;
                dirW += dw;
                wsum += w;
            }
    if (wsum > 1e-9) {
        out.ambient = amb * (1.0 / wsum);
        out.directed = dc * (1.0 / wsum);
        out.dir = dir.length() > 1e-12 ? dir.normalized() : Vec3{0, 1, 0};
        out.valid = true;
        return out;
    }
    // No valid corner (inside or next to a solid): the nearest valid probe within 3 cells.
    const int cx = std::clamp(static_cast<int>(std::lround(g.x)), 0, nx - 1), cy = std::clamp(static_cast<int>(std::lround(g.y)), 0, ny - 1), cz = std::clamp(static_cast<int>(std::lround(g.z)), 0, nz - 1);
    double best = 1e300;
    for (int z = std::max(0, cz - 3); z <= std::min(nz - 1, cz + 3); ++z)
        for (int y = std::max(0, cy - 3); y <= std::min(ny - 1, cy + 3); ++y)
            for (int x = std::max(0, cx - 3); x <= std::min(nx - 1, cx + 3); ++x) {
                const Probe& q = probes[index(x, y, z)];
                if (!q.valid) continue;
                const Vec3 d = origin + Vec3{x * cell, y * cell, z * cell} - p;
                const double l = d.dot(d);
                if (l < best) { best = l; out = q; }
            }
    return out;
}

void LightvolGrid::quantise() {
    for (Probe& p : probes) {
        if (!p.valid) continue;
        p = unpackProbe(packProbe(p));
    }
}

std::vector<uint8_t> LightvolGrid::packed() const {
    std::vector<uint8_t> out;
    out.reserve(probes.size() * kPackedProbeBytes);
    for (const Probe& p : probes) {
        const PackedProbe k = packProbe(p);
        out.insert(out.end(), {k.ambient[0], k.ambient[1], k.ambient[2], k.directed[0], k.directed[1], k.directed[2], k.lat, k.lng, k.flags});
    }
    return out;
}

LightvolGrid buildLightvolGrid(const std::vector<LightSurface>& surfaces, const SolidSet* solids, const LightvolSettings& settings) {
    LightvolGrid g;
    Aabb b;
    for (const auto& s : surfaces)
        for (const Vec3& v : s.verts) b.add(v);
    if (!b.valid()) return g;
    double cell = std::max(settings.gridSize, 0.05);
    auto dims = [&](double c, int& nx, int& ny, int& nz) {
        const Vec3 ext = b.mx - b.mn;
        nx = static_cast<int>(std::ceil(ext.x / c)) + 3;
        ny = static_cast<int>(std::ceil(ext.y / c)) + 3;
        nz = static_cast<int>(std::ceil(ext.z / c)) + 3;
    };
    int nx = 0, ny = 0, nz = 0;
    dims(cell, nx, ny, nz);
    while (settings.maxProbes > 0 && static_cast<long long>(nx) * ny * nz > settings.maxProbes) { cell *= 1.25; dims(cell, nx, ny, nz); }
    g.cell = cell;
    g.nx = nx; g.ny = ny; g.nz = nz;
    g.origin = b.mn - Vec3{cell, cell, cell};
    g.probes.assign(static_cast<size_t>(nx) * ny * nz, Probe{});
    (void)solids;  // validity is decided per probe when it is baked (solidProbes)
    return g;
}

std::vector<char> solidProbes(const LightvolGrid& grid, const SolidSet* solids) {
    std::vector<char> out(grid.count(), 0);
    if (!solids) return out;
    for (size_t i = 0; i < grid.count(); ++i) out[i] = solids->inside(grid.position(i)) ? 1 : 0;
    return out;
}

namespace {
// V(n_k) for one probe from the evaluators' per-sample outputs and the fit.
void fitFrom(LightvolGrid& grid, size_t probe, const std::vector<Vec3>& normals, const float* direct, const float* indirect, size_t at,
             double bounceWeight) {
    std::vector<Vec3> v(normals.size()), d(normals.size());
    for (size_t k = 0; k < normals.size(); ++k) {
        const size_t t = (at + k) * 4;
        d[k] = v[k] = {direct[t], direct[t + 1], direct[t + 2]};
        if (indirect) v[k] = v[k] + Vec3{indirect[t], indirect[t + 1], indirect[t + 2]};
    }
    grid.probes[probe] = fitProbeDirectFirst(probeMoments(normals, v, &d), bounceWeight);
}

BakeOptions probeOptions(const LightvolSource& src, const LightvolSettings& cfg) {
    BakeOptions o;
    o.shadows = src.bvh != nullptr && src.mesh != nullptr && !src.mesh->empty();
    o.areaSamples = cfg.areaSamples;
    o.skySamples = cfg.skySamples;
    o.superSamples = 1;  // probes have no sub-texel positions
    o.bounces = 0;
    o.classes = cfg.classes;
    return o;
}

bool canGather(const LightvolSource& src, const LightvolSettings& cfg) {
    return cfg.bounces && src.bvh && src.mesh && src.mesh->hasLightmap() && src.atlasS && !src.atlasS->empty() && src.atlasSize > 0 && src.pages > 0;
}
}  // namespace

void bakeProbesCpu(LightvolGrid& grid, const std::vector<char>& solid, std::vector<char>& baked, size_t first, size_t count, const LightvolSource& src,
                   const LightvolSettings& cfg) {
    if (!src.lights || first >= grid.count()) return;
    if (baked.size() != grid.count()) baked.assign(grid.count(), 0);
    const size_t last = std::min(grid.count(), first + count);
    const std::vector<Vec3> normals = sphereNormals(cfg.normals);
    const size_t K = normals.size();
    std::vector<size_t> ids;
    for (size_t i = first; i < last; ++i) {
        baked[i] = 1;
        grid.probes[i] = Probe{};
        if (i >= solid.size() || !solid[i]) ids.push_back(i);
    }
    if (ids.empty()) return;
    TexelSamples s;
    s.atlas = src.atlasSize;
    s.pageCount = src.pages;
    for (size_t i : ids) {
        const Vec3 p = grid.position(i);
        for (const Vec3& n : normals) {
            s.pos.insert(s.pos.end(), {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z), 1.f});
            s.nrm.insert(s.nrm.end(), {static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z), 0.f});
        }
    }
    s.texel.assign(ids.size() * K, 0u);
    const BakeOptions o = probeOptions(src, cfg);
    const std::vector<float> direct = evaluateDirectCpu(s, *src.lights, o.shadows ? src.bvh : nullptr, o);
    std::vector<float> indirect;
    const bool gather = canGather(src, cfg);
    if (gather) indirect = gatherCpu(s, *src.mesh, *src.bvh, *src.atlasS, cfg.albedo, cfg.giSamples, 0);
    for (size_t j = 0; j < ids.size(); ++j) fitFrom(grid, ids[j], normals, direct.data(), gather ? indirect.data() : nullptr, j * K, cfg.bounceDirectionWeight);
}

struct ProbeBaker::Impl {
    LightvolGrid grid;
    std::vector<char> solid, baked;
    LightvolSource src;
    LightvolSettings cfg;
    std::vector<Vec3> normals;
    std::vector<size_t> ids;  // valid (non solid) probes in grid order = the resident sample order / K
    size_t next = 0;
    std::unique_ptr<gpu::LiveSession> session;
    bool gather = false;
    std::string device = "cpu", gpuError;

    void makeSession() {
        if (!cfg.useGpu || ids.empty() || !src.lights || !src.mesh || src.mesh->empty()) return;
        gather = canGather(src, cfg);
        const BakeOptions o = probeOptions(src, cfg);
        if (!gather && !gpu::needsRayTracing(*src.lights, src.mesh, o)) return;  // nothing to trace: the CPU does the plain light math
        const size_t K = normals.size();
        TexelSamples resident;
        resident.atlas = gather ? src.atlasSize : 1;
        resident.pageCount = gather ? src.pages : 1;
        const size_t atlasTexels = static_cast<size_t>(resident.atlas) * resident.atlas * resident.pageCount;
        resident.pos.reserve(ids.size() * K * 4);
        resident.nrm.reserve(ids.size() * K * 4);
        resident.texel.reserve(ids.size() * K);
        resident.seed.reserve(ids.size() * K);
        uint32_t id = 0;
        for (size_t i : ids) {
            const Vec3 p = grid.position(i);
            for (const Vec3& n : normals) {
                resident.pos.insert(resident.pos.end(), {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z), 1.f});
                resident.nrm.insert(resident.nrm.end(), {static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z), 0.f});
                // the texel only seeds the gather's direction lattice (the session never writes the resident atlas for them): any index inside the atlas
                resident.texel.push_back(static_cast<uint32_t>(id % atlasTexels));
                resident.seed.push_back(id++);
            }
        }
        gpu::LiveSessionConfig sc;
        sc.areaSamples = cfg.areaSamples;
        sc.skySamples = cfg.skySamples;
        sc.giSamples = cfg.giSamples;
        sc.superSamples = 1;
        sc.albedo = std::min(cfg.albedo, kMaxLiveAlbedo);
        sc.gather = gather;
        static const std::vector<float> none;
        std::string error;
        session = gpu::createLiveSession(resident, *src.mesh, *src.lights, sc, gather ? *src.atlasS : none, error);
        if (!session) { gpuError = error; return; }
        device = session->device();
    }

    // One GPU submission for the probes in grid range [first, last); false = the GPU failed (the caller finishes on the CPU).
    bool stepGpu(size_t first, size_t last) {
        const size_t K = normals.size();
        const size_t a = static_cast<size_t>(std::lower_bound(ids.begin(), ids.end(), first) - ids.begin());
        const size_t b = static_cast<size_t>(std::lower_bound(ids.begin(), ids.end(), last) - ids.begin());
        if (b > a) {
            gpu::LiveSession::Job job;
            job.first = static_cast<uint32_t>(a * K);
            job.count = static_cast<uint32_t>((b - a) * K);
            std::vector<float> d, db, ind;
            if (!session->run({job}, d, db, gather ? &ind : nullptr)) { gpuError = session->error(); return false; }
            for (size_t j = a; j < b; ++j) fitFrom(grid, ids[j], normals, d.data(), gather ? ind.data() : nullptr, (j - a) * K, cfg.bounceDirectionWeight);
        }
        return true;
    }
};

ProbeBaker::ProbeBaker(LightvolGrid grid, std::vector<char> solid, const LightvolSource& src, const LightvolSettings& settings) : d_(new Impl) {
    d_->grid = std::move(grid);
    d_->solid = std::move(solid);
    d_->solid.resize(d_->grid.count(), 0);
    d_->baked.assign(d_->grid.count(), 0);
    d_->src = src;
    d_->cfg = settings;
    d_->normals = sphereNormals(settings.normals);
    for (size_t i = 0; i < d_->grid.count(); ++i) if (!d_->solid[i]) d_->ids.push_back(i);
    d_->makeSession();
}

ProbeBaker::~ProbeBaker() = default;
size_t ProbeBaker::total() const { return d_->grid.count(); }
size_t ProbeBaker::baked() const { return d_->next; }
bool ProbeBaker::onGpu() const { return d_->session != nullptr; }
const std::string& ProbeBaker::device() const { return d_->device; }
const std::string& ProbeBaker::gpuError() const { return d_->gpuError; }
const LightvolGrid& ProbeBaker::grid() const { return d_->grid; }
LightvolGrid ProbeBaker::take() { return std::move(d_->grid); }

size_t ProbeBaker::step(size_t probes) {
    Impl& s = *d_;
    if (s.next >= s.grid.count()) return 0;
    const size_t n = std::max<size_t>(1, s.session ? std::max<size_t>(probes, 4096) : probes);
    const size_t first = s.next, last = std::min(s.grid.count(), first + n);
    for (size_t i = first; i < last; ++i) { s.grid.probes[i] = Probe{}; s.baked[i] = 1; }
    if (s.session && !s.stepGpu(first, last)) {  // device lost / out of memory: finish on the CPU
        s.session.reset();
        s.device = "cpu";
    }
    if (!s.session) {
        LightvolSettings cpu = s.cfg;
        cpu.useGpu = false;
        std::vector<char> baked(s.grid.count(), 0);
        bakeProbesCpu(s.grid, s.solid, baked, first, last - first, s.src, cpu);
    }
    s.next = last;
    return last - first;
}

std::vector<EntityLight> entityLights(const std::vector<LightSurface>& surfaces, const LightingClasses& classes, const LightvolGrid& grid) {
    std::map<std::pair<std::string, std::string>, Aabb> bounds;
    for (const auto& s : surfaces) {
        if (classes.isLightmapped(s.classname)) continue;
        Aabb& b = bounds[{s.instance, s.entityId}];
        for (const Vec3& v : s.verts) b.add(v);
    }
    std::vector<EntityLight> out;
    for (const auto& [key, b] : bounds) {
        EntityLight e;
        e.instance = key.first;
        e.entityId = key.second;
        e.centre = b.center();
        e.probe = grid.sample(e.centre);
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace core
