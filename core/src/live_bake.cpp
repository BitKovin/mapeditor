#include "core/live_bake.h"
#include "core/relief_geometry.h"
#include "core/smoothing.h"
#include <cstdlib>
#include <unordered_map>
#include "core/parallel.h"
#include "core/denoise.h"
#include "gpu_bake.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <set>
#include <tuple>

namespace core {

namespace {

uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}
int64_t q(double v, double scale) { return static_cast<int64_t>(std::llround(v * scale)); }

// Identity of a texel independent of how the atlas was packed: where it is in the world and which way it faces.
uint64_t texelKey(const Vec3& p, const Vec3& n) {
    uint64_t h = 1469598103934665603ull;
    h = mix(h, static_cast<uint64_t>(q(p.x, 1000)));
    h = mix(h, static_cast<uint64_t>(q(p.y, 1000)));
    h = mix(h, static_cast<uint64_t>(q(p.z, 1000)));
    h = mix(h, static_cast<uint64_t>(q(n.x, 64)));
    h = mix(h, static_cast<uint64_t>(q(n.y, 64)));
    h = mix(h, static_cast<uint64_t>(q(n.z, 64)));
    return h;
}

// World box a light can change; nullopt = it affects the whole scene.
std::optional<Aabb> influence(const Light& l) {
    if (l.type == LightType::Directional || l.type == LightType::Sky) return std::nullopt;
    Aabb a;
    a.add(l.position);
    return a.expanded(l.radius);
}

bool sameLight(const Light& a, const Light& b) {
    auto eq = [](const Vec3& x, const Vec3& y) { return x.x == y.x && x.y == y.y && x.z == y.z; };
    return a.type == b.type && eq(a.position, b.position) && eq(a.forward, b.forward) && eq(a.right, b.right) && eq(a.up, b.up) && eq(a.color, b.color) &&
           a.brightness == b.brightness && a.radius == b.radius && a.bounce == b.bounce && a.castShadows == b.castShadows && a.style == b.style &&
           a.sourceSize == b.sourceSize && a.innerAngle == b.innerAngle && a.outerAngle == b.outerAngle && a.width == b.width && a.height == b.height &&
           a.sky == b.sky;
}

std::string lightKey(const Light& l) { return l.instance + "|" + l.id; }

}  // namespace

LiveBaker::LiveBaker(const LiveSettings& settings) : cfg_(settings), gpuBudget_(settings.gpuTexelsPerStep) {}
LiveBaker::~LiveBaker() = default;

void LiveBaker::setSessionFactory(SessionFactory f) {
    factory_ = std::move(f);
    session_.reset();
    gpuDisabled_ = false;
}

size_t LiveBaker::staleChunks() const {
    size_t n = 0;
    for (const auto& c : chunks_) n += c.passes == 0;
    return n;
}
bool LiveBaker::chunkHasData(int i) const {
    const LiveChunk& c = chunks_[static_cast<size_t>(i)];
    if (c.passes > 0) return true;
    for (uint32_t s : c.samples)
        for (size_t ch = 0; ch < 3; ++ch)
            if (direct_[s * 4 + ch] != 0.f || indirect_[s * 4 + ch] != 0.f) return true;
    return false;
}
int LiveBaker::minPasses() const {
    int m = cfg_.maxPasses;
    for (const auto& c : chunks_) m = std::min(m, c.passes);
    return m;
}
bool LiveBaker::converged() const {
    for (const auto& c : chunks_) if (c.passes < cfg_.maxPasses) return false;
    return true;
}
Vec3 LiveBaker::value(size_t i) const {
    return {direct_[i * 4] + indirect_[i * 4], direct_[i * 4 + 1] + indirect_[i * 4 + 1], direct_[i * 4 + 2] + indirect_[i * 4 + 2]};
}

Vec3 LiveBaker::shownValue(size_t i) const {
    const size_t page = samples_.texel[i] / (static_cast<size_t>(samples_.atlas) * samples_.atlas);
    const double k = aoFactor(i);
    if (page < pageState_.size() && pageState_[page] == 1 && indirectDn_.size() == samples_.count() * 4)
        return Vec3{direct_[i * 4] + indirectDn_[i * 4], direct_[i * 4 + 1] + indirectDn_[i * 4 + 1], direct_[i * 4 + 2] + indirectDn_[i * 4 + 2]} * k;
    return value(i) * k;
}

float LiveBaker::aoFactor(size_t i) const {
    const AoSettings& ao = lights_.level.ao;
    if (!ao.active() || i >= occ_.size() || std::isnan(occ_[i])) return 1.f;
    const double open = std::pow(std::clamp(1.0 - ao.strength * static_cast<double>(occ_[i]), 0.0, 1.0), ao.power);
    return static_cast<float>(1.0 - ao.intensity * (1.0 - open));
}

// Hardware ray traced occlusion on the resident GPU session (shaders/lightmap_ao.comp): every chunk that has a sample without occlusion is traced
// whole (one contiguous range), only the unknown samples take the result.
bool LiveBaker::occlusionOnGpu(const std::vector<int>& chunkIds) {
    if (!cfg_.useGpu || gpuDisabled_ || !prepareGpu()) return false;
    std::vector<gpu::LiveSession::Job> jobs;
    std::vector<int> traced;
    for (int id : chunkIds) {
        const LiveChunk& c = chunks_[static_cast<size_t>(id)];
        bool unknown = false;
        for (uint32_t s : c.samples) if (s < occ_.size() && std::isnan(occ_[s])) { unknown = true; break; }
        if (!unknown) continue;
        gpu::LiveSession::Job job;
        job.first = chunkFirst_[static_cast<size_t>(id)];
        job.count = static_cast<uint32_t>(c.samples.size());
        jobs.push_back(job);
        traced.push_back(id);
    }
    if (jobs.empty()) return true;
    std::vector<float> occ;
    if (!session_->occlusion(jobs, lights_.level.ao.radius, lights_.level.ao.rays, lights_.level.ao.falloff, occ)) {
        if (!session_->error().empty()) {  // the device failed (not just "this session cannot"): the light falls back to the CPU too
            gpuError_ = session_->error();
            gpuDisabled_ = true;
            session_.reset();
        }
        return false;
    }
    size_t at = 0;
    for (int id : traced)
        for (uint32_t s : chunks_[static_cast<size_t>(id)].samples) {
            if (s < occ_.size() && std::isnan(occ_[s])) occ_[s] = std::clamp(occ[at], 0.f, 1.f);
            ++at;
        }
    aoEvaluator_ = session_->device();
    return true;
}

