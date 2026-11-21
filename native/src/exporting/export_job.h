// exporting/export_job -- File > Export / Play map without the Shell: the document as the editor holds it goes to the Core exporter (core/exporter.h) on a worker thread.
//
// Owns: makeExportRequest (a DocumentVersion + the game folder -> core::ExportRequest: the game profile exporter and the folder lighting.json, the document as the JSON the Core
// reads), ExportJob (one export at a time on its own thread: progress to poll, cancel, the result), launchCommand (the {map} / {mapname} / {bsp} / {game} placeholders of the game
// start line, the Shell Settings > Run game).
// Thread: the UI thread calls start / cancel / progress / take; the exporter runs on the job's own thread. Spec: 08_MILESTONES.md T9.3, 06_LIGHTING.md section 13.
// Deviation from 06 section 13 (T9.1 / T9.2 not built): the request still carries the document as JSON and the Core's GPU code still makes its own device.
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <core/exporter.h>

#include "doc/document.h"
#include "exporting/launch.h"

namespace native::exporting {

struct ExportOptions {
    std::filesystem::path assetsRoot;
    std::string outPath;          // assets relative, with or without the exporter extension (it is replaced)
    bool bake = true;             // false: geometry only
    double maxBakeSeconds = 0;    // 0 = until converged
};

// The request for the game folder exporter. `warning`: why the profile's choice was replaced (empty when nothing was). Throws on a broken profile or lighting.json.
core::ExportRequest makeExportRequest(const doc::DocumentVersion& version, const ExportOptions& options, std::string* warning = nullptr);

// The exporter of the game folder: its id, its menu name ("rBSP") and the extension of its main file (".bsp").
struct ExporterInfo {
    std::string id, name, extension;
};
ExporterInfo exporterOf(const std::filesystem::path& assetsRoot);

class ExportJob {
public:
    ~ExportJob();
    bool running() const { return running_; }
    // False when an export is already running. The request is moved to the job thread.
    bool start(core::ExportRequest request);
    void cancel() { cancel_ = true; }
    struct Progress {
        std::string stage;
        double fraction = 0;
    };
    Progress progress() const;
    // The result once the export finished (then it is forgotten); null while running or when none is waiting. A failure that threw is a result with ok = false.
    std::shared_ptr<const core::ExportResult> take();
    // Waits for the thread (tests); false on timeout.
    bool wait(int timeoutMs);

private:
    mutable std::mutex m_;
    std::thread thread_;
    std::atomic<bool> running_{false}, cancel_{false};
    Progress progress_;
    std::shared_ptr<const core::ExportResult> result_;
};

} // namespace native::exporting
