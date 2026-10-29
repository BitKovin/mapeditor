#include "core/lighting_service.h"
#include "core/playable_visibility.h"
#include "core/parallel.h"
#include "core/relief_geometry.h"
#include "core/smoothing.h"
#include "core/protocol.h"
#include "core/textures.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <thread>

using nlohmann::json;

namespace core {

// Chunks within this distance (m) of the camera get the intermediate versions of their light; further away only the first and the final one are sent
constexpr double kNearTiles = 150.0;
namespace {
json optionsToJson(const LightingService::Options& o) {
    return json{{"texelsPerMeter", o.layout.texelsPerMeter}, {"atlasSize", o.layout.atlasSize}, {"padding", o.layout.padding},
                {"mergeAngleDeg", o.layout.mergeAngleDeg}, {"exposure", o.exposure},
                {"directSupersample", o.live.directSupersample}, {"giScale", o.live.giScale},
                {"areaSamplesPerPass", o.live.areaSamplesPerPass}, {"skySamplesPerPass", o.live.skySamplesPerPass}, {"giSamplesPerPass", o.live.giSamplesPerPass},
                {"maxPasses", o.live.maxPasses}, {"chunkTexels", o.live.chunkTexels}, {"bounces", o.live.bounces}, {"albedo", o.live.albedo},
                {"indirectWindow", o.live.indirectWindow},
                {"denoise", o.live.denoiseIndirect}, {"denoiseQuality", o.live.denoiseQuality},
                {"useGpu", o.live.useGpu}, {"useTextures", o.live.useTextures}, {"gpuTexelsPerStep", o.live.gpuTexelsPerStep}, {"gpuTargetStepMs", o.live.gpuTargetStepMs},
                {"gpuMaxStepScale", o.live.gpuMaxStepScale}, {"texelsPerStep", o.texelsPerStep}, {"tickBudgetMs", o.tickBudgetSeconds * 1000.0}, {"flushIntervalMs", o.flushIntervalMs},
                {"defaultSourceSize", o.defaultSourceSize}, {"defaultSunSize", o.defaultSunSize}, {"firstSendFraction", o.firstSendFraction}, {"maxHoldSeconds", o.maxHoldSeconds},
                {"lightvol", o.lightvolEnabled}, {"lightvolGridSize", o.lightvol.gridSize}, {"lightvolMaxProbes", o.lightvol.maxProbes}, {"lightvolNormals", o.lightvol.normals},
                {"lightvolGiSamples", o.lightvol.giSamples}, {"lightvolProbesPerStep", o.lightvolProbesPerStep},
                {"skipVisibility", o.skipVisibility}, {"stickyVisibility", o.stickyVisibility}, {"stickyVisibilityRadius", o.stickyVisibilityRadius}, {"lightvolIdleSeconds", o.lightvolIdleSeconds}};
}
}  // namespace

LightingService::Options LightingService::loadOptions(const std::filesystem::path& file, std::string* warning) {
    Options o;
    o.firstSendFraction = 0;    // the mesh goes out at once, the Shell predicts the direct light until the Core's tiles arrive
    o.skipVisibility = true;    // the live view only previews light: no visibility, no unseen-face culling or density falloff
    o.stickyVisibility = true;  // the editor's setting;
    o.lightvolIdleSeconds = 2;  // likewise Options{} (export, tests) asks the visibility of every face every time
    std::ifstream in(file, std::ios::binary);
    if (!in) return o;
    try {
        const json j = json::parse(in);
        if (!j.is_object()) throw std::runtime_error("not a JSON object");
        auto num = [&](const char* k, double& v, double lo, double hi) {
            if (j.contains(k) && j[k].is_number()) v = std::clamp(j[k].get<double>(), lo, hi);
        };
        auto integer = [&](const char* k, int& v, int lo, int hi) {
            if (j.contains(k) && j[k].is_number()) v = static_cast<int>(std::clamp(std::llround(j[k].get<double>()), static_cast<long long>(lo), static_cast<long long>(hi)));
        };
        auto flag = [&](const char* k, bool& v) { if (j.contains(k) && j[k].is_boolean()) v = j[k].get<bool>(); };
        num("texelsPerMeter", o.layout.texelsPerMeter, 0.25, 256);
        integer("atlasSize", o.layout.atlasSize, 64, 4096);
        { int a = 64; while (a < o.layout.atlasSize) a *= 2; o.layout.atlasSize = a; }  // power of two (the engine uses a texture array)
        integer("padding", o.layout.padding, 2, 8);  // at least 2: the bicubic lightmap lookup (editor and game) reads 2 texels around a chart
        num("mergeAngleDeg", o.layout.mergeAngleDeg, 0, 180);
        num("exposure", o.exposure, 0.05, 20);
        integer("directSupersample", o.live.directSupersample, 1, kMaxSuperSamples);
        integer("giScale", o.live.giScale, 1, 4);
        if (o.live.giScale == 3) o.live.giScale = 4;
        integer("areaSamplesPerPass", o.live.areaSamplesPerPass, 1, 1024);
        integer("skySamplesPerPass", o.live.skySamplesPerPass, 1, 1024);
        integer("giSamplesPerPass", o.live.giSamplesPerPass, 1, 1024);
        integer("maxPasses", o.live.maxPasses, 1, 4096);
        integer("chunkTexels", o.live.chunkTexels, 4, 128);
        flag("bounces", o.live.bounces);
        num("albedo", o.live.albedo, 0.0, 1.0);
        integer("indirectWindow", o.live.indirectWindow, 1, 1024);
        flag("denoise", o.live.denoiseIndirect);
        integer("denoiseQuality", o.live.denoiseQuality, 0, 2);
        flag("useGpu", o.live.useGpu);
        flag("useTextures", o.live.useTextures);
        { double v = static_cast<double>(o.live.gpuTexelsPerStep); num("gpuTexelsPerStep", v, 1024, 1e9); o.live.gpuTexelsPerStep = static_cast<size_t>(v); }
        num("gpuTargetStepMs", o.live.gpuTargetStepMs, 5, 400);
        integer("gpuMaxStepScale", o.live.gpuMaxStepScale, 1, 256);
        { double v = static_cast<double>(o.texelsPerStep); num("texelsPerStep", v, 64, 1e8); o.texelsPerStep = static_cast<size_t>(v); }
        { double ms = o.tickBudgetSeconds * 1000.0; num("tickBudgetMs", ms, 5, 2000); o.tickBudgetSeconds = ms / 1000.0; }
        num("flushIntervalMs", o.flushIntervalMs, 0, 2000);
        num("defaultSourceSize", o.defaultSourceSize, 0.0, 10.0);
        num("defaultSunSize", o.defaultSunSize, 0.0, 30.0);
        num("firstSendFraction", o.firstSendFraction, 0.0, 1.0);
        num("maxHoldSeconds", o.maxHoldSeconds, 0.0, 60.0);
        flag("lightvol", o.lightvolEnabled);
        flag("skipVisibility", o.skipVisibility);
        flag("stickyVisibility", o.stickyVisibility);
        num("stickyVisibilityRadius", o.stickyVisibilityRadius, 0, 100000);
        num("lightvolIdleSeconds", o.lightvolIdleSeconds, 0, 600);
        num("lightvolGridSize", o.lightvol.gridSize, 0.25, 64.0);
        integer("lightvolMaxProbes", o.lightvol.maxProbes, 0, 2000000000);
        integer("lightvolNormals", o.lightvol.normals, 6, 256);
        integer("lightvolGiSamples", o.lightvol.giSamples, 1, 1024);
        { double v = static_cast<double>(o.lightvolProbesPerStep); num("lightvolProbesPerStep", v, 1, 100000); o.lightvolProbesPerStep = static_cast<size_t>(v); }
    } catch (const std::exception& e) {
        if (warning) *warning = std::string("lighting settings '") + file.string() + "' ignored: " + e.what();
        Options d;
        d.firstSendFraction = 0;
        d.skipVisibility = true;
        d.stickyVisibility = true;
        d.lightvolIdleSeconds = 2;
        return d;
    }
    return o;
}

json LightingService::optionsJson(const Options& o) { return optionsToJson(o); }

bool LightingService::writeDefaultOptions(const std::filesystem::path& file) {
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) return true;
    try {
        if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        Options d;  // the editor's defaults (loadOptions)
        d.firstSendFraction = 0;
        d.skipVisibility = true;
        d.stickyVisibility = true;
        d.lightvolIdleSeconds = 2;
        out << optionsToJson(d).dump(2) << "\n";
        return static_cast<bool>(out);
    } catch (const std::exception&) {
        return false;
    }
}

using Clock = std::chrono::steady_clock;