// Like q3map2's "dirt": `_ao_rays` cosine-weighted rays over the hemisphere of the normal; a hit nearer than the radius counts by how near it is
// ((1 - d / radius)^falloff: 1 at the surface, 0 at the radius; falloff 0 = every hit counts 1). Deterministic per sample (stratified directions, turned by a hash of the sample's id). Traced with hardware
// ray queries on the GPU session when there is one, else here on the bvh (same directions, same weights).
void LiveBaker::computeOcclusion(const std::vector<int>& chunkIds) {
    const double radius = lights_.level.ao.radius;
    if (!(radius > 0) || mesh_.empty()) return;
    if (occlusionOnGpu(chunkIds)) return;
    if (!bvh_) return;
    std::vector<uint32_t> todo;
    for (int c : chunkIds)
        for (uint32_t s : chunks_[static_cast<size_t>(c)].samples) if (s < occ_.size() && std::isnan(occ_[s])) todo.push_back(s);
    if (todo.empty()) return;
    aoEvaluator_ = "cpu";
    const int rays = std::max(1, lights_.level.ao.rays);
    const double falloff = lights_.level.ao.falloff;
    constexpr double kPi = 3.14159265358979323846, kGolden = 0.6180339887498949;
    parallelFor(todo.size(), 64, [&](size_t b, size_t e) {
        for (size_t t = b; t < e; ++t) {
            const size_t i = todo[t];
            const Vec3 p{samples_.pos[i * 4], samples_.pos[i * 4 + 1], samples_.pos[i * 4 + 2]};
            const Vec3 n = Vec3{samples_.nrm[i * 4], samples_.nrm[i * 4 + 1], samples_.nrm[i * 4 + 2]}.normalized();
            const Vec3 helper = std::abs(n.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
            const Vec3 tu = n.cross(helper).normalized(), tv = n.cross(tu);
            uint32_t h = samples_.seedOf(i) * 2654435761u;
            h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15;
            const double turn = (h & 0xFFFFFF) / double(0x1000000);
            const Vec3 origin = p + n * kAoBias;
            double occ = 0;
            for (int k = 0; k < rays; ++k) {
                const double u1 = (k + 0.5) / rays, u2 = std::fmod(k * kGolden + turn, 1.0);
                const double r = std::sqrt(u1), phi = 2 * kPi * u2;
                const Vec3 dir = (tu * (r * std::cos(phi)) + tv * (r * std::sin(phi)) + n * std::sqrt(std::max(0.0, 1 - u1))).normalized();
                const double d = bvh_->nearestHit(origin, dir, radius);
                if (d >= 0 && d < radius) occ += falloff == 1 ? 1.0 - d / radius : std::pow(1.0 - d / radius, falloff);
            }
            occ_[i] = static_cast<float>(occ / rays);
        }
    });
}

size_t LiveBaker::denoisedPages() const {
    size_t n = 0;
    for (char c : pageState_) n += c == 1;
    return n;
}

bool LiveBaker::denoisePending() const {
    if (!denoiseWanted()) return false;
    const size_t pages = pageState_.size();
    std::vector<char> ready(pages, 1);
    for (const auto& c : chunks_) if (c.passes < cfg_.maxPasses) ready[static_cast<size_t>(c.page)] = 0;
    for (const auto& c : chunks_) if (pageState_[static_cast<size_t>(c.page)] == 0 && ready[static_cast<size_t>(c.page)]) return true;
    return false;
}

bool LiveBaker::denoiseNext(std::vector<int>* outChunks) {
    DenoiseJob job;
    if (!beginDenoise(job)) return false;
    const denoise::Result r = runDenoise(job, cfg_.denoiseQuality);
    return finishDenoise(job, r, outChunks);
}

bool LiveBaker::beginDenoise(DenoiseJob& job) {
    if (!denoiseWanted()) return false;
    const size_t pages = pageState_.size();
    std::vector<char> ready(pages, 1);
    for (const auto& c : chunks_) if (c.passes < cfg_.maxPasses) ready[static_cast<size_t>(c.page)] = 0;
    int page = -1;
    for (const auto& c : chunks_)
        if (pageState_[static_cast<size_t>(c.page)] == 0 && ready[static_cast<size_t>(c.page)]) { page = c.page; break; }
    if (page < 0) return false;
    const denoise::Info info = denoise::probe();
    if (!info.available) {
        denoiseError_ = info.error;
        denoiseUnavailable_ = true;  // loading the library is not retried every tick
        return false;
    }
    // The denoiser only needs the atlas layout and where each value sits in it. On the fine grid the values are the grid entries (fine atlas).
    const bool fine = fineGi();
    job = DenoiseJob{};
    job.page = page;
    job.fine = fine;
    job.geometry = geometryGen_;
    job.ps.atlas = fine ? gi_.s.atlas : samples_.atlas;
    job.ps.pageCount = samples_.pageCount;
    for (size_t ci = 0; ci < chunks_.size(); ++ci) {
        if (chunks_[ci].page != page) continue;
        job.ids.push_back(static_cast<int>(ci));
        for (uint32_t s : chunks_[ci].samples) {
            if (!fine) { job.ps.texel.push_back(samples_.texel[s]); job.idx.push_back(s); continue; }
            for (uint32_t e = gi_.begin[s]; e < gi_.begin[s + 1]; ++e) { job.ps.texel.push_back(gi_.s.texel[e]); job.idx.push_back(e); }
        }
    }
    const std::vector<float>& source = fine ? indirectHi_ : indirect_;
    job.vals.assign(job.idx.size() * 4, 0.f);
    for (size_t j = 0; j < job.idx.size(); ++j)
        for (size_t c = 0; c < 3; ++c) job.vals[j * 4 + c] = source[static_cast<size_t>(job.idx[j]) * 4 + c];
    pageState_[static_cast<size_t>(page)] = 3;
    return true;
}

denoise::Result LiveBaker::runDenoise(DenoiseJob& job, int quality) {
    // A page without any indirect light (e.g. a style layer whose lights do not bounce here) stays exactly black: the filter would add a faint haze.
    if (std::all_of(job.vals.begin(), job.vals.end(), [](float v) { return v == 0.f; })) { denoise::Result r; r.ok = true; return r; }
    return denoise::denoiseSamples(job.ps, job.vals, static_cast<denoise::Quality>(std::clamp(quality, 0, 2)), 8);
}

bool LiveBaker::finishDenoise(DenoiseJob& job, const denoise::Result& r, std::vector<int>* outChunks) {
    const size_t page = static_cast<size_t>(job.page);
    if (job.geometry != geometryGen_ || page >= pageState_.size() || pageState_[page] != 3) return false;  // refined or replaced meanwhile: the page is pending again
    if (!r.ok) {
        denoiseError_ = r.error;
        pageState_[page] = 2;
        return false;
    }
    denoiseError_.clear();
    denoiseSeconds_ += r.seconds;
    const std::vector<uint32_t>& idx = job.idx;
    const std::vector<float>& vals = job.vals;
    if (indirectDn_.size() != samples_.count() * 4) indirectDn_.assign(samples_.count() * 4, 0.f);
    if (job.fine) {
        if (indirectHiDn_.size() != gi_.count() * 4) indirectHiDn_.assign(gi_.count() * 4, 0.f);
        for (size_t j = 0; j < idx.size(); ++j)
            for (size_t c = 0; c < 3; ++c) indirectHiDn_[static_cast<size_t>(idx[j]) * 4 + c] = vals[j * 4 + c];
        for (int id : job.ids)  // back to the texels: mean of each sample's denoised grid entries
            for (uint32_t s : chunks_[static_cast<size_t>(id)].samples) {
                const uint32_t a = gi_.begin[s], b = gi_.begin[s + 1];
                for (size_t c = 0; c < 3; ++c) {
                    float acc = 0.f;
                    for (uint32_t e = a; e < b; ++e) acc += indirectHiDn_[static_cast<size_t>(e) * 4 + c];
                    indirectDn_[static_cast<size_t>(s) * 4 + c] = b > a ? acc / static_cast<float>(b - a) : 0.f;
                }
            }
    } else {
        for (size_t j = 0; j < idx.size(); ++j)
            for (size_t c = 0; c < 3; ++c) indirectDn_[static_cast<size_t>(idx[j]) * 4 + c] = vals[j * 4 + c];
    }
    pageState_[page] = 1;
    for (int id : job.ids) chunkDirty_[static_cast<size_t>(id)] = 1;  // shown with the denoised light now
    if (outChunks) *outChunks = job.ids;
    return true;
}

size_t LiveBaker::giEntryCount(const LiveChunk& c) const {
    size_t n = 0;
    for (uint32_t s : c.samples) n += gi_.begin[s + 1] - gi_.begin[s];
    return n;
}

TexelSamples LiveBaker::giSamplesOf(const LiveChunk& c) const {
    TexelSamples out;
    out.atlas = gi_.s.atlas;
    out.pageCount = gi_.s.pageCount;
    for (uint32_t s : c.samples)
        for (uint32_t e = gi_.begin[s]; e < gi_.begin[s + 1]; ++e) {
            out.pos.insert(out.pos.end(), gi_.s.pos.begin() + e * 4, gi_.s.pos.begin() + e * 4 + 4);
            out.nrm.insert(out.nrm.end(), gi_.s.nrm.begin() + e * 4, gi_.s.nrm.begin() + e * 4 + 4);
            out.texel.push_back(gi_.s.texel[e]);
        }
    return out;
}

void LiveBaker::accumulateFine(size_t sample, const float* values, size_t& cursor, float k) {
    const uint32_t a = gi_.begin[sample], b = gi_.begin[sample + 1];
    for (uint32_t e = a; e < b; ++e, ++cursor)
        for (size_t ch = 0; ch < 3; ++ch) {
            float& I = indirectHi_[static_cast<size_t>(e) * 4 + ch];
            I += (values[cursor * 4 + ch] - I) * k;
        }
    if (b <= a) return;
    for (size_t ch = 0; ch < 3; ++ch) {
        float acc = 0.f;
        for (uint32_t e = a; e < b; ++e) acc += indirectHi_[static_cast<size_t>(e) * 4 + ch];
        indirect_[sample * 4 + ch] = acc / static_cast<float>(b - a);
    }
}

void LiveBaker::refreshIndirect(const LiveChunk& c) {
    for (uint32_t s : c.samples) {
        const uint32_t a = gi_.begin[s], b = gi_.begin[s + 1];
        if (b <= a) continue;
        for (size_t ch = 0; ch < 3; ++ch) {
            float acc = 0.f;
            for (uint32_t e = a; e < b; ++e) acc += indirectHi_[static_cast<size_t>(e) * 4 + ch];
            indirect_[static_cast<size_t>(s) * 4 + ch] = acc / static_cast<float>(b - a);
        }
    }
}

void LiveBaker::rebuildAtlasSource() {
    const size_t perPage = static_cast<size_t>(samples_.atlas) * samples_.atlas;
    atlasS_.assign(perPage * static_cast<size_t>(samples_.pageCount) * 4, 0.f);
    for (size_t i = 0; i < samples_.count(); ++i) {
        const size_t t = static_cast<size_t>(samples_.texel[i]) * 4;
        for (size_t c = 0; c < 3; ++c) atlasS_[t + c] = db_[i * 4 + c] + indirect_[i * 4 + c];
        atlasS_[t + 3] = 1.f;
    }
}

void LiveBaker::setGeometry(const LightmapLayout& layout, const std::vector<LightSurface>& surfaces, const std::vector<Aabb>& changedRegions,
                            std::shared_ptr<const SolidSet> solids) {
    using Clock = std::chrono::steady_clock;
    auto since = [](Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); };
    const auto tStart = Clock::now();
    GeometryTimings gt;
    ++geometryGen_;
    gpuDisabled_ = false;  // new geometry may bring the GPU path back (e.g. the first shadow casters)
    session_.reset();      // the resident samples, occluders and atlas belong to the old geometry

    // What we know about the old texels. Moved, not copied: they are replaced below.
    const TexelSamples oldSamples = std::move(samples_);
    const std::vector<float> oldD = std::move(direct_), oldDb = std::move(db_), oldI = std::move(indirect_), oldDn = std::move(indirectDn_);
    const std::vector<float> oldOcc = std::move(occ_);
    const std::vector<int32_t> oldAt = std::move(sampleAt_);
    const std::vector<char> oldPageState = pageState_;
    std::vector<int> oldPasses(oldSamples.count(), 0);
    std::vector<char> oldUnsent(oldSamples.count(), 0);  // inside a chunk whose tile the client has not received yet
    for (size_t ci = 0; ci < chunks_.size(); ++ci) {
        const bool unsent = ci < chunkDirty_.size() && chunkDirty_[ci];
        for (uint32_t smp : chunks_[ci].samples) { oldPasses[smp] = chunks_[ci].passes; oldUnsent[smp] = unsent; }
    }
    gt.inherit = since(tStart);

    layout_ = layout;
    auto t1 = Clock::now();
    samples_ = texelCache_.build(layout_, surfaces, solids.get());
    gt.samples = since(t1);
    gt.samplesGenerate = texelCache_.stats().generateSeconds;
    gt.samplesAssemble = texelCache_.stats().assembleSeconds;
    t1 = Clock::now();
    mesh_ = buildOccluders(surfaces, cfg_.classes, &layout_);
    attachAlbedo(mesh_, cfg_.albedoLoader);
    gt.occluders = since(t1);
    t1 = Clock::now();
    bvh_ = std::make_unique<TriangleBvh>(mesh_);
    gt.bvh = since(t1);

    // Inheritance. A texel keeps its result (and pass count) when the old set had a texel at the same world position facing the same way. The
    // layout is usually stable, so the texel at the same atlas slot is tried first (no hashing); what does not match goes through a content key
    // lookup restricted to the old texels nobody took over (a moved crate: a few hundred, not the whole level).
    t1 = Clock::now();
    const size_t n = samples_.count();
    direct_.assign(n * 4, 0.f);
    db_.assign(n * 4, 0.f);
    indirect_.assign(n * 4, 0.f);
    occ_.assign(n, std::numeric_limits<float>::quiet_NaN());
    std::vector<int> inherited(n, -1);
    std::vector<int32_t> from(n, -1);   // old sample each new one took over, -1 = none
    std::vector<char> fresh(n, 0);      // the pixels of this texel are not what the client holds for its slot
    auto keyOf = [](const TexelSamples& s, size_t i) {
        return texelKey({s.pos[i * 4], s.pos[i * 4 + 1], s.pos[i * 4 + 2]}, {s.nrm[i * 4], s.nrm[i * 4 + 1], s.nrm[i * 4 + 2]});
    };
    auto same = [&](size_t i, size_t o) {
        if (std::memcmp(&samples_.pos[i * 4], &oldSamples.pos[o * 4], 3 * sizeof(float)) == 0 && std::memcmp(&samples_.nrm[i * 4], &oldSamples.nrm[o * 4], 3 * sizeof(float)) == 0) return true;
        return keyOf(samples_, i) == keyOf(oldSamples, o);
    };
    const bool sameAtlas = oldSamples.atlas == samples_.atlas && oldSamples.pageCount == samples_.pageCount;
    std::vector<char> taken(oldSamples.count(), 0);
    for (size_t i = 0; i < n; ++i) {
        if (!sameAtlas) break;
        const uint32_t t = samples_.texel[i];
        if (t >= oldAt.size() || oldAt[t] < 0) continue;
        const size_t o = static_cast<size_t>(oldAt[t]);
        if (taken[o] || !same(i, o)) continue;
        taken[o] = 1;
        from[i] = static_cast<int32_t>(o);
    }
    std::vector<size_t> missing;
    for (size_t i = 0; i < n; ++i) if (from[i] < 0) missing.push_back(i);
    if (!missing.empty() && oldSamples.count()) {
        std::unordered_map<uint64_t, size_t> leftovers;
        for (size_t o = 0; o < oldSamples.count(); ++o) if (!taken[o]) leftovers.emplace(keyOf(oldSamples, o), o);
        for (size_t i : missing) {
            const auto it = leftovers.find(keyOf(samples_, i));
            if (it == leftovers.end()) continue;
            from[i] = static_cast<int32_t>(it->second);
            fresh[i] = 1;  // same light, other place in the atlas
        }
    }
    std::vector<char> pageTouched(static_cast<size_t>(std::max(0, samples_.pageCount)), 0);  // some texel of the page is new or moved
    const size_t perPage = static_cast<size_t>(samples_.atlas) * samples_.atlas;
    for (size_t i = 0; i < n; ++i) {
        const int32_t o = from[i];
        if (o < 0) fresh[i] = 1;
        else {
            for (size_t c = 0; c < 4; ++c) {
                direct_[i * 4 + c] = oldD[static_cast<size_t>(o) * 4 + c];
                db_[i * 4 + c] = oldDb[static_cast<size_t>(o) * 4 + c];
                indirect_[i * 4 + c] = oldI[static_cast<size_t>(o) * 4 + c];
            }
            inherited[i] = oldPasses[static_cast<size_t>(o)];
            if (static_cast<size_t>(o) < oldOcc.size()) occ_[i] = oldOcc[static_cast<size_t>(o)];
            if (oldUnsent[static_cast<size_t>(o)]) fresh[i] = 1;
        }
        if (fresh[i]) pageTouched[samples_.texel[i] / perPage] = 1;
    }
    gt.inherit += since(t1);

    // Extrapolation: a texel the edit created (a new face, a moved wall, a freshly carved opening) has no old value of its own. Showing black until it is
    // baked makes every edit flash dark, so it starts from the light of the nearest old texel that faced about the same way (the same surface a moment
    // ago, or the one next to it). It stays stale (inherited = -1): the first pass replaces the guess. A rough guess beats black.
    if (oldSamples.count() > 0) {
        constexpr double kCell = 0.5;   // m
        constexpr int kMaxRing = 12;    // cells: 6 m
        auto cellOf = [&](double v) { return static_cast<int64_t>(std::floor(v / kCell)); };
        auto hashCell = [](int64_t x, int64_t y, int64_t z) { return (x * 73856093) ^ (y * 19349663) ^ (z * 83492791); };
        std::unordered_map<int64_t, std::vector<uint32_t>> grid;
        grid.reserve(oldSamples.count());
        for (size_t o = 0; o < oldSamples.count(); ++o)
            grid[hashCell(cellOf(oldSamples.pos[o * 4]), cellOf(oldSamples.pos[o * 4 + 1]), cellOf(oldSamples.pos[o * 4 + 2]))].push_back(static_cast<uint32_t>(o));
        for (size_t i = 0; i < n; ++i) {
            if (inherited[i] >= 0) continue;
            const double px = samples_.pos[i * 4], py = samples_.pos[i * 4 + 1], pz = samples_.pos[i * 4 + 2];
            const double nx = samples_.nrm[i * 4], ny = samples_.nrm[i * 4 + 1], nz = samples_.nrm[i * 4 + 2];
            const int64_t cx = cellOf(px), cy = cellOf(py), cz = cellOf(pz);
            double best = 1e300, bestAnyD = 1e300;
            int64_t bestOld = -1, bestAny = -1;  // bestAny: nearest of any orientation, the last resort (a new wall has no old neighbour facing its way)
            int found = -1;  // ring in which the first candidate turned up: one more ring is searched (a closer one may sit in the next shell)
            for (int ring = 0; ring <= kMaxRing && (found < 0 || ring <= found + 1); ++ring) {
                for (int64_t dx = -ring; dx <= ring; ++dx)
                    for (int64_t dy = -ring; dy <= ring; ++dy)
                        for (int64_t dz = -ring; dz <= ring; ++dz) {
                            if (std::max({std::llabs(dx), std::llabs(dy), std::llabs(dz)}) != ring) continue;
                            const auto it = grid.find(hashCell(cx + dx, cy + dy, cz + dz));
                            if (it == grid.end()) continue;
                            for (uint32_t o : it->second) {
                                const double dot = nx * oldSamples.nrm[o * 4] + ny * oldSamples.nrm[o * 4 + 1] + nz * oldSamples.nrm[o * 4 + 2];
                                const double ex = px - oldSamples.pos[o * 4], ey = py - oldSamples.pos[o * 4 + 1], ez = pz - oldSamples.pos[o * 4 + 2];
                                const double d2 = ex * ex + ey * ey + ez * ez;
                                if (d2 < bestAnyD) { bestAnyD = d2; bestAny = static_cast<int64_t>(o); }
                                if (dot < 0.7) continue;  // another way of facing: its light says little about this surface
                                if (d2 < best) { best = d2; bestOld = static_cast<int64_t>(o); if (found < 0) found = ring; }
                            }
                        }
            }
            if (bestOld < 0) bestOld = bestAny;
            if (bestOld < 0) continue;
            for (size_t c = 0; c < 4; ++c) {
                direct_[i * 4 + c] = oldD[static_cast<size_t>(bestOld) * 4 + c];
                db_[i * 4 + c] = oldDb[static_cast<size_t>(bestOld) * 4 + c];
                indirect_[i * 4 + c] = oldI[static_cast<size_t>(bestOld) * 4 + c];
            }
        }
    }

    // the fine GI grid: its entries start from the (inherited) value of their texel
    t1 = Clock::now();
    gi_ = cfg_.bounces && mesh_.hasLightmap() ? buildGiSamples(samples_, cfg_.giScale) : GiSamples{};
    indirectHi_.clear();
    indirectHiDn_.clear();
    if (fineGi()) {
        indirectHi_.assign(gi_.count() * 4, 0.f);
        for (size_t e = 0; e < gi_.count(); ++e)
            for (size_t c = 0; c < 4; ++c) indirectHi_[e * 4 + c] = indirect_[static_cast<size_t>(gi_.owner[e]) * 4 + c];
    }
    gt.gi = since(t1);

    // chunks: a direct table (page, tile x, tile y) -> chunk, in first-encounter order
    t1 = Clock::now();
    chunks_.clear();
    chunkDirty_.clear();
    const int atlas = samples_.atlas;
    const int edge = std::max(1, cfg_.chunkTexels);
    const size_t tilesPerRow = static_cast<size_t>((atlas + edge - 1) / edge);
    std::vector<int32_t> chunkAt(static_cast<size_t>(std::max(0, samples_.pageCount)) * tilesPerRow * tilesPerRow, -1);
    for (size_t i = 0; i < n; ++i) {
        const size_t page = samples_.texel[i] / perPage, local = samples_.texel[i] % perPage;
        const int x = static_cast<int>(local % static_cast<size_t>(atlas)), y = static_cast<int>(local / static_cast<size_t>(atlas));
        int32_t& slot = chunkAt[(page * tilesPerRow + static_cast<size_t>(y / edge)) * tilesPerRow + static_cast<size_t>(x / edge)];
        if (slot < 0) {
            slot = static_cast<int32_t>(chunks_.size());
            LiveChunk c;
            c.page = static_cast<int>(page);
            c.tx = x / edge;
            c.ty = y / edge;
            c.passes = cfg_.maxPasses;  // lowered below to the minimum over its samples
            chunks_.push_back(std::move(c));
            chunkDirty_.push_back(0);
        }
        LiveChunk& c = chunks_[static_cast<size_t>(slot)];
        c.samples.push_back(static_cast<uint32_t>(i));
        c.bounds.add({samples_.pos[i * 4], samples_.pos[i * 4 + 1], samples_.pos[i * 4 + 2]});
        c.passes = inherited[i] < 0 ? 0 : std::min(c.passes, inherited[i]);
        if (fresh[i]) chunkDirty_[static_cast<size_t>(slot)] = 1;
    }
    gt.chunks = since(t1);

    t1 = Clock::now();
    // Denoised copies: texels that took over a result take over its denoised indirect light too, and a page whose texels are all unchanged stays
    // denoised (a moved crate must not cost a denoise run of every page).
    pageState_.assign(static_cast<size_t>(std::max(0, samples_.pageCount)), 0);
    indirectDn_.clear();
    if (sameAtlas && oldDn.size() == oldSamples.count() * 4) {
        bool any = false;
        for (size_t pg = 0; pg < pageState_.size() && pg < oldPageState.size(); ++pg)
            if (oldPageState[pg] == 1 && !pageTouched[pg]) { pageState_[pg] = 1; any = true; }
        if (any) {
            indirectDn_.assign(n * 4, 0.f);
            for (size_t i = 0; i < n; ++i) {
                if (from[i] < 0 || pageState_[samples_.texel[i] / perPage] != 1) continue;
                for (size_t c = 0; c < 4; ++c) indirectDn_[i * 4 + c] = oldDn[static_cast<size_t>(from[i]) * 4 + c];
            }
        }
    }
    sampleAt_.assign(static_cast<size_t>(samples_.pageCount) * perPage, -1);
    for (size_t i = 0; i < n; ++i) sampleAt_[samples_.texel[i]] = static_cast<int32_t>(i);
    rebuildAtlasSource();
    gt.atlas = since(t1);

    // edits: stale where the change can show (shadows reach as far as a light does), bounce-refresh around it
    t1 = Clock::now();
    double maxRadius = 0;
    bool globalShadows = false;
    for (const auto& l : lights_.lights) {
        if (!l.castShadows) continue;
        if (l.type == LightType::Directional || l.type == LightType::Sky) globalShadows = true;
        else maxRadius = std::max(maxRadius, l.radius);
    }
    for (const auto& region : changedRegions) {
        if (!region.valid()) continue;
        if (globalShadows) { markAllStale(); break; }
        markStale(region.expanded(maxRadius), true, cfg_.indirectRadius);
    }
    // ambient occlusion near an edit is no longer known: recompute it there (those chunks get at least one more pass)
    if (lights_.level.ao.active())
        for (const auto& region : changedRegions) if (region.valid()) forgetOcclusion(region.expanded(lights_.level.ao.radius));
    gt.stale = since(t1);
    gt.total = since(tStart);
    geomTimings_ = gt;
}

