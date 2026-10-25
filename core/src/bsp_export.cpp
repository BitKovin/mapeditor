// The BSP export driver (REQUIREMENTS §25, DESIGN §12): own Scene -> final bake -> compileBsp -> files.
#include "core/bsp_export.h"
#include "core/exporter.h"
#include "core/albedo.h"
#include "core/exr.h"
#include "core/relief_geometry.h"
#include "core/scene.h"
#include "core/textures.h"
#include <cstring>
#include <chrono>
#include <fstream>
#include <unordered_map>

namespace core {

namespace fs = std::filesystem;
using nlohmann::json;

std::string sandboxedExportPath(const std::string& rel, const std::string& extension) {
    const fs::path p(rel);
    if (rel.empty() || p.is_absolute() || p.has_root_name() || p.has_root_directory()) throw std::runtime_error("the export path must be inside the assets folder: " + rel);
    const fs::path norm = p.lexically_normal();
    for (const auto& part : norm) if (part == "..") throw std::runtime_error("the export path escapes the assets folder: " + rel);
    if (norm.extension() != extension) throw std::runtime_error("the export path must end in " + extension + ": " + rel);
    return norm.generic_string();
}

void writeLightmapPages(const fs::path& lmDir, const std::vector<BspLightmapPage>& pages) {
    std::error_code ec;
    if (fs::exists(lmDir, ec))
        for (const auto& f : fs::directory_iterator(lmDir, ec))
            if (f.path().filename().string().rfind("lm_", 0) == 0 && (f.path().extension() == ".exr" || f.path().extension() == ".jpg")) fs::remove(f.path(), ec);  // pages of an older export
    if (!pages.empty()) fs::create_directories(lmDir);
    for (size_t i = 0; i < pages.size(); ++i) {
        char name[32];
        if (!pages[i].jpg.empty()) {  // an 8 bit page (bsp_compile.h lightmapJpeg)
            std::snprintf(name, sizeof(name), "lm_%04d.jpg", static_cast<int>(i));
            std::ofstream f(lmDir / name, std::ios::binary);
            f.write(reinterpret_cast<const char*>(pages[i].jpg.data()), static_cast<std::streamsize>(pages[i].jpg.size()));
            if (!f) throw std::runtime_error("cannot write " + (lmDir / name).string());
            continue;
        }
        std::snprintf(name, sizeof(name), "lm_%04d.exr", static_cast<int>(i));
        if (!writeExr((lmDir / name).string(), pages[i].size, pages[i].size, pages[i].rgb.data())) throw std::runtime_error("cannot write " + (lmDir / name).string());
    }
}

namespace {

bool isStatic(const std::string& c, const LightingClasses& classes) {
    return c == "worldspawn" || c == "func_group" || c == "func_detail" || classes.isLightmapped(c);  // lightmapped = static (moving ones use the light volume)
}

}  // namespace

json BspExportResult::toJson() const {
    json s = {{"surfaces", stats.surfaces}, {"decals", stats.decals}, {"sky", stats.skySurfaces}, {"laidOut", stats.laidOutSurfaces}, {"droppedSurfaces", stats.droppedSurfaces}, {"brushes", stats.brushes}, {"models", stats.models},
              {"leafs", stats.leafs}, {"clusters", stats.clusters}, {"lightmapPages", stats.lightmapPages}, {"gridPoints", stats.gridPoints},
              {"palette", stats.palette}, {"visCell", stats.visCell}, {"visSeconds", stats.visSeconds}};
    return {{"type", "export_result"}, {"ok", ok}, {"cancelled", cancelled}, {"error", error}, {"warnings", warnings}, {"stats", s},
            {"format", format}, {"bsp", bspPath}, {"lightmaps", lightmapDir}, {"seconds", seconds}, {"bakeSeconds", bakeSeconds}, {"device", device}};
}

bool prepareExport(const BspExportRequest& req, const ProgressFn& progress, const std::function<bool()>& cancelled, BspExportResult& r, PreparedExport& prepared) {
    auto stop = [&]() { return cancelled && cancelled(); };
    auto report = [&](const std::string& stage, double f) { if (progress) progress(stage, f); };
    {
        Scene scene(req.assetsRoot);
        scene.loadGameProfile();
        const Scene::UpdateResult up = scene.applyUpdate(req.document);
        if (!up.ok) throw std::runtime_error("the map is not valid: " + up.error);
        if (req.relief.is_object()) scene.setReliefGeometry(req.relief);

        // the map, typed, with its prefabs
        const Document doc = documentFromJson(req.document);
        std::unordered_map<std::string, std::unique_ptr<Document>> cache;
        const DocumentLoader loader = [&](const std::string& p) -> const Document& {
            const std::string key = normalizeAssetPath(p);
            auto& slot = cache[key];
            if (!slot) slot = std::make_unique<Document>(documentFromJson(scene.readPrefabJson(key)));
            return *slot;
        };
        FlattenResult flat = flattenForCompile(doc, loader, FlattenOptions{}, req.mapPath);
        for (auto& p : flat.problems) r.warnings.push_back(p);
        std::vector<ResolvedEntity> prefabs;
        for (auto& e : resolvePrefabs(doc, loader, ResolveOptions{}, req.mapPath)) if (e.instancePath.size() > 1) prefabs.push_back(std::move(e));
        const LightingClasses classes = scene.lightingClasses();

        BspInput in;
        in.entities = std::move(flat.entities);
        in.surfaces = scene.lightSurfaces(true);  // sky faces too: the lighting never sees them, the BSP draws them
        in.classes = classes;
        const SolidSet statics = collectSolids(doc, prefabs, [&](const std::string& c) { return isStatic(c, classes); });
        in.solids = &statics;
        auto albedo = std::make_shared<AlbedoLibrary>(req.assetsRoot, scene.profile());
        in.opaque = [albedo](const std::string& t) {
            const auto img = albedo->get(t);
            return !img || img->transparency.mode == TransparencyMode::Opaque;
        };
        in.decals = collectDecals(doc, prefabs);
        // the authored faces (world space, prefabs resolved) and the textures / groups: the texture layout as the editor shows it (texplan.h)
        std::map<std::pair<std::string, std::string>, const Brush*> brushIndex;
        for (const auto& e : doc.entities) for (const auto& b : e.brushes) brushIndex[{"", b.id}] = &b;
        for (const auto& r : prefabs) for (const auto& b : r.entity.brushes) brushIndex[{instanceString(r.instancePath), b.id}] = &b;
        in.parentFace = [&brushIndex](const std::string& inst, const std::string& id, uint32_t face) -> std::pair<const Brush*, const BrushFace*> {
            const auto it = brushIndex.find({inst, id});
            if (it == brushIndex.end()) return {nullptr, nullptr};
            for (const auto& f : it->second->faces) if (f.id == face) return {it->second, &f};
            return {it->second, nullptr};
        };
        texplan::TexEnvironment texEnv(req.assetsRoot, scene.profile());
        in.texInfo = texEnv.infoFn();
        in.texGroups = texEnv.groupFn();
        if (!req.relief.is_object()) {
            // no editor sent its relief geometry (command line): build it the same way, so the bake lights the relief (relief_geometry.h)
            std::map<std::string, json> faces;
            std::map<std::string, std::vector<float>> trisOf;
            texplan::ReliefBudget budget;
            budget.left = 400000;
            for (const LightSurface& sf : in.surfaces) {
                const auto parent = in.parentFace(sf.brushInstance, sf.brushId, sf.faceId);
                if (!parent.first || !parent.second) continue;
                const auto plan = texplan::planForFace(*parent.first, *parent.second, in.texInfo, in.texGroups);
                bool relief = false;
                if (plan) { for (const auto& t : plan->textures) relief = relief || texplan::reliefOf(in.texInfo, t) != nullptr; }
                else relief = texplan::reliefOf(in.texInfo, parent.second->texture.empty() ? std::string("dev/grey") : parent.second->texture) != nullptr;
                if (!relief) continue;
                std::vector<texplan::V3> poly;
                for (const Vec3& v : sf.verts) poly.push_back(texplan::v3(v));
                const auto tris = texplan::texturePolygon(poly, texplan::v3(sf.normal), parent.first, parent.second, in.texInfo, in.texGroups, &budget);
                const std::string key = reliefKey(sf.instance, sf.entityId, sf.brushInstance, sf.brushId, sf.faceId, sf.side);
                if (!faces.count(key))
                    faces[key] = json{{"i", sf.instance}, {"e", sf.entityId}, {"bi", sf.brushInstance}, {"b", sf.brushId}, {"f", sf.faceId}, {"s", sf.side == FaceSide::Back ? 1 : 0},
                                      {"n", {sf.normal.x, sf.normal.y, sf.normal.z}}, {"d", sf.normal.dot(sf.verts[0])}};
                auto& v = trisOf[key];
                for (const auto& t : tris) for (const auto& p : t.pos) for (double x : p) v.push_back(static_cast<float>(x));
            }
            if (!faces.empty()) {
                json msg = {{"type", "relief_geometry"}, {"faces", json::array()}};
                for (auto& [key, f] : faces) {
                    const auto& v = trisOf[key];
                    if (v.size() < 9) continue;
                    std::vector<unsigned char> bytes(v.size() * 4);
                    std::memcpy(bytes.data(), v.data(), bytes.size());
                    f["v"] = base64Encode(bytes);
                    msg["faces"].push_back(f);
                }
                scene.setReliefGeometry(msg);
            }
        }
        in.settings = req.settings;
        in.settings.textureRoot = scene.profile().textureRoot;
        in.settings.contents = BspContentRules::fromJson(scene.profile().bsp, &r.warnings);
        for (const auto& e : doc.entities)
            if (e.classname == "worldspawn") {
                in.settings.sky = skySettingsOf(e, [&]() { return listSkyboxes(req.assetsRoot); }, &r.warnings);  // REQUIREMENTS §28
                if (in.settings.sky.mode != AutoSky::Off && !in.settings.sky.texture.empty() && !readSkybox(req.assetsRoot, in.settings.sky.texture))
                    r.warnings.push_back("auto sky: " + in.settings.sky.texture + " has no image in env/ (the engine would draw nothing)");
                if (const auto it = e.keyvalues.find("_vis_cell"); it != e.keyvalues.end()) {
                    try { const double c = std::stod(it->second); if (c > 0) in.settings.visCell = c; } catch (...) { r.warnings.push_back("_vis_cell is not a number"); }
                }
            }
                const LevelSettings lv = levelSettingsOf(e);  // the exposure the lightmap fit judges by (the level's, else lighting.json's)
                in.settings.lightmapExposure = lv.exposure > 0 ? lv.exposure : req.lighting.exposure;

                {  // 8 bit lightmap pages as .jpg (bsp_compile.h): _lightmap_jpg 1; _lightmap_jpg_srgb 1 stores the sRGB curve of the light; _lightmap_jpg_quality
                    auto flag = [&](const char* key) { const auto k = e.keyvalues.find(key); return k != e.keyvalues.end() && k->second != "0" && !k->second.empty(); };
                    in.settings.lightmapJpeg = flag("_lightmap_jpg");
                    in.settings.lightmapSrgb = flag("_lightmap_jpg_srgb");
                    if (const auto q = e.keyvalues.find("_lightmap_jpg_quality"); q != e.keyvalues.end()) {
                        try { in.settings.lightmapJpegQuality = std::clamp(std::stoi(q->second), 50, 100); } catch (...) { r.warnings.push_back("_lightmap_jpg_quality is not a number"); }
                    }
                }
        // final bake: the live pipeline, synchronously, at the level's bake quality
        std::unique_ptr<LightingService> svc;
        LightmapLayout layout;
        if (req.bake) {
            LevelSettings level;
            for (const auto& e : doc.entities) if (e.classname == "worldspawn") level = levelSettingsOf(e);
            LightingService::Options opt = req.lighting;
            opt.qualityScale = level.bakeQuality > 0 ? level.bakeQuality : 2.0;
            opt.tiles = false;  // no client: the pages are read from the bakers when it converged
            svc = std::make_unique<LightingService>(scene, opt);
            svc->setEnabled(true);
            const auto b0 = std::chrono::steady_clock::now();
            int idle = 0;
            for (;;) {
                if (stop()) { r.cancelled = true; r.error = "cancelled"; return false; }
                svc->tick(0.25);
                // progress: passes of the least refined layer, then the light volume
                double f = 1;
                for (int st : svc->styles())
                    if (const LiveBaker* b = svc->layerBaker(st); b && b->chunkCount() > 0)
                        f = std::min(f, double(b->minPasses()) / std::max(1, b->settings().maxPasses));
                const bool lmBusy = svc->busy(), volBusy = svc->lightvolPending();
                report(lmBusy ? "baking lightmaps" : "baking light volume", (lmBusy ? 0.6 * f : 0.6 + 0.2 * svc->lightvolProgress()) * 0.8);
                if (!lmBusy && !volBusy) { if (++idle > 1) break; } else idle = 0;
                if (req.maxBakeSeconds > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - b0).count() > req.maxBakeSeconds) {
                    r.warnings.push_back("the bake was stopped after " + std::to_string(static_cast<int>(req.maxBakeSeconds)) + " s before it converged");
                    break;
                }
            }
            r.bakeSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - b0).count();
            r.device = svc->baker().evaluator();
            for (const auto& w : svc->styleWarnings()) r.warnings.push_back(w);
            layout = svc->baker().layout();
            if (!layout.charts.empty()) {
                in.layout = &layout;
                for (int st : svc->styles())
                    if (const LiveBaker* b = svc->layerBaker(st)) in.pages[st] = b->snapshot();
            }
            for (int st : svc->styles())
                if (svc->lightvol(st).count() > 0) in.lightvol[st] = svc->lightvol(st);
            if (!svc->lightvolReady()) in.lightvol.clear();
            svc.reset();  // everything the compile needs was copied out: the bakers hold gigabytes on a big level
        }
        if (stop()) { r.cancelled = true; r.error = "cancelled"; return false; }