namespace {

double msBetween(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

bool sameSurface(const LightSurface& a, const LightSurface& b) {
    auto eq = [](const Vec3& x, const Vec3& y) { return x.x == y.x && x.y == y.y && x.z == y.z; };
    if (a.faceId != b.faceId || a.side != b.side || !eq(a.normal, b.normal) || a.verts.size() != b.verts.size()) return false;
    if (a.instance != b.instance || a.entityId != b.entityId || a.classname != b.classname || a.brushId != b.brushId || a.brushInstance != b.brushInstance) return false;
    for (size_t i = 0; i < a.verts.size(); ++i) if (!eq(a.verts[i], b.verts[i])) return false;
    if ((a.relief ? a.relief->hash : 0) != (b.relief ? b.relief->hash : 0)) return false;  // a relief appeared, went, or changed shape
    if ((a.shading ? a.shading->hash : 0) != (b.shading ? b.shading->hash : 0)) return false;  // smooth edges marked or cleared (REQUIREMENTS §33 H6)
    if ((a.smoothing != nullptr) != (b.smoothing != nullptr)) return false;  // the lightmap charts follow smooth edges
    // The texture does not matter to the light of the face, but the bounce colour of the face comes from it (Lit view and albedo of the bounces) and
    // the projection decides which texel of it: a retexture must reach the baker as a change.
    const FaceProjection &p = a.projection, &q = b.projection;
    return a.lightmapScale == b.lightmapScale && a.texture == b.texture && a.lookHash == b.lookHash && p.scaleU == q.scaleU && p.scaleV == q.scaleV && p.shiftU == q.shiftU && p.shiftV == q.shiftV && p.rotate == q.rotate;
}
bool sameSurfaces(const std::vector<LightSurface>& a, const std::vector<LightSurface>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (!sameSurface(a[i], b[i])) return false;
    return true;
}

constexpr uint16_t kHalfOne = 0x3C00;  // 1.0 as a half: the alpha of a pixel that holds a value

// One tile entry; `px` are RGBA halves (little endian on every supported host).
void appendTile(std::string& json, bool first, int page, int x, int y, int w, int h, const std::vector<uint16_t>& px, const std::vector<uint16_t>* direct = nullptr) {
    if (!first) json += ',';
    std::vector<unsigned char> raw(px.size() * 2);
    if (!px.empty()) std::memcpy(raw.data(), px.data(), raw.size());
    json += "{\"page\":" + std::to_string(page) + ",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + ",\"w\":" + std::to_string(w) + ",\"h\":" + std::to_string(h) +
            ",\"rgba\":\"" + base64Encode(raw) + "\"";
    if (direct && !direct->empty()) {  // the same rectangle of the direct layer (DESIGN 6.11): applied together with the tile, so the two never disagree on screen
        std::vector<unsigned char> d(direct->size() * 2);
        std::memcpy(d.data(), direct->data(), d.size());
        json += ",\"d\":\"" + base64Encode(d) + "\"";
    }
    json += "}";
}

}  // namespace

// ---- construction, options, switches --------------------------------------------------------------------------------------------

LightingService::LightingService(Scene& scene, const Options& options) : scene_(scene), opt_(options), baker_(options.live) {
    exposure_ = options.exposure;
    baseExposure_ = options.exposure;
    baseLive_ = options.live;
    baseVol_ = options.lightvol;
    maxPasses_ = options.live.maxPasses;
    lastFlush_ = lastEmit_ = Clock::now();
    styleTable_ = LightStyleTable::fromProfile(scene_.profile());
}
LightingService::~LightingService() { stopWorker(); }

void LightingService::setSessionFactory(LiveBaker::SessionFactory f) {
    sessionFactory_ = f;
    baker_.setSessionFactory(f);
    for (auto& l : layers_) l.baker->setSessionFactory(f);
}

void LightingService::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    ++enableGen_;
    if (on) dirty_ = true;
    statusDue_ = true;
    ++epoch_;
    cv_.notify_one();
}

void LightingService::setExposure(double e) {
    baseExposure_ = e;
    if (level_.exposure > 0) return;  // the level's own exposure wins
    if (e == exposure_) return;
    exposure_ = e;  // a display setting: the Shell multiplies it in (lighting_status carries it); tiles and probes keep their full range
    statusDue_ = true;
    cv_.notify_one();
}

void LightingService::setCamera(const Vec3& camera) {
    std::lock_guard<std::mutex> lock(m_);
    camera_ = camera;
    haveCamera_ = true;
}

// ---- the layers -----------------------------------------------------------------------------------------------------------------

std::vector<std::pair<int, LiveBaker*>> LightingService::bakers() {
    std::vector<std::pair<int, LiveBaker*>> v{{0, &baker_}};
    for (auto& l : layers_) v.push_back({l.style, l.baker.get()});
    return v;
}
std::vector<std::pair<int, const LiveBaker*>> LightingService::bakers() const {
    std::vector<std::pair<int, const LiveBaker*>> v{{0, &baker_}};
    for (const auto& l : layers_) v.push_back({l.style, l.baker.get()});
    return v;
}
bool LightingService::allConverged() const {
    for (const auto& [s, b] : bakers()) if (!b->converged()) return false;
    return true;
}
bool LightingService::anyDenoisePending() const {
    for (const auto& [s, b] : bakers()) if (b->denoisePending()) return true;
    return false;
}
size_t LightingService::totalChunks() const {
    size_t n = 0;
    for (const auto& [s, b] : bakers()) n += b->chunkCount();
    return n;
}
size_t LightingService::staleChunks() const {
    size_t n = 0;
    for (const auto& [s, b] : bakers()) n += b->staleChunks();
    return n;
}
bool LightingService::anyDirtyChunks() const {
    for (const auto& [s, b] : bakers()) if (b->hasDirtyChunks()) return true;
    return false;
}
std::vector<int> LightingService::styles() const {
    std::vector<int> v;
    for (const auto& [s, b] : bakers()) v.push_back(s);
    return v;
}
const LiveBaker* LightingService::layerBaker(int style) const {
    for (const auto& [s, b] : bakers()) if (s == style) return b;
    return nullptr;
}

bool LightingService::busy() const {
    if (threaded()) return false;
    return enabled_ && (dirty_ || (built_ && (!allConverged() || anyDenoisePending() || anyDirtyChunks() || meshPending_ || flushPending_)));
}

// ---- status ---------------------------------------------------------------------------------------------------------------------

json LightingService::statusLive() const {
    size_t chunks = 0, stale = 0, texels = 0;
    int minPasses = -1;
    for (const auto& [s, b] : bakers()) {
        chunks += b->chunkCount();
        stale += b->staleChunks();
        texels += b->sampleCount();
        if (b->chunkCount()) minPasses = minPasses < 0 ? b->minPasses() : std::min(minPasses, b->minPasses());
    }
    return json{{"type", proto::kLightingStatus}, {"enabled", enabled_.load()}, {"rev", rev_.load()}, {"chunks", chunks}, {"stale", stale},
                {"minPasses", std::max(minPasses, 0)}, {"maxPasses", maxPasses_.load()}, {"converged", built_ && allConverged()},
                {"exposure", exposure_.load()}, {"brightness", brightness_.load()}, {"texels", texels}, {"pages", baker_.layout().pageCount}, {"evaluator", baker_.evaluator()},
                {"directLayer", baker_.directLayerAvailable()}, {"defaultSourceSize", opt_.defaultSourceSize}, {"defaultSunSize", opt_.defaultSunSize},
                {"gpuError", baker_.gpuError()}, {"denoisedPages", baker_.denoisedPages()}, {"denoiseError", baker_.denoiseError()}, {"steps", baker_.stats().steps},
                {"gpuSeconds", baker_.stats().gpuSeconds}, {"stepSeconds", baker_.stats().stepSeconds}, {"styles", styles()}, {"styleWarnings", styleWarnings_},
                {"timingsMs", json{{"queued", timings_.queued}, {"surfaces", timings_.surfaces}, {"layout", timings_.layout}, {"visibility", timings_.visibility}, {"diff", timings_.diff}, {"geometry", timings_.geometry}, {"lights", timings_.lights},
                                   {"session", timings_.session}, {"steps", timings_.steps}, {"tiles", timings_.tiles}, {"denoise", timings_.denoise},
                                   {"firstTiles", timings_.firstTiles}, {"converged", timings_.converged}}}};
}

json LightingService::statusJson() const {
    if (threaded()) {
        std::lock_guard<std::mutex> lock(mirror_.m);
        if (!mirror_.status.empty()) return json::parse(mirror_.status);
        return json{{"type", proto::kLightingStatus}, {"enabled", enabled_.load()}, {"rev", rev_.load()}, {"chunks", 0}, {"stale", 0}, {"minPasses", 0},
                    {"maxPasses", maxPasses_.load()}, {"converged", false}, {"exposure", exposure_.load()}, {"brightness", brightness_.load()}, {"texels", 0}, {"pages", 0}};
    }
    return statusLive();
}

std::string LightingService::statusMessage() {
    const std::string s = statusLive().dump();
    std::lock_guard<std::mutex> lock(mirror_.m);
    mirror_.status = s;
    return s;
}

// ---- messages -------------------------------------------------------------------------------------------------------------------

std::string LightingService::lightStylesMessage() const {
    return json{{"type", proto::kLightStyles}, {"patterns", styleTable_.toJson()}, {"hz", kStyleHz}}.dump();
}