std::vector<int> LiveBaker::takeDirtyChunks() {
    std::vector<int> out;
    for (size_t i = 0; i < chunkDirty_.size(); ++i)
        if (chunkDirty_[i]) { out.push_back(static_cast<int>(i)); chunkDirty_[i] = 0; }
    return out;
}
void LiveBaker::markAllChunksDirty() { std::fill(chunkDirty_.begin(), chunkDirty_.end(), 1); }
bool LiveBaker::hasDirtyChunks() const {
    for (char d : chunkDirty_) if (d) return true;
    return false;
}

void LiveBaker::invalidateSource(LiveChunk& c) {
    c.resetIndirect = true;
    float* gpuAtlas = session_ ? session_->atlas() : nullptr;
    for (uint32_t s : c.samples) {
        const size_t t = static_cast<size_t>(samples_.texel[s]) * 4;
        for (size_t ch = 0; ch < 3; ++ch) {
            atlasS_[t + ch] = 0.f;
            if (gpuAtlas) gpuAtlas[t + ch] = 0.f;
        }
    }
}

bool LiveBaker::setQuality(int maxPasses, int directSupersample) {
    maxPasses = std::max(1, maxPasses);
    directSupersample = std::clamp(directSupersample, 1, kMaxSuperSamples);
    if (maxPasses == cfg_.maxPasses && directSupersample == cfg_.directSupersample) return false;
    session_.reset();  // the resident session was made for the old settings
    if (directSupersample != cfg_.directSupersample) { cfg_.directSupersample = directSupersample; markAllStale(); }
    cfg_.maxPasses = maxPasses;
    for (auto& c : chunks_) c.passes = std::min(c.passes, maxPasses);
    return true;
}

