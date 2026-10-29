#pragma once
#include "core/bsp_compile.h"
#include "core/lighting_service.h"
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace core {

// The BSP export (REQUIREMENTS §25, DESIGN §12): the document as the editor holds it -> final bake -> rBSP + EXR lightmaps.
// It works on its own Scene built from a copy of the document, so it can run on a thread while the editor keeps editing and baking.

struct BspExportRequest {
    std::filesystem::path assetsRoot;
    nlohmann::json document;       // the document (as in document_sync)
    std::string mapPath;           // assets relative path of the map ("" = unsaved), for prefab cycle detection
    nlohmann::json relief;         // the last relief_geometry message (optional)
    std::string outPath;           // assets relative path of the main file (.bsp, or the format's extension)
    std::string format = "rbsp";   // the exporter (exporter.h; the game profile's "exporter")
    LightingService::Options lighting;  // lighting.json, as the live view uses it
    bool bake = true;              // false: geometry only (no lightmaps, no light grid)
    double maxBakeSeconds = 0;     // 0 = until converged
    BspSettings settings;
    bool rbspLightmapEncoding = true;  // false (the OBJ exporter): pages as plain .exr light, no 8 bit .jpg pages and no per-chart light scale in the vertex colours
};

struct BspExportResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::vector<std::string> warnings;
    BspOutput::Stats stats;
    std::string format = "rbsp";       // the exporter that wrote the files
    std::string bspPath, lightmapDir;  // assets relative: the main file (the .bsp, or the format's), the lightmap folder
    double seconds = 0, bakeSeconds = 0;
    std::string device;                // what baked the lightmaps
    std::shared_ptr<const VisResult> vis;  // the baked visibility of the level (the editor's visibility view reads it)
    nlohmann::json toJson() const;
};

/// What every exporter starts from (exporter.h): the final bake and the rBSP compiled in memory (its lumps hold the geometry as the game gets it).
struct PreparedExport {
    BspOutput bsp;
    std::map<int, LightvolGrid> lightvol;  // light style -> the baked light volume (editor space); empty without a bake
};
/// Builds the scene, bakes (request.bake) and compiles. Throws on errors; false when cancelled (`result.cancelled` / `error` set). Warnings,
/// stats, bake time and device go into `result`.
bool prepareExport(const BspExportRequest& request, const ProgressFn& progress, const std::function<bool()>& cancelled, BspExportResult& result, PreparedExport& out);
/// `rel` normalised, or throws when it leaves the assets folder or does not end in `extension`.
std::string sandboxedExportPath(const std::string& rel, const std::string& extension);
/// lm_0000.exr, lm_0001.exr, ... in `dir` (pages of an older export are removed first; no folder when there are no pages).
void writeLightmapPages(const std::filesystem::path& dir, const std::vector<BspLightmapPage>& pages);

/// The rBSP exporter: runs the whole export on the calling thread. `progress` (optional) is called with a stage name and 0..1 (also from helper threads);
/// `cancelled` (optional) is polled between steps.
BspExportResult exportBsp(const BspExportRequest& request, const ProgressFn& progress = {}, const std::function<bool()>& cancelled = {});

/// One export at a time on its own thread, polled by the Router.
class BspExportJob {
public:
    ~BspExportJob();
    bool running() const { return running_; }
    /// The baked visibility of the last export that finished (null before the first).
    std::shared_ptr<const VisResult> lastVisibility() const { std::lock_guard<std::mutex> lock(m_); return lastVis_; }
    /// False when one is already running.
    bool start(BspExportRequest request);
    void cancel() { cancel_ = true; }
    /// `export_progress` messages since the last call (at most one, the latest) and the `export_result` once finished.
    std::vector<std::string> poll();

private:
    std::thread thread_;
    std::atomic<bool> running_{false}, cancel_{false}, finished_{false};
    mutable std::mutex m_;
    std::shared_ptr<const VisResult> lastVis_;
    std::string stage_;
    double fraction_ = 0;
    bool progressDue_ = false;
    BspExportResult result_;
    std::string outPath_;
};

}  // namespace core