        BspOutput o = compileBsp(in, [&](const std::string& stage, double f) { report(stage, (req.bake ? 0.8 : 0.0) + f * (req.bake ? 0.18 : 0.98)); });
        prepared.lightvol = std::move(in.lightvol);
        for (const auto& w : o.warnings) r.warnings.push_back(w);
        r.stats = o.stats;

        prepared.bsp = std::move(o);
    }
    return true;
}

BspExportResult exportBsp(const BspExportRequest& req, const ProgressFn& progress, const std::function<bool()>& cancelled) {
    BspExportResult r;
    r.format = "rbsp";
    const auto t0 = std::chrono::steady_clock::now();
    auto report = [&](const std::string& stage, double f) { if (progress) progress(stage, f); };
    try {
        const std::string out = sandboxedExportPath(req.outPath, ".bsp");
        PreparedExport p;
        if (!prepareExport(req, progress, cancelled, r, p)) return r;
        const BspOutput& o = p.bsp;

        // files: <name>.bsp and <name>/lm_NNNN.exr
        report("writing files", 0.99);
        const fs::path bspFile = req.assetsRoot / fs::path(out);
        fs::create_directories(bspFile.parent_path());
        writeLightmapPages(bspFile.parent_path() / bspFile.stem(), o.lightmaps);
        {
            const fs::path tmp = bspFile.string() + ".tmp";
            std::ofstream f(tmp, std::ios::binary);
            f.write(reinterpret_cast<const char*>(o.bsp.data()), static_cast<std::streamsize>(o.bsp.size()));
            f.close();
            if (!f) throw std::runtime_error("cannot write " + bspFile.string());
            fs::rename(tmp, bspFile);
        }
        r.bspPath = out;
        r.lightmapDir = (fs::path(out).parent_path() / fs::path(out).stem()).generic_string();
        r.ok = true;
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = e.what();
    }
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    report("done", 1);
    return r;
}