void LiveBaker::forgetOcclusion(const Aabb& box) {
    for (auto& c : chunks_) {
        if (!c.bounds.intersects(box)) continue;
        bool any = false;
        for (uint32_t s : c.samples) {
            const Vec3 p{samples_.pos[s * 4], samples_.pos[s * 4 + 1], samples_.pos[s * 4 + 2]};
            if (box.contains(p)) { occ_[s] = std::numeric_limits<float>::quiet_NaN(); any = true; }
        }
        if (any) c.passes = std::min(c.passes, std::max(0, cfg_.maxPasses - 1));
    }
}

void LiveBaker::markAllStale() {
    for (auto& c : chunks_) {
        c.passes = 0;
        invalidateSource(c);
    }
}

// `direct`: chunks inside go fully stale. Chunks within a gather range (+ `indirectRadius`) of it can only see different bounce light: they drop
// to indirectRefreshPasses. Both kinds lose their bounce source at once and replace (not average) their indirect estimate on the next pass,
// otherwise the old light keeps circulating between the surfaces that still hold it. `indirectRadius` 0 = no bounce refresh at all.
void LiveBaker::markStale(const Aabb& direct, bool directToo, double indirectRadius) {
    const Aabb wide = direct.expanded(indirectRadius > 0 ? indirectRadius + cfg_.bounceRange : 0.0);
    for (auto& c : chunks_) {
        if (directToo && c.bounds.intersects(direct)) {
            c.passes = 0;
            invalidateSource(c);
        } else if (cfg_.bounces && indirectRadius > 0 && c.bounds.intersects(wide)) {
            c.passes = std::min(c.passes, cfg_.indirectRefreshPasses);
            invalidateSource(c);
        }
    }
}