std::string LightingService::litMeshMessage(bool keepPages) const {
    // Rebuilt from the same surfaces the layout was made from: one vertex = pos, normal, lightmap uv, page, and the style layers of its chart as a bit mask
    // (styles 1..63 -> bit style-1, split over three floats of 21 bits each: a float holds 24 bits exactly). Style 0, the base, is always there.
    std::vector<float> v;
    json faces = json::array();
    std::set<std::string> mapped;  // faces whose lightmap mapping was already sent (once per authored face, with its first piece)
    const LightmapLayout& L = baker_.layout();
    for (const auto& s : prevSurfaces_) {
        const LightmapFace* f = L.findForFragment(s.instance, s.entityId, s.brushInstance, s.brushId, s.faceId, s.verts);
        if (!f || f->chart < 0 || s.verts.size() < 3) continue;
        const LightmapChart& chart = L.charts[static_cast<size_t>(f->chart)];
        const float page = static_cast<float>(chart.page);
        uint64_t mask = 0;
        for (int st : chart.styles) if (st >= 1 && st <= kMaxStyleId) mask |= (uint64_t{1} << (st - 1));
        const float m0 = static_cast<float>(mask & 0x1FFFFF), m1 = static_cast<float>((mask >> 21) & 0x1FFFFF), m2 = static_cast<float>((mask >> 42) & 0x1FFFFF);
        auto push = [&](const Vec3& p) {
            const Vec2 uv = L.uv(*f, p);
            for (double c : {p.x, p.y, p.z, s.normal.x, s.normal.y, s.normal.z, uv.u, uv.v}) v.push_back(static_cast<float>(c));
            v.push_back(page);
            v.push_back(m0); v.push_back(m1); v.push_back(m2);
        };
        for (size_t k = 1; k + 1 < s.verts.size(); ++k) { push(s.verts[0]); push(s.verts[k]); push(s.verts[k + 1]); }
        json entry{{"i", s.instance}, {"e", s.entityId}, {"bi", s.brushInstance}, {"b", s.brushId}, {"f", s.faceId}, {"t", s.texture}, {"n", s.verts.size() - 2}};
        if (mapped.insert(f->key()).second) {
            // The face's lightmap mapping for the Shell (decals lie on faces and take their light): uv = (dot(p, u.xyz) + u.w, dot(p, v.xyz) + v.w), page, style mask
            const double a = L.settings.atlasSize;
            entry["m"] = json{{"u", {f->su.x / a, f->su.y / a, f->su.z / a, f->so / a}}, {"v", {f->sv.x / a, f->sv.y / a, f->sv.z / a, f->to / a}}, {"pg", chart.page}, {"k", {m0, m1, m2}}};
        }
        faces.push_back(std::move(entry));
    }
    std::vector<unsigned char> bytes(v.size() * sizeof(float));
    if (!v.empty()) std::memcpy(bytes.data(), v.data(), bytes.size());
    return json{{"type", proto::kLitMesh}, {"rev", rev_.load()}, {"atlas", L.settings.atlasSize}, {"pageCount", L.pageCount}, {"triangleCount", v.size() / 36},
                {"keepPages", keepPages}, {"vertices", base64Encode(bytes)}, {"faces", faces}}.dump();
}

void LightingService::allChunksOf(const LiveBaker& baker, std::vector<int>& ids) const {
    for (size_t i = 0; i < baker.chunkCount(); ++i) if (baker.chunkHasData(static_cast<int>(i))) ids.push_back(static_cast<int>(i));
}

// The rectangles to send for a set of chunks: runs of chunks next to each other in a row, then rows of equal runs on top of each other (blocks of at most
// kMaxBlockChunks per side), each with the padding of its outer chunks.
std::vector<LiveBaker::Rect> LightingService::mergeChunkRects(const LiveBaker& baker, std::vector<int> ids) {
    constexpr int kMaxBlockChunks = 8;
    auto key = [&](int id) { const LiveChunk& c = baker.chunk(id); return std::array<int, 3>{c.page, c.ty, c.tx}; };
    std::sort(ids.begin(), ids.end(), [&](int a, int b) { return key(a) < key(b); });
    struct Run { int page, ty, tx0, tx1; };
    std::vector<Run> runs;
    for (int id : ids) {
        const LiveChunk& c = baker.chunk(id);
        if (!runs.empty() && runs.back().page == c.page && runs.back().ty == c.ty && runs.back().tx1 + 1 == c.tx && runs.back().tx1 - runs.back().tx0 + 1 < kMaxBlockChunks) runs.back().tx1 = c.tx;
        else runs.push_back({c.page, c.ty, c.tx, c.tx});
    }
    struct Block { int page, ty0, ty1, tx0, tx1; };
    std::vector<Block> blocks;
    std::map<std::array<int, 3>, size_t> open;  // (page, tx0, tx1) -> the block that ends in the previous row
    for (const Run& r : runs) {
        const auto it = open.find({r.page, r.tx0, r.tx1});
        if (it != open.end() && blocks[it->second].ty1 + 1 == r.ty && blocks[it->second].ty1 - blocks[it->second].ty0 + 1 < kMaxBlockChunks) {
            blocks[it->second].ty1 = r.ty;
            continue;
        }
        open[{r.page, r.tx0, r.tx1}] = blocks.size();
        blocks.push_back({r.page, r.ty, r.ty, r.tx0, r.tx1});
    }
    // a rectangle is the union of the rectangles of its first and last chunk; chunk indices are found through the sorted ids
    std::map<std::array<int, 3>, int> idOf;
    for (int id : ids) idOf[key(id)] = id;
    std::vector<LiveBaker::Rect> out;
    for (const Block& b : blocks) {
        const LiveBaker::Rect a = baker.chunkRect(idOf.at({b.page, b.ty0, b.tx0})), z = baker.chunkRect(idOf.at({b.page, b.ty1, b.tx1}));
        out.push_back({a.page, a.x, a.y, z.x + z.w - a.x, z.y + z.h - a.y});
    }
    return out;
}

// Renders the chunks of one layer, keeps the ones whose pixels differ from what the clients hold and appends them as lightmap_tiles messages.
size_t LightingService::tilesFor(int style, LiveBaker& baker, const std::vector<int>& chunks, std::vector<std::string>& out) {
    const auto t0 = Clock::now();
    baker.noteSent(chunks);
    const int atlas = baker_.layout().settings.atlasSize;
    std::string msg;
    size_t sent = 0, inMsg = 0, bytes = 0;
    auto open = [&] {
        msg = "{\"type\":\"lightmap_tiles\",\"rev\":" + std::to_string(rev_.load()) + ",\"atlas\":" + std::to_string(atlas) + ",\"style\":" + std::to_string(style) + ",\"tiles\":[";
        inMsg = 0;
        bytes = 0;
    };
    auto close = [&] {
        if (!inMsg) return;
        msg += "]}";
        out.push_back(std::move(msg));
        msg.clear();
        inMsg = 0;
        bytes = 0;
    };
    // Rendering and encoding run on all cores (every refined chunk is re-sent after every step: on a big level that was the most expensive part of a
    // step); the comparison with the mirror stays in chunk order, because neighbouring tiles overlap in their padding.
    // Neighbouring chunks go out as one rectangle: every chunk's tile carries a margin of padding on all sides (the bicubic lookup reads 2 texels around a
    // chart), 56% more pixels than the chunk itself, and a block of 8 x 8 chunks only 6% more.
    const std::vector<LiveBaker::Rect> rects = mergeChunkRects(baker, chunks);
    const size_t nc = rects.size();
    std::vector<std::vector<uint16_t>> pixels(nc), directPixels(nc);
    const bool withDirect = style == 0 && baker.directLayerAvailable();  // the base layer only: the Shell predicts the lights of style 0
    parallelFor(nc, 4, [&](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i)
            if (rects[i].w > 0 && rects[i].h > 0) {
                pixels[i] = baker.renderTile(rects[i]);
                if (withDirect) directPixels[i] = baker.renderDirectTile(rects[i]);
            }
    });
    std::vector<char> changed(nc, 0);
    {
        std::lock_guard<std::mutex> lock(mirror_.m);
        for (size_t i = 0; i < nc; ++i) {
            const LiveBaker::Rect& r = rects[i];
            if (r.w <= 0 || r.h <= 0) continue;
            if (!mirror_.valid || mirror_.atlas != atlas || r.page < 0 || r.page >= mirror_.pageCount) continue;
            std::vector<std::vector<uint16_t>>& pages = mirror_.pages[style];  // a layer appears with its first tile, like in the Shell
            if (pages.empty()) pages.assign(static_cast<size_t>(mirror_.pageCount), std::vector<uint16_t>(static_cast<size_t>(atlas) * atlas * 4, 0));
            std::vector<uint16_t>& page = pages[static_cast<size_t>(r.page)];
            const std::vector<uint16_t>& px = pixels[i];
            for (int y = 0; y < r.h; ++y)
                for (int x = 0; x < r.w; ++x) {
                    const uint16_t* s = &px[(static_cast<size_t>(y) * r.w + x) * 4];
                    if (s[3] == 0) continue;  // nothing was baked here: the Shell keeps what it has
                    uint16_t* d = &page[(static_cast<size_t>(r.y + y) * atlas + r.x + x) * 4];
                    if (d[0] != s[0] || d[1] != s[1] || d[2] != s[2] || d[3] != kHalfOne) {
                        changed[i] = 1;
                        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = kHalfOne;
                    }
                }
            if (withDirect && !directPixels[i].empty()) {
                if (mirror_.directPages.empty()) mirror_.directPages.assign(static_cast<size_t>(mirror_.pageCount), std::vector<uint16_t>(static_cast<size_t>(atlas) * atlas * 4, 0));
                std::vector<uint16_t>& dpage = mirror_.directPages[static_cast<size_t>(r.page)];
                const std::vector<uint16_t>& dpx = directPixels[i];
                for (int y = 0; y < r.h; ++y)
                    for (int x = 0; x < r.w; ++x) {
                        const uint16_t* s = &dpx[(static_cast<size_t>(y) * r.w + x) * 4];
                        if (s[3] == 0) continue;
                        uint16_t* d = &dpage[(static_cast<size_t>(r.y + y) * atlas + r.x + x) * 4];
                        if (d[0] != s[0] || d[1] != s[1] || d[2] != s[2] || d[3] != s[3]) {
                            changed[i] = 1;
                            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
                        }
                    }
            }
        }
    }
    std::vector<std::string> encoded(nc);
    parallelFor(nc, 16, [&](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i)
            if (changed[i]) appendTile(encoded[i], true, rects[i].page, rects[i].x, rects[i].y, rects[i].w, rects[i].h, pixels[i], withDirect ? &directPixels[i] : nullptr);
    });
    for (size_t i = 0; i < nc; ++i) {
        if (!changed[i]) continue;
        if (!inMsg) open();
        else msg += ',';
        msg += encoded[i];
        ++inMsg;
        ++sent;
        bytes += pixels[i].size() * 2;
        if (bytes > 200000) close();
    }
    close();
    timings_.tiles += msBetween(t0, Clock::now());
    return sent;
}

