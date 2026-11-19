// doc/io -- see io.h.
#include "doc/io.h"

#include <core/parallel.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "base/hash.h"
#include "base/timing.h"

namespace native::doc {

namespace {

// Typed parse of one entity node and its brushes. `ej` is consumed (its brush JSON moves into the brush nodes).
std::shared_ptr<const EntityNode> makeEntityNode(Json&& ej) {
    if (!ej.is_object()) throw std::runtime_error("entity must be a JSON object");
    auto node = std::make_shared<EntityNode>();
    Json brushes;
    if (ej.contains("brushes")) {
        node->hasBrushesKey = true;
        brushes = std::move(ej["brushes"]);
        ej.erase("brushes");
        if (!brushes.is_array()) throw std::runtime_error("entity brushes must be an array");
    }
    node->typed = core::entityFromJson(ej);   // without brushes: typed.brushes stays empty
    node->raw = std::move(ej);
    node->brushes.reserve(brushes.size());
    for (Json& bj : brushes) {
        auto b = std::make_shared<BrushNode>();
        b->brush = core::brushFromJson(bj);
        b->contentHash = base::hashJson(bj);
        b->raw = std::move(bj);
        node->brushes.push_back(std::move(b));
    }
    return node;
}

std::shared_ptr<const InstanceNode> makeInstanceNode(Json&& pj) {
    if (!pj.is_object()) throw std::runtime_error("prefab instance must be a JSON object");
    auto node = std::make_shared<InstanceNode>();
    node->typed = core::instanceFromJson(pj);
    node->raw = std::move(pj);
    return node;
}

LoadResult fail(std::string message, LoadResult r = {}) {
    r.version.reset();
    r.error = std::move(message);
    return r;
}

} // namespace

LoadResult loadDocumentText(const std::string& text, const std::string& assetsRelativePath) {
    base::Stopwatch total, step;
    LoadResult result;
    result.bytes = text.size();

    Json root = Json::parse(text, nullptr, false);
    result.timings.parseMs = step.elapsedMs();
    if (root.is_discarded()) return fail("not valid JSON", result);
    if (!root.is_object()) return fail("the document must be a JSON object", result);
    if (!root.contains("schemaVersion") || !root["schemaVersion"].is_string())
        return fail("the document has no schemaVersion", result);
    if (root["schemaVersion"].get<std::string>() != core::kSchemaVersion)
        return fail("unsupported schemaVersion \"" + root["schemaVersion"].get<std::string>() + "\" (expected \"" +
                        core::kSchemaVersion + "\")",
                    result);

    auto version = std::make_shared<DocumentVersion>();
    version->version = nextVersionNumber();
    version->path = assetsRelativePath;
    version->hasEntitiesKey = root.contains("entities");
    version->hasInstancesKey = root.contains("prefabInstances");

    step.restart();
    // Entities in parallel: each task parses one entity and its brushes and moves their JSON into nodes.
    std::vector<Json> entityJson;
    if (version->hasEntitiesKey) {
        if (!root["entities"].is_array()) return fail("entities must be an array", result);
        entityJson = std::move(root["entities"].get_ref<Json::array_t&>());
        root.erase("entities");
    }
    std::vector<Json> instanceJson;
    if (version->hasInstancesKey) {
        if (!root["prefabInstances"].is_array()) return fail("prefabInstances must be an array", result);
        instanceJson = std::move(root["prefabInstances"].get_ref<Json::array_t&>());
        root.erase("prefabInstances");
    }
    version->rootRaw = std::move(root);

    version->entities.resize(entityJson.size());
    std::mutex errorMutex;
    std::string firstError;
    try {
        core::parallelFor(entityJson.size(), 8, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                try {
                    version->entities[i] = makeEntityNode(std::move(entityJson[i]));
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (firstError.empty()) firstError = "entity " + std::to_string(i) + ": " + e.what();
                }
            }
        });
        for (size_t i = 0; i < instanceJson.size(); ++i) {
            try {
                version->instances.push_back(makeInstanceNode(std::move(instanceJson[i])));
            } catch (const std::exception& e) {
                if (firstError.empty()) firstError = "prefab instance " + std::to_string(i) + ": " + e.what();
            }
        }
    } catch (const std::exception& e) {
        if (firstError.empty()) firstError = e.what();
    }
    if (!firstError.empty()) return fail("malformed document: " + firstError, result);
    result.timings.nodesMs = step.elapsedMs();

    step.restart();
    const core::Document typed = toCoreDocument(*version);
    if (const auto problem = core::validateDocument(typed)) return fail("invalid document: " + *problem, result);
    result.timings.validateMs = step.elapsedMs();

    result.brushes = version->brushCount();
    result.version = std::move(version);
    result.timings.totalMs = total.elapsedMs();
    return result;
}

LoadResult loadDocumentFile(const std::filesystem::path& file, const std::string& assetsRelativePath) {
    base::Stopwatch total, step;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        LoadResult r;
        r.error = "cannot open " + file.string();
        return r;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();
    const double readMs = step.elapsedMs();
    LoadResult r = loadDocumentText(text, assetsRelativePath);
    r.timings.readMs = readMs;
    r.timings.totalMs = total.elapsedMs();
    if (!r.error.empty()) r.error = file.filename().string() + ": " + r.error;
    return r;
}

std::string serializeDocument(const DocumentVersion& version) { return toJson(version).dump(2) + "\n"; }

std::string saveDocumentFile(const DocumentVersion& version, const std::filesystem::path& file) {
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    try {
        const std::string text = serializeDocument(version);
        if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path());
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return "could not write " + file.string();
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            if (!out) {
                out.close();
                std::error_code ignored;
                std::filesystem::remove(tmp, ignored);
                return "write failed for " + file.string();
            }
        }
        // Replaces an existing file atomically. On Windows a virus scanner or the indexer may hold the old file open for a moment
        // (sharing violation): retry briefly before giving up.
        for (int attempt = 0;; ++attempt) {
            std::error_code ec;
            std::filesystem::rename(tmp, file, ec);
            if (!ec) break;
            if (attempt >= 20) throw std::filesystem::filesystem_error("rename", tmp, file, ec);
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    } catch (const std::exception& e) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);   // never leave the temp file behind
        return std::string("could not save ") + file.string() + ": " + e.what();
    }
    return {};
}

} // namespace native::doc
