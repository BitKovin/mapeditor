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
    std::string outPath;           // assets relative .bsp path
    LightingService::Options lighting;  // lighting.json, as the live view uses it
    bool bake = true;              // false: geometry only (no lightmaps, no light grid)
    double maxBakeSeconds = 0;     // 0 = until converged
    BspSettings settings;
};

struct BspExportResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::vector<std::string> warnings;
    BspOutput::Stats stats;
    std::string bspPath, lightmapDir;  // assets relative
    double seconds = 0, bakeSeconds = 0;
    std::string device;                // what baked the lightmaps
    nlohmann::json toJson() const;
};

/// Runs the whole export on the calling thread. `progress` (optional) is called with a stage name and 0..1 (also from helper threads);
/// `cancelled` (optional) is polled between steps.
BspExportResult exportBsp(const BspExportRequest& request, const ProgressFn& progress = {}, const std::function<bool()>& cancelled = {});

/// One export at a time on its own thread, polled by the Router.
class BspExportJob {
public:
    ~BspExportJob();
    bool running() const { return running_; }
    /// False when one is already running.
    bool start(BspExportRequest request);
    void cancel() { cancel_ = true; }
    /// `export_progress` messages since the last call (at most one, the latest) and the `export_result` once finished.
    std::vector<std::string> poll();

private:
    std::thread thread_;
    std::atomic<bool> running_{false}, cancel_{false}, finished_{false};
    std::mutex m_;
    std::string stage_;
    double fraction_ = 0;
    bool progressDue_ = false;
    BspExportResult result_;
    std::string outPath_;
};

}  // namespace core