size_t LightingService::flushDirty(std::vector<std::string>& out) {
    size_t sent = 0;
    if (!opt_.tiles) {  // nobody to send to
        for (auto& [style, b] : bakers()) b->takeDirtyChunks();
        return 0;
    }
    for (auto& [style, b] : bakers()) if (b->hasDirtyChunks()) sent += tilesFor(style, *b, b->takeDueChunks(flushHaveCamera_ ? &flushCamera_ : nullptr, kNearTiles), out);
    return sent;
}

// ---- light volumes ---------------------------------------------------------------------------------------------------------------

bool LightingService::sameLightLists(const std::vector<Light>& a, const std::vector<Light>& b) {
    if (a.size() != b.size()) return false;
    auto eq = [](const Vec3& x, const Vec3& y) { return x.x == y.x && x.y == y.y && x.z == y.z; };
    for (size_t i = 0; i < a.size(); ++i) {
        const Light &x = a[i], &y = b[i];
        if (x.id != y.id || x.instance != y.instance || x.type != y.type || !eq(x.position, y.position) || !eq(x.forward, y.forward) || !eq(x.right, y.right) ||
            !eq(x.up, y.up) || x.brightness != y.brightness || x.radius != y.radius || x.castShadows != y.castShadows || x.style != y.style ||
            x.sourceSize != y.sourceSize || x.innerAngle != y.innerAngle || x.outerAngle != y.outerAngle || x.width != y.width || x.height != y.height)
            return false;
    }
    return true;
}

AlbedoLoader LightingService::albedoLoader(bool force) {
    if (!force && !opt_.live.useTextures) return {};
    const GameProfile& pr = scene_.profile();
    std::string key = scene_.assetsRoot().string() + "|" + pr.textureRoot;
    for (const auto& e : pr.textureExtensions) key += "|" + e;
    if (!albedoLib_ || key != albedoKey_) { albedoLib_ = std::make_shared<AlbedoLibrary>(scene_.assetsRoot(), pr); albedoKey_ = key; }
    std::shared_ptr<AlbedoLibrary> lib = albedoLib_;
    return [lib](const std::string& name) { return lib->get(name); };
}

// The level's quality and colour settings on top of lighting.json (REQUIREMENTS §22 P2). Pass target = rays per texel / gather rays per pass.
void LightingService::applyLevel(const LevelSettings& level) {
    const LevelSettings old = level_;
    level_ = level;
    const double ex = level.exposure > 0 ? level.exposure : baseExposure_.load();
    if (ex != exposure_.load() || level.brightness != brightness_.load()) { exposure_ = ex; brightness_ = level.brightness; statusDue_ = true; }
    const LiveSettings& base = baseLive_;
    const int levelPasses = level.raysPerTexel > 0 ? std::clamp((level.raysPerTexel + std::max(1, base.giSamplesPerPass) - 1) / std::max(1, base.giSamplesPerPass), 1, 4096)
                                                   : base.maxPasses;
    const double q = std::clamp(opt_.qualityScale, 0.1, 64.0);
    const int passes = std::clamp(static_cast<int>(std::lround(levelPasses * q)), 1, 16384);
    const int ss = level.supersample > 0 ? level.supersample : base.directSupersample;
    opt_.live.maxPasses = passes;
    opt_.live.directSupersample = ss;
    maxPasses_ = passes;
    bool changed = false;
    for (auto& [s, b] : bakers()) changed = b->setQuality(passes, ss) || changed;
    if (changed) statusDue_ = true;
    const double grid = level.lightvolGrid > 0 ? level.lightvolGrid : baseVol_.gridSize;
    const int probeRays = std::max(1, static_cast<int>(std::lround((level.raysPerProbe > 0 ? level.raysPerProbe : baseVol_.giSamples) * q)));
    if (grid != opt_.lightvol.gridSize || probeRays != opt_.lightvol.giSamples) {
        opt_.lightvol.gridSize = grid;
        opt_.lightvol.giSamples = probeRays;
        volStale_ = true;
        volRunning_ = false;
        volDone_ = false;
    }
    (void)old;
}

LightvolSettings LightingService::volSettings() const {
    LightvolSettings c = opt_.lightvol;
    c.bounces = opt_.live.bounces;
    c.albedo = std::min(opt_.live.albedo, kMaxLiveAlbedo);
    c.useGpu = opt_.live.useGpu;
    c.classes = prevClasses_;  // of the snapshot the bakers work on (the Scene belongs to the other thread)
    return c;
}

std::string LightingService::lightvolGridMessage() const {
    const auto base = vols_.find(0);
    if (base == vols_.end() || base->second.count() == 0) return {};
    const LightvolGrid& g = base->second;
    // The Shell keeps every probe (9 bytes) and draws the ones around the view: the whole grid goes out. A message carries at most about this many probes
    // over all style layers (base64 of 24M probes is 290 MB); a bigger grid goes out with every s-th probe along each axis. The light of the entities is
    // sampled from the full grid here, and the export carries all of it.
    constexpr size_t kMessageProbes = size_t(24) << 20;
    const size_t layerCount = vols_.size();
    int stride = 1;
    auto strided = [&](int n) { return static_cast<size_t>((n + stride - 1) / stride); };
    while (strided(g.nx) * strided(g.ny) * strided(g.nz) * layerCount > kMessageProbes) ++stride;
    auto pack = [&](const LightvolGrid& src) {
        if (stride == 1) return base64Encode(src.packed());
        LightvolGrid sub;
        sub.nx = static_cast<int>(strided(src.nx)); sub.ny = static_cast<int>(strided(src.ny)); sub.nz = static_cast<int>(strided(src.nz));
        sub.probes.reserve(static_cast<size_t>(sub.nx) * sub.ny * sub.nz);
        for (int z = 0; z < src.nz; z += stride)
            for (int y = 0; y < src.ny; y += stride)
                for (int x = 0; x < src.nx; x += stride) sub.probes.push_back(src.probes[src.index(x, y, z)]);
        return base64Encode(sub.packed());
    };
    json layers = json::array();
    for (const auto& [style, other] : vols_) if (style != 0) layers.push_back(json{{"s", style}, {"probes", pack(other)}});
    const std::string baseProbes = pack(g);
    const int nx = stride == 1 ? g.nx : static_cast<int>(strided(g.nx)), ny = stride == 1 ? g.ny : static_cast<int>(strided(g.ny)), nz = stride == 1 ? g.nz : static_cast<int>(strided(g.nz));
    return json{{"type", proto::kLightvol}, {"rev", volRev_}, {"origin", {g.origin.x, g.origin.y, g.origin.z}}, {"cell", g.cell * stride},
                {"dims", {nx, ny, nz}}, {"probes", baseProbes}, {"layers", layers}}.dump();
}

std::string LightingService::entityLightMessage() const {
    // one entry per entity, with the light of every style layer that reaches it
    // one direction per entity for all its style layers (REQUIREMENTS §27 G4): the trilinear samples of the layers are re-fitted to a shared one
    std::map<std::pair<std::string, std::string>, std::vector<std::pair<int, Probe>>> ents;
    for (const auto& [style, g] : vols_)
        for (const EntityLight& e : entityLights(prevSurfaces_, prevClasses_, g))
            if (e.probe.valid) ents[{e.instance, e.entityId}].push_back({style, e.probe});
    json list = json::array();
    for (auto& [key, layers] : ents) {
        std::vector<Probe*> ptrs;
        for (auto& l : layers) ptrs.push_back(&l.second);
        shareDirection(ptrs);
        const Vec3 d = layers.front().second.dir;
        json lights = json::array();
        for (const auto& [style, p] : layers)
            lights.push_back(json{{"s", style}, {"ambient", {p.ambient.x, p.ambient.y, p.ambient.z}}, {"directed", {p.directed.x, p.directed.y, p.directed.z}}});
        list.push_back(json{{"i", key.first}, {"e", key.second}, {"dir", {d.x, d.y, d.z}}, {"lights", lights}});
    }
    return json{{"type", proto::kEntityLight}, {"rev", volRev_}, {"entities", list}}.dump();
}