void LiveBaker::setLights(const LightScene& next) {
    gpuDisabled_ = false;
    sessionLightsDirty_ = true;
    if (!haveLights_) {
        lights_ = next;
        haveLights_ = true;
        markAllStale();
        return;
    }
    bool global = (next.ambient - lights_.ambient).length() > 0;
    const AoSettings aoOld = lights_.level.ao, aoNew = next.level.ao;
    if (aoOld != aoNew) {
        const bool retrace = !aoOld.sameTrace(aoNew);
        if (retrace) std::fill(occ_.begin(), occ_.end(), std::numeric_limits<float>::quiet_NaN());
        if (aoNew.active() && retrace) for (auto& c : chunks_) c.passes = std::min(c.passes, std::max(0, cfg_.maxPasses - 1));
        markAllChunksDirty();  // power / intensity / strength: only the shown values change
    }
    std::map<std::string, const Light*> before, after;
    for (const auto& l : lights_.lights) before[lightKey(l)] = &l;
    for (const auto& l : next.lights) after[lightKey(l)] = &l;
    std::vector<const Light*> changed;
    for (const auto& kv : after) {
        const auto it = before.find(kv.first);
        if (it == before.end()) changed.push_back(kv.second);
        else if (!sameLight(*it->second, *kv.second)) { changed.push_back(it->second); changed.push_back(kv.second); }
    }
    for (const auto& kv : before) if (!after.count(kv.first)) changed.push_back(kv.second);
    for (const Light* l : changed) {
        const auto inf = influence(*l);
        if (!inf) { global = true; continue; }
        markStale(*inf, true, l->bounce > 0 ? cfg_.indirectRadius : 0.0);
    }
    lights_ = next;
    if (global) markAllStale();
}