BspExportJob::~BspExportJob() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

bool BspExportJob::start(BspExportRequest request) {
    if (running_) return false;
    if (thread_.joinable()) thread_.join();
    running_ = true;
    cancel_ = false;
    finished_ = false;
    {
        std::lock_guard<std::mutex> lock(m_);
        stage_ = "starting";
        fraction_ = 0;
        progressDue_ = true;
    }
    thread_ = std::thread([this, req = std::move(request)]() {
        BspExportResult res = exporterFor(req.format).run(
            req,
            [this](const std::string& stage, double f) {
                std::lock_guard<std::mutex> lock(m_);
                stage_ = stage;
                fraction_ = std::max(fraction_, f);
                progressDue_ = true;
            },
            [this]() { return cancel_.load(); });
        {
            std::lock_guard<std::mutex> lock(m_);
            result_ = std::move(res);
        }
        finished_ = true;
    });
    return true;
}

std::vector<std::string> BspExportJob::poll() {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(m_);
    if (progressDue_ && running_) {
        out.push_back(json{{"type", "export_progress"}, {"stage", stage_}, {"fraction", fraction_}}.dump());
        progressDue_ = false;
    }
    if (finished_ && running_) {
        out.push_back(result_.toJson().dump());
        running_ = false;
    }
    return out;
}

}  // namespace core