void LightingService::sendLightvol(bool grid, std::vector<std::string>& out) {
    if (grid) {
        std::string g = lightvolGridMessage();
        if (!g.empty()) {
            out.push_back(g);
            std::lock_guard<std::mutex> lock(mirror_.m);
            mirror_.lightvol = std::move(g);
        }
    }
    std::string e = entityLightMessage();
    out.push_back(e);
    std::lock_guard<std::mutex> lock(mirror_.m);
    mirror_.entityLight = std::move(e);
}

bool LightingService::workLightvol(double budgetSeconds, Clock::time_point t0, const Emit& emit, const Interrupted& interrupted) {
    if (!opt_.lightvolEnabled || !built_) return false;
    auto elapsed = [&] { return std::chrono::duration<double>(Clock::now() - t0).count(); };
    std::vector<std::string> out;
    auto finish = [&] {
        if (!out.empty()) emit(std::move(out));
        return volStale_ || volRunning_ || volSendDue_ || entityLightDue_;
    };
    if (volDone_ && (volSendDue_ || entityLightDue_) && !meshPending_) {
        sendLightvol(volSendDue_, out);
        volSendDue_ = entityLightDue_ = false;
    }
    // The probes come last: the lightmaps (refined and denoised) are what the level is edited against, so they get the cores first. The probes gather
    // from the lightmaps' bounce sources: wait until those stopped moving.
    if (!allConverged() || anyDenoisePending() || denoiseInFlight_) {
        if (volRunning_) { probes_.reset(); volRunning_ = false; volStale_ = true; }
        return finish();
    }
    volWaiting_ = false;
    if (volStale_ && !volRunning_ && opt_.lightvolIdleSeconds > 0 && msBetween(lastEdit_, Clock::now()) < opt_.lightvolIdleSeconds * 1000.0) {
        volWaiting_ = true;  // not yet: the worker loop checks again in a few ms (it must not spin)
        return finish();
    }
    const LightvolSettings cfg = volSettings();
    auto startLayer = [&](int style) {
        const LiveBaker* b = layerBaker(style);
        LightvolSource src;
        src.lights = &b->lights();
        src.mesh = &b->occluders();
        src.bvh = b->bvh();
        src.atlasS = &b->bounceSource();
        src.atlasSize = b->layout().settings.atlasSize;
        src.pages = b->layout().pageCount;
        probes_ = std::make_unique<ProbeBaker>(volBase_, volSolid_, src, cfg);
    };
    if (volStale_ && !volRunning_) {
        volBase_ = buildLightvolGrid(prevSurfaces_, prevSolidSet_.get(), cfg, prevPlayable_.get(), prevLod_.lightvolMargin);
        volSolid_ = solidProbes(volBase_, prevSolidSet_.get(), prevPlayable_.get(), prevLod_.lightvolMargin);
        if (log_ && volBase_.count() > 0) {
            char line[200];
            std::snprintf(line, sizeof line, "lighting: light volume grid %dx%dx%d = %zu probes (cell %.2f m, lightvolGridSize / lightvolMaxProbes)", volBase_.nx, volBase_.ny, volBase_.nz,
                          volBase_.count(), volBase_.cell);
            log_(line);
        }
        volsWork_.clear();
        volQueue_.clear();
        for (int st : styles()) volQueue_.push_back(st);  // every layer has its own probes
        volQueuePos_ = 0;
        startLayer(volQueue_[0]);
        if (log_ && probes_) log_("lighting: light volume evaluator " + probes_->device() + (probes_->gpuError().empty() ? "" : ", GPU unavailable: " + probes_->gpuError()));
        volRunning_ = true;
        volStale_ = false;
    }
    if (!volRunning_ || !probes_) return finish();
    do {
        probes_->step(opt_.lightvolProbesPerStep);
        if (probes_->done()) {
            volDevice_ = probes_->device();
            volGpuError_ = probes_->gpuError();
            LightvolGrid g = probes_->take();
            probes_.reset();
            g.quantise();
            volsWork_[volQueue_[volQueuePos_]] = std::move(g);
            if (++volQueuePos_ < volQueue_.size()) startLayer(volQueue_[volQueuePos_]);
        }
    } while (probes_ && elapsed() < budgetSeconds && !(interrupted && interrupted()));
    if (!probes_ && volQueuePos_ >= volQueue_.size()) {
        shareProbeDirections(volsWork_);  // one light direction per point for every style layer (REQUIREMENTS §27 G4)
        vols_ = std::move(volsWork_);
        volsWork_.clear();
        volRunning_ = false;
        volDone_ = true;
        ++volRev_;
        volSendDue_ = true;
        entityLightDue_ = true;
        if (log_) {
            const LightvolGrid& g = vols_.begin()->second;
            char line[300];
            std::snprintf(line, sizeof line, "lighting: light volume baked, %zu probes (%dx%dx%d, cell %.2f m) x %zu style layer%s, evaluator %s%s%s", g.count(), g.nx, g.ny, g.nz, g.cell,
                          vols_.size(), vols_.size() == 1 ? "" : "s", volDevice_.c_str(), volGpuError_.empty() ? "" : ", GPU unavailable: ", volGpuError_.c_str());
            log_(line);
        }
        if (!meshPending_) {
            sendLightvol(true, out);
            volSendDue_ = entityLightDue_ = false;
        }
    }
    return finish();
}

// ---- taking in a snapshot of the document ----------------------------------------------------------------------------------------

LightingService::Inputs LightingService::captureInputs() {
    Inputs in;
    in.surfaces = scene_.lightSurfaces();
    in.classes = scene_.lightingClasses();
    in.solids = scene_.lightSolids();
    in.lights = scene_.lightScene();
    in.albedoLoader = albedoLoader();
    in.visibilityLoader = albedoLoader(true);
    {
        std::lock_guard<std::mutex> lock(texM_);
        in.changedTextures.swap(changedTextures_);
    }
    return in;
}

