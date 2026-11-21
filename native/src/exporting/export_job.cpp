#include "exporting/export_job.h"

#include <chrono>
#include <exception>

#include <core/lighting_service.h>
#include <core/profile.h>

#include "doc/io.h"

namespace native::exporting {

namespace fs = std::filesystem;

core::ExportRequest makeExportRequest(const doc::DocumentVersion& version, const ExportOptions& options, std::string* warning) {
    const core::GameProfile profile = core::loadProfile(options.assetsRoot);
    std::string formatWarning;
    const core::Exporter& exporter = core::exporterFor(profile.exporter, &formatWarning);
    if (warning) *warning = formatWarning;
    core::ExportRequest r;
    r.assetsRoot = options.assetsRoot;
    r.document = nlohmann::json::parse(doc::serializeDocument(version));
    r.mapPath = version.path;
    r.format = exporter.id;
    r.outPath = core::withExporterExtension(options.outPath, exporter);
    r.lighting = core::LightingService::loadOptions(options.assetsRoot / "lighting.json");
    r.bake = options.bake;
    r.maxBakeSeconds = options.maxBakeSeconds;
    return r;
}

ExporterInfo exporterOf(const fs::path& assetsRoot) {
    const core::GameProfile profile = core::loadProfile(assetsRoot);
    const core::Exporter& e = core::exporterFor(profile.exporter);
    return {e.id, e.name, e.extension};
}

ExportJob::~ExportJob() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

bool ExportJob::start(core::ExportRequest request) {
    if (running_) return false;
    if (thread_.joinable()) thread_.join();
    cancel_ = false;
    {
        std::lock_guard<std::mutex> lock(m_);
        progress_ = {"starting", 0.0};
        result_.reset();
    }
    running_ = true;
    thread_ = std::thread([this, request = std::move(request)] {
        auto result = std::make_shared<core::ExportResult>();
        try {
            const core::Exporter& exporter = core::exporterFor(request.format);
            *result = exporter.run(
                request,
                [this](const std::string& stage, double f) {
                    std::lock_guard<std::mutex> lock(m_);
                    progress_ = {stage, f};
                },
                [this] { return cancel_.load(); });
        } catch (const std::exception& e) {
            result->ok = false;
            result->error = e.what();
        }
        {
            std::lock_guard<std::mutex> lock(m_);
            result_ = result;
        }
        running_ = false;
    });
    return true;
}

ExportJob::Progress ExportJob::progress() const {
    std::lock_guard<std::mutex> lock(m_);
    return progress_;
}

std::shared_ptr<const core::ExportResult> ExportJob::take() {
    std::lock_guard<std::mutex> lock(m_);
    if (running_) return nullptr;
    auto r = std::move(result_);
    result_.reset();
    return r;
}

bool ExportJob::wait(int timeoutMs) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (running_) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

} // namespace native::exporting