LiveBaker::StepResult LiveBaker::step(const Vec3* camera, size_t texelBudget) {
    StepResult r;
    std::vector<int> order;
    for (size_t i = 0; i < chunks_.size(); ++i) if (chunks_[i].passes < cfg_.maxPasses) order.push_back(static_cast<int>(i));
    auto dist = [&](int i) { return camera ? (chunks_[static_cast<size_t>(i)].bounds.center() - *camera).length() : 0.0; };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const LiveChunk &ca = chunks_[static_cast<size_t>(a)], &cb = chunks_[static_cast<size_t>(b)];
        const bool sa = ca.passes == 0, sb = cb.passes == 0;
        if (sa != sb) return sa;                                  // stale chunks first
        if (!sa && ca.passes != cb.passes) return ca.passes < cb.passes;  // then the least refined
        return dist(a) < dist(b);                                  // nearest to the camera first
    });
    const auto t0 = std::chrono::steady_clock::now();
    if (!order.empty() && cfg_.gpuTexelsPerStep > 0 && prepareGpu()) texelBudget = std::max<size_t>(texelBudget, gpuBudget_);
    std::vector<int> chosen;
    for (int i : order) {
        if (r.texels >= texelBudget && !r.chunks.empty()) break;
        chosen.push_back(i);
        r.chunks.push_back(i);
        r.texels += chunks_[static_cast<size_t>(i)].samples.size();
    }
    if (!chosen.empty()) {
        if (lights_.level.ao.active()) computeOcclusion(chosen);
        refineBatch(chosen);
        for (int i : chosen) {
            pageState_[static_cast<size_t>(chunks_[static_cast<size_t>(i)].page)] = 0;  // refined: its denoised copy is stale
            chunkDirty_[static_cast<size_t>(i)] = 1;
        }
        stats_.stepSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        stats_.texels += r.texels;
        ++stats_.steps;
    }
    return r;
}

TexelSamples LiveBaker::samplesOf(const LiveChunk& c) const {
    TexelSamples sub;
    sub.atlas = samples_.atlas;
    sub.pageCount = samples_.pageCount;
    for (uint32_t idx : c.samples) {
        sub.appendFrom(samples_, idx);
        sub.seed.push_back(idx);
    }
    return sub;
}

BakeOptions LiveBaker::bakeOptions() const {
    BakeOptions o;
    o.shadows = true;
    o.areaSamples = cfg_.areaSamplesPerPass;
    o.skySamples = cfg_.skySamplesPerPass;
    o.superSamples = cfg_.directSupersample;
    o.denoiseGi = false;  // the live baker denoises its own estimate (denoiseNext), never a single pass
    o.classes = cfg_.classes;
    o.gpuValidation = cfg_.gpuValidation;
    return o;
}

// Makes sure the resident GPU session exists when it can be used. False: this step runs on the CPU (and `gpuError_` says why when the
// GPU was expected).
bool LiveBaker::prepareGpu() {
    if (session_) return true;
    if (!cfg_.useGpu || gpuDisabled_ || mesh_.empty() || chunks_.empty() || !haveLights_) return false;
    const bool gather = cfg_.bounces && mesh_.hasLightmap();
    // nothing to trace (no bounces, no shadows, no ambient occlusion): the CPU does the plain light math
    if (!gather && !gpu::needsRayTracing(lights_, &mesh_, bakeOptions()) && !lights_.level.ao.active()) return false;
    const auto tSession = std::chrono::steady_clock::now();
    // chunk by chunk, so every chunk is one contiguous range of the resident samples. Offsets first (prefix sums), then every chunk fills its own
    // range, on all cores: appending sample by sample cost more than the light on a big level.
    TexelSamples resident;
    resident.atlas = samples_.atlas;
    resident.pageCount = samples_.pageCount;
    const size_t nc = chunks_.size();
    chunkFirst_.assign(nc, 0);
    std::vector<size_t> baseOff(nc + 1, 0), giOff(nc + 1, 0);
    for (size_t c = 0; c < nc; ++c) {
        chunkFirst_[c] = static_cast<uint32_t>(baseOff[c]);
        baseOff[c + 1] = baseOff[c] + chunks_[c].samples.size();
    }
    const size_t n = baseOff[nc];
    const bool withGi = fineGi();  // behind the base samples: the grid entries, chunk by chunk (no sub-texel data, no random id: they only gather)
    if (withGi) {
        giFirst_.assign(nc, 0);
        for (size_t c = 0; c < nc; ++c) {
            size_t count = 0;
            for (uint32_t idx : chunks_[c].samples) count += gi_.begin[idx + 1] - gi_.begin[idx];
            giFirst_[c] = static_cast<uint32_t>(n + giOff[c]);
            giOff[c + 1] = giOff[c] + count;
        }
    }
    const size_t total = n + giOff[nc];
    const bool hasSub = samples_.hasSub();
    resident.pos.resize(total * 4);
    resident.nrm.resize(total * 4);
    resident.texel.resize(total);
    resident.seed.assign(total, 0);  // the random numbers of a sample never depend on where it sits in the resident order (entries: 0)
    if (hasSub) resident.sub.assign(total * 12, 0.f);
    parallelFor(nc, 8, [&](size_t cb, size_t ce) {
        for (size_t c = cb; c < ce; ++c) {
            size_t o = baseOff[c];
            for (uint32_t idx : chunks_[c].samples) {
                std::memcpy(&resident.pos[o * 4], &samples_.pos[static_cast<size_t>(idx) * 4], 4 * sizeof(float));
                std::memcpy(&resident.nrm[o * 4], &samples_.nrm[static_cast<size_t>(idx) * 4], 4 * sizeof(float));
                resident.texel[o] = samples_.texel[idx];
                resident.seed[o] = idx;
                if (hasSub) std::memcpy(&resident.sub[o * 12], &samples_.sub[static_cast<size_t>(idx) * 12], 12 * sizeof(float));
                ++o;
            }
            if (!withGi) continue;
            o = n + giOff[c];
            for (uint32_t idx : chunks_[c].samples)
                for (uint32_t e = gi_.begin[idx]; e < gi_.begin[idx + 1]; ++e, ++o) {
                    std::memcpy(&resident.pos[o * 4], &gi_.s.pos[static_cast<size_t>(e) * 4], 4 * sizeof(float));
                    std::memcpy(&resident.nrm[o * 4], &gi_.s.nrm[static_cast<size_t>(e) * 4], 4 * sizeof(float));
                    resident.texel[o] = gi_.s.texel[e];
                }
        }
    });
    lastResidentSeconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - tSession).count();
    gpu::LiveSessionConfig sc;
    sc.giEntries = gather && fineGi() ? gi_.count() : 0;
    sc.giAtlas = gi_.s.atlas;
    sc.areaSamples = cfg_.areaSamplesPerPass;
    sc.skySamples = cfg_.skySamplesPerPass;
    sc.giSamples = cfg_.giSamplesPerPass;
    sc.superSamples = cfg_.directSupersample;
    sc.albedo = std::min(cfg_.albedo, kMaxLiveAlbedo);
    sc.bounceRange = cfg_.bounceRange;
    sc.gather = gather;
    sc.validation = cfg_.gpuValidation;
    std::string error;
    session_ = factory_ ? factory_(resident, mesh_, lights_, sc, atlasS_, error) : gpu::createLiveSession(resident, mesh_, lights_, sc, atlasS_, error);
    lastSessionSeconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - tSession).count();
    if (!session_) {
        gpuError_ = error;
        gpuDisabled_ = true;  // no device / no ray query / a limit: do not pay for a failed device creation every step
        return false;
    }
    sessionGather_ = gather;
    sessionLightsDirty_ = false;
    if (gpuBudget_ == 0) gpuBudget_ = std::max<size_t>(cfg_.gpuTexelsPerStep, 1024);
    return true;
}