void LightingService::applyInputs(Inputs&& in, const Emit& emit) {
    try {
        timings_ = Timings{};
        timings_.queued = queuedMs_;
        queuedMs_ = 0;
        editStart_ = Clock::now();
        lastEdit_ = editStart_;
        firstTilesSent_ = false;
        const auto a0 = Clock::now();
        probes_.reset();  // it points into the bakers, whose geometry or lights are about to change
        applyLevel(in.lights.level);
        for (Light& l : in.lights.lights) {  // soften authored-as-perfect lights (Options::defaultSourceSize)
            if (l.sourceSize > 0) continue;
            if ((l.type == LightType::Point || l.type == LightType::Spot) && opt_.defaultSourceSize > 0) l.sourceSize = opt_.defaultSourceSize;
            else if (l.type == LightType::Directional && opt_.defaultSunSize > 0) l.sourceSize = opt_.defaultSunSize;
        }
        const uint64_t solidsFp = in.solids ? in.solids->fingerprint() : 0;
        // Which styled lights touch which face decides the chart arrangement and the layers of every chart, so with more than one style in the level
        // a light edit can change the layout too. Without styled lights a lights-only edit keeps the geometry (and with it the layout, the samples,
        // the chunks) as it is.
        const bool styled = lightStyles(in.lights).size() > 1;
        // Only the lights of another style than 0 enter the style analysis: while those (and the surfaces) stay as they are, moving the sun or a plain
        // light cannot change a chart, so the layout and the texel samples stay too (this used to rebuild everything on any light edit of a styled level).
        std::vector<Light> styledLights;
        for (const Light& l : in.lights.lights) if (l.style != 0) styledLights.push_back(l);
        const bool styledSame = styled == prevStyled_ && sameLightLists(styledLights, prevStyledLights_);
        // a texture whose properties changed (transparency) is part of the occluders even when no face changed: rebuild, and re-bake around its faces
        const bool texturesTouched = !in.changedTextures.empty() && std::any_of(in.surfaces.begin(), in.surfaces.end(), [&](const LightSurface& s) { return in.changedTextures.count(s.texture) > 0; });
        // the playable area decides the density of the lightmap charts and the extent of the light volume
        const uint64_t playableFp = in.lights.playable ? in.lights.playable->fingerprint() : 0;
        const bool playableSame = playableFp == (prevPlayable_ ? prevPlayable_->fingerprint() : 0) && (playableFp == 0 || in.lights.level.lod == prevLod_);
        const bool geometrySame = built_ && playableSame && in.classes == prevClasses_ && solidsFp == prevSolids_ && sameSurfaces(prevSurfaces_, in.surfaces) && styledSame && !texturesTouched;
        const size_t chunksBefore = totalChunks();
        const int pagesBefore = baker_.layout().pageCount;
        const bool hadSurfaces = !prevSurfaces_.empty();
        bool geometryChanged = !built_ || !clientHasMesh_;
        const AlbedoLoader& loader = in.albedoLoader;
        if (!geometrySame) {
            const auto a1 = Clock::now();
            LightmapSettings layoutSettings = opt_.layout;
            layoutSettings.playable = in.lights.playable;
            layoutSettings.lod = in.lights.level.lod;
            std::unique_ptr<PlayableVisibility> visibility;
            std::shared_ptr<std::unordered_map<std::string, VisibilityAnswer>> stickyFresh;
            std::mutex visibilityM;
            std::vector<Aabb> layoutRegions;  // what changed since the last layout (needed before the layout for the sticky visibility)
            if (in.lights.playable && !in.lights.playable->empty() && (layoutSettings.lod.levels > 1 || layoutSettings.lod.removeUnseen) && layoutSettings.lod.cullUnseen && !opt_.skipVisibility) {
                // removing a surface needs a surer "nobody sees it" than choosing a density: many more viewpoints
                // what blocks the view is what the compiler's own visibility uses: the static world, and only its opaque textures
                const AlbedoLoader& texLoader = in.visibilityLoader;
                const auto blocks = [&texLoader](const LightSurface& s) {
                    if (s.classname != "worldspawn" && s.classname != "func_group" && s.classname != "func_detail") return false;  // doors, triggers: not walls
                    if (!texLoader) return true;
                    const auto img = texLoader(s.texture);
                    return !img || img->transparency.mode == TransparencyMode::Opaque;  // glass, water, cutouts: see-through
                };
                // built when the first face asks (an edit that leaves every face's answer in the cache never builds it)
                const auto ensureVisibility = [&]() -> const PlayableVisibility& {
                    std::lock_guard<std::mutex> lock(visibilityM);
                    if (!visibility) {
                        const auto v0 = Clock::now();
                        visibility = std::make_unique<PlayableVisibility>(*in.lights.playable, in.surfaces, in.classes, layoutSettings.lod.removeUnseen ? 768 : 96, blocks);
                        timings_.visibility = msBetween(v0, Clock::now());
                    }
                    return *visibility;
                };
                if (!opt_.stickyVisibility) {
                    layoutSettings.seen = [&](const std::vector<Vec3>& poly, const Vec3& n) { return ensureVisibility().seen(poly, n); };
                } else {
                    // the answers are only valid for this scope; a change of any of it asks everything again
                    uint64_t scope = playableFp ^ 0x9e3779b97f4a7c15ull;
                    scope = (scope ^ static_cast<uint64_t>(layoutSettings.lod.levels)) * 1099511628211ull;
                    scope = (scope ^ static_cast<uint64_t>(layoutSettings.lod.removeUnseen)) * 1099511628211ull;
                    scope = (scope ^ std::hash<std::string>{}(in.classes.toJson().dump())) * 1099511628211ull;
                    if (texturesTouched) scope ^= 0xabcdefull + visibilityScope_;
                    if (scope != visibilityScope_) { visibilityCache_.clear(); visibilityScope_ = scope; }
                    if (built_) layoutRegions = diffSurfaces(prevSurfaces_, in.surfaces);
                    const double R = opt_.stickyVisibilityRadius;
                    auto fresh = std::make_shared<std::unordered_map<std::string, VisibilityAnswer>>();
                    auto freshM = std::make_shared<std::mutex>();
                    const auto* cache = &visibilityCache_;
                    const std::vector<Aabb>* regions = &layoutRegions;
                    layoutSettings.seenFace = [&, fresh, freshM, cache, regions, R](const std::string& key, const std::vector<std::vector<Vec3>>& pieces, const Vec3& n) {
                        uint64_t h = 1469598103934665603ull;
                        Aabb box;
                        auto mix = [&](double d) { uint64_t b; std::memcpy(&b, &d, sizeof b); h = (h ^ b) * 1099511628211ull; };
                        for (const auto& piece : pieces) {
                            for (const Vec3& p : piece) { mix(p.x); mix(p.y); mix(p.z); box.add(p); }
                            h = (h ^ 0xFFull) * 1099511628211ull;
                        }
                        mix(n.x); mix(n.y); mix(n.z);
                        const auto it = cache->find(key);
                        if (it != cache->end() && it->second.geometry == h) {
                            const Aabb wide = box.expanded(R);
                            bool near = false;
                            for (const Aabb& r : *regions) if (r.intersects(wide)) { near = true; break; }
                            if (!near) {
                                std::lock_guard<std::mutex> lock(*freshM);
                                (*fresh)[key] = it->second;
                                return it->second.seen;
                            }
                        }
                        bool seen = false;
                        for (const auto& piece : pieces) if (ensureVisibility().seen(piece, n)) { seen = true; break; }
                        std::lock_guard<std::mutex> lock(*freshM);
                        (*fresh)[key] = VisibilityAnswer{h, seen};
                        return seen;
                    };
                    stickyFresh = fresh;
                }
            }
            styleWarnings_.clear();
            if (styled) {
                const OccluderMesh shadowMesh = buildOccluders(in.surfaces, in.classes);
                const TriangleBvh shadowBvh(shadowMesh);
                const StyleAnalysis analysis = analyseLightStyles(in.surfaces, in.classes, in.lights, shadowMesh.empty() ? nullptr : &shadowBvh);
                layoutSettings.faceStyles = analysis.faceStyles;
                styleWarnings_ = analysis.warnings;
                if (log_) for (const auto& w : analysis.warnings) log_("lighting: " + w);
            }
            const LightmapLayout layout = buildLightmapLayout(in.surfaces, in.classes, layoutSettings);
            layoutSettings.seen = nullptr;
            layoutSettings.seenFace = nullptr;
            if (stickyFresh) visibilityCache_ = std::move(*stickyFresh);
            visibility.reset();
            const auto a2 = Clock::now();
            std::vector<Aabb> regions = built_ ? diffSurfaces(prevSurfaces_, in.surfaces) : std::vector<Aabb>{};
            if (built_ && texturesTouched)
                for (const auto& s : in.surfaces) {
                    if (!in.changedTextures.count(s.texture)) continue;
                    Aabb box;
                    for (const auto& v : s.verts) box.add(v);
                    regions.push_back(box);
                }
            const auto a3 = Clock::now();
            timings_.surfaces = msBetween(a0, a1);
            timings_.layout = msBetween(a1, a2);
            timings_.diff = msBetween(a2, a3);
            baker_.setClasses(in.classes);
            baker_.setAlbedoLoader(loader);
            baker_.setGeometry(layout, in.surfaces, regions, in.solids);
            // the other style layers: a style needs a layer when a light of that style exists and some chart carries it
            std::set<int> wanted, haveLights;
            for (const auto& c : layout.charts) for (int st : c.styles) if (st != 0) wanted.insert(st);
            for (const Light& l : in.lights.lights) haveLights.insert(l.style);
            auto dropsResult = [&](int style) { if (denoiseInFlight_ && denoiseStyle_ == style) denoiseDiscard_ = true; };
            for (auto it = layers_.begin(); it != layers_.end();) {
                if (wanted.count(it->style) && haveLights.count(it->style)) { ++it; continue; }
                dropsResult(it->style);
                it = layers_.erase(it);
            }
            for (int st : wanted) {
                if (!haveLights.count(st)) continue;
                auto it = std::find_if(layers_.begin(), layers_.end(), [&](const StyleLayer& l) { return l.style == st; });
                if (it == layers_.end()) {
                    layers_.push_back({st, std::make_unique<LiveBaker>(opt_.live)});
                    if (sessionFactory_) layers_.back().baker->setSessionFactory(sessionFactory_);
                    dropsResult(st);
                    it = layers_.end() - 1;
                }
                it->baker->setClasses(in.classes);
                it->baker->setAlbedoLoader(loader);
                it->baker->setGeometry(layoutForStyle(layout, st), in.surfaces, regions, in.solids);
            }
            std::sort(layers_.begin(), layers_.end(), [](const StyleLayer& a, const StyleLayer& b) { return a.style < b.style; });
            uint64_t sig = 1469598103934665603ull;  // the style layers of every chart: part of the mesh (vertex mask), so a change replaces it
            for (const auto& c : layout.charts) { for (int st : c.styles) sig = (sig ^ static_cast<uint64_t>(st + 1)) * 1099511628211ull; sig = (sig ^ 0xFF) * 1099511628211ull; }
            timings_.geometry = msBetween(a3, Clock::now());
            geometryChanged = geometryChanged || sig != styleSig_ || !regions.empty() || in.surfaces.size() != prevSurfaces_.size() || chunksBefore != totalChunks() ||
                              pagesBefore != baker_.layout().pageCount || (!hadSurfaces && !in.surfaces.empty());
            styleSig_ = sig;
            prevSurfaces_ = std::move(in.surfaces);
            prevClasses_ = in.classes;
            prevSolids_ = solidsFp;
            prevSolidSet_ = in.solids;
            prevPlayable_ = in.lights.playable;
            prevLod_ = in.lights.level.lod;
        }
        prevStyled_ = styled;
        prevStyledLights_ = std::move(styledLights);
        const auto l0 = Clock::now();
        baker_.setLights(lightsOfStyle(in.lights, 0));
        for (auto& l : layers_) l.baker->setLights(lightsOfStyle(in.lights, l.style));
        timings_.lights = msBetween(l0, Clock::now());
        built_ = true;
        firstPassPending_ = true;
        if (geometryChanged) {
            // The mesh (and with it the Shell's geometry) is replaced: hold it back until the new surfaces have light (see meshReady()).
            ++rev_;
            if (!meshPending_) holdStart_ = Clock::now();
            meshPending_ = true;
        }  // lighting-only change: the Shell keeps its mesh, refined chunks arrive as tiles
        volStale_ = true;        // the probes are baked again once the lightmap has converged
        volRunning_ = false;
        entityLightDue_ = true;  // entities may have moved: resample the published grid
        statusDue_ = true;
    } catch (const std::exception& e) {
        emitError(emit, std::string("lighting: ") + e.what());
    }
}

void LightingService::emitError(const Emit& emit, const std::string& what) {
    std::vector<std::string> v;
    v.push_back(json{{"type", proto::kError}, {"message", what}}.dump());
    emit(std::move(v));
}

// ---- results leaving ---------------------------------------------------------------------------------------------------------------

bool LightingService::meshReady() const {
    const size_t total = totalChunks();
    if (allConverged() || msBetween(holdStart_, Clock::now()) >= opt_.maxHoldSeconds * 1000.0) return true;
    if (!clientHasMesh_) {  // the first one: a part of the level is enough
        const size_t done = total - std::min(total, staleChunks());
        return total == 0 || static_cast<double>(done) >= opt_.firstSendFraction * static_cast<double>(total);
    }
    return staleChunks() == 0;  // the Shell shows the old light of everything else meanwhile
}

void LightingService::releaseMesh(std::vector<std::string>& out) {
    const LightmapLayout& L = baker_.layout();
    const std::vector<int> layerStyles = styles();
    bool keep;
    {
        std::lock_guard<std::mutex> lock(mirror_.m);
        // the pages stay only when the Shell holds exactly the layers that exist now (a layer that appeared or went away starts from empty pages)
        keep = clientHasMesh_ && mirror_.valid && mirror_.atlas == L.settings.atlasSize && mirror_.pageCount == L.pageCount && mirror_.layerStyles == layerStyles;
    }
    const std::string mesh = litMeshMessage(keep);
    const std::string stylesMsg = lightStylesMessage();
    out.push_back(mesh);
    bool sendStyles = true;
    {
        std::lock_guard<std::mutex> lock(mirror_.m);
        sendStyles = !keep || stylesMsg != mirror_.styles;  // the Shell rebuilds its texture when it gets the table: only when it is news
        if (!keep) {
            mirror_.pages.clear();
            mirror_.directPages.clear();
            mirror_.atlas = L.settings.atlasSize;
            mirror_.pageCount = L.pageCount;
        }
        mirror_.layerStyles = layerStyles;
        mirror_.rev = rev_;
        mirror_.mesh = mesh;
        mirror_.styles = stylesMsg;
        mirror_.valid = true;
    }
    if (sendStyles) out.push_back(stylesMsg);
    for (auto& [style, b] : bakers()) {
        std::vector<int> ids = b->takeDirtyChunks();
        if (!keep) {  // fresh pages: everything computed (or inherited) so far; chunks that are still empty arrive when refined
            ids.clear();
            allChunksOf(*b, ids);
        }
        tilesFor(style, *b, ids, out);
    }
    meshPending_ = false;
    clientHasMesh_ = true;
    statusDue_ = true;
}

void LightingService::flush(const Emit& emit, bool force) {
    if (!built_) return;
    const auto now = Clock::now();
    if (!force && msBetween(lastFlush_, now) < opt_.flushIntervalMs) return;
    if (inFlight_ >= 2 && msBetween(lastEmit_, now) < 250) { flushPending_ = true; return; }  // the receiver is behind: merge into the next flush
    flushPending_ = false;
    lastFlush_ = now;
    std::vector<std::string> out;
    bool visual = false;
    if (meshPending_) {
        if (meshReady()) { releaseMesh(out); visual = true; }
    } else if (clientHasMesh_) {
        visual = flushDirty(out) > 0;
    } else {
        for (auto& [style, b] : bakers()) b->takeDirtyChunks();  // nobody to send to
    }
    if (visual && !firstTilesSent_) {
        timings_.firstTiles = msBetween(editStart_, Clock::now());
        firstTilesSent_ = true;
    }
    if (built_ && allConverged() && !anyDenoisePending() && timings_.converged == 0 && !meshPending_) timings_.converged = msBetween(editStart_, Clock::now());
    if (!meshPending_ && (statusDue_ || visual)) {
        statusDue_ = false;
        out.push_back(statusMessage());
    }
    if (!out.empty()) emit(std::move(out));
}

void LightingService::goIdle() {
    visibilityCache_.clear();  // lighting off / on: the next layout asks everything again
    // the Shell dropped its lightmap: the next enable sends everything again
    clientHasMesh_ = false;
    meshPending_ = false;
    flushPending_ = false;
    probes_.reset();
    volDone_ = volRunning_ = volSendDue_ = entityLightDue_ = false;
    volStale_ = true;
    vols_.clear();
    volsWork_.clear();
    std::lock_guard<std::mutex> lock(mirror_.m);
    mirror_.valid = false;
    mirror_.pages.clear();
    mirror_.directPages.clear();
    mirror_.layerStyles.clear();
    mirror_.mesh.clear();
    mirror_.styles.clear();
    mirror_.lightvol.clear();
    mirror_.entityLight.clear();
}

// ---- the work ------------------------------------------------------------------------------------------------------------------

bool LightingService::work(double budgetSeconds, const Emit& emit, const Interrupted& interrupted) {
    const auto t0 = Clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(Clock::now() - t0).count(); };
    if (!built_) return false;
    Vec3 camera;
    bool haveCamera;
    {
        std::lock_guard<std::mutex> lock(m_);
        camera = camera_;
        haveCamera = haveCamera_;
        flushCamera_ = camera_;
        flushHaveCamera_ = haveCamera_;
    }
    constexpr double kFirstPassGuardMs = 500;
    // an interruption counts only once the snapshot applied last has shown its first pass
    auto superseded = [&] {
        if (firstPassPending_ && msBetween(editStart_, Clock::now()) > kFirstPassGuardMs) firstPassPending_ = false;
        return !firstPassPending_ && interrupted && interrupted();
    };
    while (!allConverged()) {
        if (superseded()) return true;
        const auto s0 = Clock::now();
        // the base layer first (what everybody sees), then the style layers in order
        for (auto& [style, b] : bakers()) {
            if (b->converged()) continue;
            b->step(haveCamera ? &camera : nullptr, opt_.texelsPerStep);
            if (b->lastSessionSeconds() > 0) timings_.session = b->lastSessionSeconds() * 1000;
            break;
        }
        timings_.steps += msBetween(s0, Clock::now());
        flush(emit, firstPassPending_);  // progressive: what this step refined goes out now, not when the tick ends (the first pass of an edit at once)
        if (firstPassPending_ && !meshPending_ && staleChunks() == 0) firstPassPending_ = false;
        if (elapsed() >= budgetSeconds) break;
    }
    // Denoise the indirect light of a converged page. With a bake thread the filter runs on its own thread (startDenoise); in synchronous mode at
    // most one page per call (an OIDN run takes up to a few hundred ms), when time is left.
    if (anyDenoisePending() && !superseded()) {  // a page is pending once all its own chunks reached their last pass
        if (threaded()) {
            if (!denoiseInFlight_)
                for (auto& [style, b] : bakers()) if (b->denoisePending()) { startDenoise(style, *b); break; }
        } else if (elapsed() < budgetSeconds) {
            for (auto& [style, b] : bakers()) {
                if (!b->denoisePending()) continue;
                std::vector<int> page;
                const auto d0 = Clock::now();
                if (b->denoiseNext(&page)) {
                    timings_.denoise += msBetween(d0, Clock::now());
                    if (log_) {
                        char line[160];
                        std::snprintf(line, sizeof line, "lighting: denoised the indirect light of a page of style %d (%zu chunks) in %.0f ms", style, page.size(), msBetween(d0, Clock::now()));
                        log_(line);
                    }
                }
                statusDue_ = true;
                break;
            }
        }
    }
    if (log_ && baker_.denoiseError() != lastDenoiseError_) {
        lastDenoiseError_ = baker_.denoiseError();
        if (!lastDenoiseError_.empty()) log_("lighting: GI denoiser not applied: " + lastDenoiseError_);
    }
    if (log_) {
        const bool done = allConverged();
        if (!done && wasConverged_) runStart_ = baker_.stats();  // a new refinement run begins
        if (done && !wasConverged_) {
            const auto& now = baker_.stats();
            const double wall = now.stepSeconds - runStart_.stepSeconds, gpu = now.gpuSeconds - runStart_.gpuSeconds;
            char line[256];
            std::snprintf(line, sizeof line, "lighting: converged (%zu style layer%s), base layer %zu steps, %zu texels refined, %.0f ms in steps (%.0f ms waiting for the GPU = %.0f%%), evaluator %s",
                          layers_.size() + 1, layers_.empty() ? "" : "s", now.steps - runStart_.steps, now.texels - runStart_.texels, wall * 1000, gpu * 1000, wall > 0 ? 100 * gpu / wall : 0.0, baker_.evaluator().c_str());
            log_(line);
        }
        wasConverged_ = done;
    }
    bool volMore = false;
    if (!superseded()) {
        flush(emit, true);
        if (!meshPending_ && staleChunks() == 0) firstPassPending_ = false;
        volMore = workLightvol(budgetSeconds, t0, emit, interrupted);
    }
    return !allConverged() || anyDenoisePending() || flushPending_ || anyDirtyChunks() || meshPending_ || volMore;
}