// One step on the resident GPU session: a single submission evaluates direct light and one gather pass for every chosen chunk, then the
// results are merged exactly like the CPU path does (running mean, moving average) and the bounce source is mirrored into the GPU atlas.
// The gather reads the atlas as the previous step left it, so bounce light lags the direct light by one (short) step.
bool LiveBaker::refineOnGpu(const std::vector<int>& ids, int perPass) {
    if (!prepareGpu()) return false;
    if (sessionLightsDirty_) {
        if (!session_->setLights(lights_)) session_.reset();  // more lights than the session was sized for: start over with a bigger one
        sessionLightsDirty_ = false;
        if (!prepareGpu()) return false;
    }
    std::vector<gpu::LiveSession::Job> jobs;
    for (int id : ids) {
        const LiveChunk& c = chunks_[static_cast<size_t>(id)];
        gpu::LiveSession::Job job;
        job.first = chunkFirst_[static_cast<size_t>(id)];
        job.count = static_cast<uint32_t>(c.samples.size());
        job.offset = static_cast<uint32_t>(c.passes) * static_cast<uint32_t>(perPass);
        if (sessionGather_ && fineGi()) {
            job.giFirst = giFirst_[static_cast<size_t>(id)];
            job.giCount = static_cast<uint32_t>(giEntryCount(c));
        }
        jobs.push_back(job);
    }
    std::vector<float> d, db, ind;
    if (!session_->run(jobs, d, db, sessionGather_ ? &ind : nullptr)) {
        gpuError_ = session_->error();
        gpuDisabled_ = true;
        session_.reset();
        return false;
    }
    evaluator_ = session_->device();
    gpuValidation_ = session_->validation();
    const double gpuSeconds = session_->lastGpuSeconds();
    stats_.gpuSeconds += gpuSeconds;
    float* atlasGpu = session_->atlas();
    const bool fine = sessionGather_ && fineGi();
    size_t first = 0, gridAt = 0;
    for (int id : ids) {
        LiveChunk& c = chunks_[static_cast<size_t>(id)];
        const int p = c.passes;
        const float k = p == 0 ? 1.f : 1.f / static_cast<float>(p + 1);  // running mean: new samples join the estimate
        const float kk = (p == 0 || c.resetIndirect) ? 1.f : 1.f / static_cast<float>(std::min(p + 1, std::max(1, cfg_.indirectWindow)));  // moving average
        for (size_t j = 0; j < c.samples.size(); ++j) {
            const size_t s = c.samples[j], t = static_cast<size_t>(samples_.texel[s]) * 4;
            for (size_t ch = 0; ch < 3; ++ch) {
                float& D = direct_[s * 4 + ch];
                float& B = db_[s * 4 + ch];
                D += (d[(first + j) * 4 + ch] - D) * k;
                B += (db[(first + j) * 4 + ch] - B) * k;
                if (sessionGather_ && !fine) {
                    float& I = indirect_[s * 4 + ch];
                    I += (ind[(first + j) * 4 + ch] - I) * kk;
                }
            }
            if (fine) accumulateFine(s, ind.data(), gridAt, kk);
            for (size_t ch = 0; ch < 3; ++ch) atlasS_[t + ch] = db_[s * 4 + ch] + indirect_[s * 4 + ch];
            atlasS_[t + 3] = 1.f;
            for (size_t ch = 0; ch < 4; ++ch) atlasGpu[t + ch] = atlasS_[t + ch];  // write only: never read the (write-combined) GPU copy
        }
        first += c.samples.size();
        c.passes = p + 1;
        c.resetIndirect = false;
    }
    // keep a step near gpuTargetStepMs: long enough to fill the GPU, short enough that the editor stays responsive (and far from a driver timeout)
    const double target = std::clamp(cfg_.gpuTargetStepMs, 5.0, 400.0) / 1000.0;
    const size_t lo = std::max<size_t>(1024, cfg_.gpuTexelsPerStep / 4), hi = cfg_.gpuTexelsPerStep * static_cast<size_t>(std::max(1, cfg_.gpuMaxStepScale));
    if (gpuSeconds > target * 1.5) gpuBudget_ = std::max(lo, gpuBudget_ / 2);
    else if (gpuSeconds < target * 0.6) gpuBudget_ = std::min(hi, gpuBudget_ + gpuBudget_ / 2);
    return true;
}

void LiveBaker::refineBatch(const std::vector<int>& ids) {
    const int perPass = std::max({1, cfg_.areaSamplesPerPass, cfg_.skySamplesPerPass, cfg_.giSamplesPerPass});
    if (refineOnGpu(ids, perPass)) return;
    const BakeOptions o = bakeOptions();
    const TriangleBvh* bvh = bvh_ && bvh_->nodeCount() ? bvh_.get() : nullptr;

    // all chosen chunks in one sample set; every sample keeps its own random id and the pass offset of its chunk
    TexelSamples all;
    all.atlas = samples_.atlas;
    all.pageCount = samples_.pageCount;
    std::vector<size_t> begin;
    for (int id : ids) {
        const LiveChunk& c = chunks_[static_cast<size_t>(id)];
        begin.push_back(all.count());
        const TexelSamples sub = samplesOf(c);
        all.pos.insert(all.pos.end(), sub.pos.begin(), sub.pos.end());
        all.nrm.insert(all.nrm.end(), sub.nrm.begin(), sub.nrm.end());
        all.texel.insert(all.texel.end(), sub.texel.begin(), sub.texel.end());
        all.sub.insert(all.sub.end(), sub.sub.begin(), sub.sub.end());
        all.seed.insert(all.seed.end(), sub.seed.begin(), sub.seed.end());
        all.passOffset.insert(all.passOffset.end(), sub.count(), static_cast<uint32_t>(c.passes) * static_cast<uint32_t>(perPass));
    }
    begin.push_back(all.count());

    std::vector<float> d, db;
    bool onGpu = false;
    gpuError_.clear();
    if (cfg_.useGpu && !gpuDisabled_ && bvh && gpu::needsRayTracing(lights_, &mesh_, o)) {
        const gpu::Result r = gpu::evaluateDirect(all, lights_, &mesh_, o, d, &db);
        gpuValidation_ = r.validation;
        if (r.ok) {
            onGpu = true;
            evaluator_ = r.device;
        } else {
            gpuError_ = r.error;
            gpuDisabled_ = true;  // no device / no ray query / a limit: do not pay for a failed device creation every step
        }
    }
    if (!onGpu) {
        evaluator_ = "cpu";
        d = evaluateDirectWithBounceCpu(all, lights_, bvh, o, &db);
    }

    for (size_t n = 0; n < ids.size(); ++n) {
        LiveChunk& c = chunks_[static_cast<size_t>(ids[n])];
        const size_t first = begin[n];
        const int p = c.passes;
        const float k = p == 0 ? 1.f : 1.f / static_cast<float>(p + 1);  // running mean: new samples join the estimate
        auto publish = [&] {
            for (size_t j = 0; j < c.samples.size(); ++j) {
                const size_t t = static_cast<size_t>(samples_.texel[c.samples[j]]) * 4;
                for (size_t ch = 0; ch < 3; ++ch) atlasS_[t + ch] = db_[c.samples[j] * 4 + ch] + (c.resetIndirect ? 0.f : indirect_[c.samples[j] * 4 + ch]);
                atlasS_[t + 3] = 1.f;
            }
        };
        for (size_t j = 0; j < c.samples.size(); ++j)
            for (size_t ch = 0; ch < 3; ++ch) {
                float& D = direct_[c.samples[j] * 4 + ch];
                float& B = db_[c.samples[j] * 4 + ch];
                D += (d[(first + j) * 4 + ch] - D) * k;
                B += (db[(first + j) * 4 + ch] - B) * k;
            }
        publish();
        if (cfg_.bounces && bvh && mesh_.hasLightmap()) {
            const bool fine = fineGi();
            const TexelSamples sub = fine ? giSamplesOf(c) : samplesOf(c);  // on the fine grid the gather runs over the grid entries
            const std::vector<float> g = gatherCpu(sub, mesh_, *bvh_, atlasS_, std::min(cfg_.albedo, kMaxLiveAlbedo), cfg_.giSamplesPerPass,
                                                   static_cast<uint32_t>(p) * static_cast<uint32_t>(perPass), cfg_.bounceRange, samples_.atlas);
            const float kk = (p == 0 || c.resetIndirect) ? 1.f : 1.f / static_cast<float>(std::min(p + 1, std::max(1, cfg_.indirectWindow)));  // moving average
            if (fine) {
                size_t cursor = 0;
                for (uint32_t smp : c.samples) accumulateFine(smp, g.data(), cursor, kk);
            } else {
                for (size_t j = 0; j < c.samples.size(); ++j)
                    for (size_t ch = 0; ch < 3; ++ch) {
                        float& I = indirect_[c.samples[j] * 4 + ch];
                        I += (g[j * 4 + ch] - I) * kk;
                    }
            }
            publish();
        }
        c.passes = p + 1;
        c.resetIndirect = false;
    }
}

LiveBaker::Rect LiveBaker::chunkRect(int i) const {
    const LiveChunk& c = chunks_[static_cast<size_t>(i)];
    const int edge = std::max(1, cfg_.chunkTexels), pad = std::max(1, layout_.settings.padding);
    Rect r;
    r.page = c.page;
    r.x = std::max(0, c.tx * edge - pad);
    r.y = std::max(0, c.ty * edge - pad);
    r.w = std::min(samples_.atlas, (c.tx + 1) * edge + pad) - r.x;
    r.h = std::min(samples_.atlas, (c.ty + 1) * edge + pad) - r.y;
    return r;
}

uint16_t floatToHalf(float v) {
    if (!(v > 0.f)) return 0;  // negative, zero and NaN
    if (v >= 65504.f) return 0x7BFF;
    uint32_t b;
    std::memcpy(&b, &v, 4);
    const int e = static_cast<int>((b >> 23) & 0xFF) - 127;
    uint32_t m = b & 0x7FFFFF;
    if (e < -24) return 0;
    if (e < -14) {  // subnormal half
        m |= 0x800000;
        const int shift = -e - 14 + 13;
        uint32_t h = m >> shift;
        const uint32_t rem = m & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (h & 1))) ++h;
        return static_cast<uint16_t>(h);
    }
    uint32_t h = (static_cast<uint32_t>(e + 15) << 10) | (m >> 13);
    const uint32_t rem = m & 0x1FFF;
    if (rem > 0x1000 || (rem == 0x1000 && (h & 1))) ++h;  // may carry into the exponent: still correct
    return static_cast<uint16_t>(h > 0x7BFF ? 0x7BFF : h);
}

float halfToFloat(uint16_t h) {
    const int e = (h >> 10) & 0x1F;
    const float m = static_cast<float>(h & 0x3FF);
    float v = e == 0 ? std::ldexp(m, -24) : std::ldexp(1.f + m / 1024.f, e - 15);
    return (h & 0x8000) ? -v : v;
}

std::vector<uint16_t> LiveBaker::renderTile(const Rect& r) const {
    std::vector<uint16_t> out(static_cast<size_t>(r.w) * static_cast<size_t>(r.h) * 4, 0);
    const int atlas = samples_.atlas;
    const size_t perPage = static_cast<size_t>(atlas) * atlas;
    auto sampleOf = [&](int x, int y) -> int32_t {
        if (x < 0 || y < 0 || x >= atlas || y >= atlas || sampleAt_.empty()) return -1;
        return sampleAt_[static_cast<size_t>(r.page) * perPage + static_cast<size_t>(y) * atlas + x];
    };
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) {
            const int ax = r.x + x, ay = r.y + y;
            Vec3 v;
            int count = 0;
            const int32_t own = sampleOf(ax, ay);
            if (own >= 0) { v = shownValue(static_cast<size_t>(own)); count = 1; }
            else {  // gutter: one ring of dilation, like assemblePages
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int32_t s = sampleOf(ax + dx, ay + dy);
                        if (s < 0) continue;
                        v = v + shownValue(static_cast<size_t>(s));
                        ++count;
                    }
                if (count) v = v * (1.0 / count);
            }
            uint16_t* px = &out[(static_cast<size_t>(y) * static_cast<size_t>(r.w) + static_cast<size_t>(x)) * 4];
            if (!count) continue;  // alpha 0: nothing here
            px[0] = floatToHalf(static_cast<float>(v.x));
            px[1] = floatToHalf(static_cast<float>(v.y));
            px[2] = floatToHalf(static_cast<float>(v.z));
            px[3] = 0x3C00;  // 1.0
        }
    return out;
}

LightmapPages LiveBaker::snapshot() const {
    std::vector<float> values(samples_.count() * 4, 0.f);
    for (size_t i = 0; i < samples_.count(); ++i) {
        const Vec3 v = shownValue(i);
        values[i * 4] = static_cast<float>(v.x);
        values[i * 4 + 1] = static_cast<float>(v.y);
        values[i * 4 + 2] = static_cast<float>(v.z);
    }
    return assemblePages(layout_, samples_, values);
}

// ---- change detection for geometry ---------------------------------------------------------------------------------------------
std::vector<Aabb> diffSurfaces(const std::vector<LightSurface>& before, const std::vector<LightSurface>& after) {
    struct Item { uint64_t hash; Aabb box; };
    auto collect = [](const std::vector<LightSurface>& list) {
        std::map<std::string, std::vector<Item>> out;
        for (const auto& s : list) {
            const std::string key = s.instance + "|" + s.entityId + "|" + s.brushInstance + "|" + s.brushId + "|" + std::to_string(s.faceId) + (s.side == FaceSide::Front ? "F" : "B");
            Item it{1469598103934665603ull, {}};
            // The albedo of the face is part of what its neighbours see through a bounce: a retexture (or a shift / scale / rotate of the texture)
            // changes the light around the face although no vertex moved.
            it.hash = mix(it.hash, static_cast<uint64_t>(std::hash<std::string>{}(s.texture)));
            it.hash = mix(it.hash, static_cast<uint64_t>(q(s.projection.scaleU, 10000)));
            it.hash = mix(it.hash, static_cast<uint64_t>(q(s.projection.scaleV, 10000)));
            it.hash = mix(it.hash, static_cast<uint64_t>(q(s.projection.shiftU, 10000)));
            it.hash = mix(it.hash, static_cast<uint64_t>(q(s.projection.shiftV, 10000)));
            it.hash = mix(it.hash, static_cast<uint64_t>(q(s.projection.rotate, 10000)));
            it.hash = mix(it.hash, s.lookHash);  // UV kind / basis / shear / stored uvs / repetition / layout
            if (s.shading) it.hash = mix(it.hash, s.shading->hash);  // smooth edges change the normals the face is lit with
            if (s.relief) {  // the relief changes the shadows around the face and what it shows to a bounce: its shape and extent count
                it.hash = mix(it.hash, s.relief->hash);
                it.box.add(s.relief->bmin);
                it.box.add(s.relief->bmax);
            }
            for (const auto& v : s.verts) {
                it.box.add(v);
                it.hash = mix(mix(mix(it.hash, static_cast<uint64_t>(q(v.x, 10000))), static_cast<uint64_t>(q(v.y, 10000))), static_cast<uint64_t>(q(v.z, 10000)));
            }
            out[key].push_back(it);
        }
        return out;
    };
    const auto a = collect(before), b = collect(after);
    std::vector<Aabb> regions;
    auto unmatched = [&](const std::vector<Item>& from, const std::vector<Item>* other) {
        std::multiset<uint64_t> pool;
        if (other) for (const auto& o : *other) pool.insert(o.hash);
        for (const auto& it : from) {
            const auto f = pool.find(it.hash);
            if (f != pool.end()) pool.erase(f);
            else regions.push_back(it.box);
        }
    };
    for (const auto& kv : a) {
        const auto o = b.find(kv.first);
        unmatched(kv.second, o == b.end() ? nullptr : &o->second);
    }
    for (const auto& kv : b) {
        const auto o = a.find(kv.first);
        unmatched(kv.second, o == a.end() ? nullptr : &o->second);
    }
    return regions;
}

}  // namespace core