std::vector<std::string> LightingService::tick(double budgetSeconds) {
    std::vector<std::string> out;
    if (threaded()) return out;
    const Emit emit = [&out](std::vector<std::string>&& m) { for (auto& s : m) out.push_back(std::move(s)); };
    if (enableGen_ != seenEnableGen_) { seenEnableGen_ = enableGen_; goIdle(); }  // switched on or off since: the Shell's lightmap is gone
    if (!enabled_) {
        if (statusDue_.exchange(false)) out.push_back(statusMessage());
        return out;
    }
    if (dirty_.exchange(false)) {
        const auto c0 = Clock::now();
        Inputs in = captureInputs();
        const double captureMs = msBetween(c0, Clock::now());
        applyInputs(std::move(in), emit);
        timings_.capture = captureMs;
    }
    work(budgetSeconds, emit, nullptr);
    return out;
}

// ---- the bake thread -----------------------------------------------------------------------------------------------------------

void LightingService::startWorker(Sink sink) {
    if (threaded()) return;
    sink_ = std::move(sink);
    quit_ = false;
    worker_ = std::thread([this] { workerMain(); });
}

void LightingService::stopWorker() {
    if (!worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(m_);
        quit_ = true;
        ++epoch_;
    }
    cv_.notify_all();
    worker_.join();
    if (denoiseThread_.joinable()) denoiseThread_.join();
    denoiseTask_.reset();
    denoiseInFlight_ = false;
    denoiseFinished_ = false;
}

void LightingService::delivered() {
    int v = inFlight_.load();
    while (v > 0 && !inFlight_.compare_exchange_weak(v, v - 1)) {}
    cv_.notify_one();
}

void LightingService::deliver(std::vector<std::string>&& messages) {
    lastEmit_ = Clock::now();
    ++inFlight_;
    if (sink_) sink_(std::move(messages));
}

void LightingService::flushInputs() {
    if (!threaded() || !enabled_ || !dirty_.exchange(false)) return;
    Inputs in;
    try {
        in = captureInputs();
    } catch (const std::exception& e) {
        std::vector<std::string> v;
        v.push_back(json{{"type", proto::kError}, {"message", std::string("lighting: ") + e.what()}}.dump());
        deliver(std::move(v));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_);
        pending_ = std::move(in);
        hasPending_ = true;
        pendingAt_ = Clock::now();
        ++epoch_;
    }
    cv_.notify_one();
}

void LightingService::startDenoise(int style, LiveBaker& baker) {
    auto task = std::make_unique<DenoiseTask>();
    if (!baker.beginDenoise(task->job)) {
        statusDue_ = true;  // the library is missing or failed: the status says why
        return;
    }
    task->quality = baker.denoiseQuality();
    denoiseTask_ = std::move(task);
    denoiseStyle_ = style;
    denoiseDiscard_ = false;
    denoiseInFlight_ = true;
    denoiseFinished_ = false;
    denoiseThread_ = std::thread([this] {
        denoiseTask_->result = LiveBaker::runDenoise(denoiseTask_->job, denoiseTask_->quality);
        {
            std::lock_guard<std::mutex> lock(m_);
            denoiseFinished_ = true;
        }
        cv_.notify_one();
    });
}

void LightingService::finishDenoise() {
    if (denoiseThread_.joinable()) denoiseThread_.join();
    std::vector<int> chunks;
    LiveBaker* b = nullptr;
    if (!denoiseDiscard_)
        for (auto& [style, cand] : bakers()) if (style == denoiseStyle_) b = cand;
    if (b && denoiseTask_ && b->finishDenoise(denoiseTask_->job, denoiseTask_->result, &chunks)) {
        timings_.denoise += denoiseTask_->result.seconds * 1000.0;
        if (log_) {
            char line[200];
            std::snprintf(line, sizeof line, "lighting: denoised the indirect light of a page of style %d (%zu chunks) in %.0f ms (on its own thread)", denoiseStyle_, chunks.size(), denoiseTask_->result.seconds * 1000.0);
            log_(line);
        }
    }
    denoiseTask_.reset();
    denoiseInFlight_ = false;
    denoiseDiscard_ = false;
    denoiseFinished_ = false;
    statusDue_ = true;
}

void LightingService::workerMain() {
    uint64_t seen = 0;
    bool busy = false;
    const Emit emit = [this](std::vector<std::string>&& m) { deliver(std::move(m)); };
    for (;;) {
        Inputs in;
        bool have = false;
        {
            std::unique_lock<std::mutex> lock(m_);
            if (busy) {
                if (flushPending_) cv_.wait_for(lock, std::chrono::milliseconds(2));  // only waiting for the receiver
                else if (volWaiting_) cv_.wait_for(lock, std::chrono::milliseconds(20), [&] { return quit_ || epoch_.load() != seen; });  // the probes wait for the editor to be idle
            } else {
                cv_.wait(lock, [&] { return quit_ || epoch_.load() != seen || statusDue_.load() || denoiseFinished_.load(); });
            }
            if (quit_) return;
            if (hasPending_) { in = std::move(pending_); hasPending_ = false; have = true; queuedMs_ = msBetween(pendingAt_, Clock::now()); }
            seen = epoch_.load();
        }
        if (denoiseInFlight_ && denoiseFinished_.load()) finishDenoise();
        if (enableGen_ != seenEnableGen_) { seenEnableGen_ = enableGen_; goIdle(); }
        if (!enabled_) {
            if (statusDue_.exchange(false)) {
                std::vector<std::string> v;
                v.push_back(statusMessage());
                deliver(std::move(v));
            }
            busy = false;
            continue;
        }
        if (have) applyInputs(std::move(in), emit);
        const Interrupted interrupted = [this, &seen] { return epoch_.load() != seen; };
        try {
            busy = work(0.2, emit, interrupted);
        } catch (const std::exception& e) {
            emitError(emit, std::string("lighting: ") + e.what());
            busy = false;
        }
        if (!busy && statusDue_.exchange(false)) {  // idle and nothing held back: the status alone
            std::vector<std::string> v;
            v.push_back(statusMessage());
            deliver(std::move(v));
        }
    }
}

// ---- a Shell that connects -----------------------------------------------------------------------------------------------------

std::vector<std::string> LightingService::fullState() {
    std::vector<std::string> out;
    if (!enabled_) return out;
    std::lock_guard<std::mutex> lock(mirror_.m);
    if (!mirror_.valid) return out;
    std::string mesh = mirror_.mesh;  // this client gets the pages whole (below), whatever it already holds: it must not keep them
    const size_t keep = mesh.find("\"keepPages\":true");  // (quotes inside strings are escaped, so this is the key)
    if (keep != std::string::npos) mesh.replace(keep, 16, "\"keepPages\":false");
    out.push_back(std::move(mesh));
    if (!mirror_.styles.empty()) out.push_back(mirror_.styles);
    const int atlas = mirror_.atlas;
    const int band = std::max(1, 200000 / (atlas * 8));  // rows per message
    for (const auto& [style, pages] : mirror_.pages) {
        for (size_t pg = 0; pg < pages.size(); ++pg) {
            const std::vector<uint16_t>& page = pages[pg];
            for (int y = 0; y < atlas; y += band) {
                const int h = std::min(band, atlas - y);
                const uint16_t* rows = &page[static_cast<size_t>(y) * atlas * 4];
                bool any = false;
                for (size_t i = 3; i < static_cast<size_t>(h) * atlas * 4 && !any; i += 4) any = rows[i] != 0;
                if (!any) continue;  // nothing baked in these rows
                std::vector<uint16_t> px(rows, rows + static_cast<size_t>(h) * atlas * 4);
                std::string msg = "{\"type\":\"lightmap_tiles\",\"rev\":" + std::to_string(mirror_.rev) + ",\"atlas\":" + std::to_string(atlas) + ",\"style\":" + std::to_string(style) + ",\"tiles\":[";
                std::vector<uint16_t> dpx;
                if (style == 0 && pg < mirror_.directPages.size() && !mirror_.directPages[pg].empty()) {
                    const uint16_t* drows = &mirror_.directPages[pg][static_cast<size_t>(y) * atlas * 4];
                    dpx.assign(drows, drows + static_cast<size_t>(h) * atlas * 4);
                }
                appendTile(msg, true, static_cast<int>(pg), 0, y, atlas, h, px, dpx.empty() ? nullptr : &dpx);
                msg += "]}";
                out.push_back(std::move(msg));
            }
        }
    }
    if (!mirror_.lightvol.empty()) out.push_back(mirror_.lightvol);
    if (!mirror_.entityLight.empty()) out.push_back(mirror_.entityLight);
    out.push_back(mirror_.status.empty() ? statusLive().dump() : mirror_.status);
    return out;
}

}  // namespace core
