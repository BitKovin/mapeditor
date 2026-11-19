// doc/document -- see document.h.
#include "doc/document.h"

#include <atomic>

namespace native::doc {

namespace {
constexpr char kSep = '\x1f';
}

DocumentVersion::DocumentVersion(const DocumentVersion& o)
    : version(o.version), entities(o.entities), instances(o.instances), rootRaw(o.rootRaw), hasEntitiesKey(o.hasEntitiesKey),
      hasInstancesKey(o.hasInstancesKey), path(o.path) {}

DocumentVersion& DocumentVersion::operator=(const DocumentVersion& o) {
    if (this == &o) return *this;
    version = o.version;
    entities = o.entities;
    instances = o.instances;
    rootRaw = o.rootRaw;
    hasEntitiesKey = o.hasEntitiesKey;
    hasInstancesKey = o.hasInstancesKey;
    path = o.path;
    std::lock_guard<std::mutex> lock(indexMutex_);
    index_.reset();
    return *this;
}

std::shared_ptr<const DocumentVersion::Index> DocumentVersion::index() const {
    std::lock_guard<std::mutex> lock(indexMutex_);
    if (!index_) {
        auto idx = std::make_shared<Index>();
        idx->entities.reserve(entities.size());
        for (size_t i = 0; i < entities.size(); ++i) {
            const EntityNode& e = *entities[i];
            idx->entities.emplace(e.typed.id, static_cast<int>(i));
            for (size_t b = 0; b < e.brushes.size(); ++b)
                idx->brushes.emplace(e.typed.id + kSep + e.brushes[b]->brush.id, static_cast<int>(b));
            idx->brushCount += e.brushes.size();
        }
        index_ = std::move(idx);
    }
    return index_;
}

int DocumentVersion::findEntity(const std::string& entityId) const {
    const auto idx = index();
    auto it = idx->entities.find(entityId);
    return it == idx->entities.end() ? -1 : it->second;
}

int DocumentVersion::findBrush(const std::string& entityId, const std::string& brushId) const {
    const auto idx = index();
    auto it = idx->brushes.find(entityId + kSep + brushId);
    return it == idx->brushes.end() ? -1 : it->second;
}

size_t DocumentVersion::brushCount() const { return index()->brushCount; }

uint64_t nextVersionNumber() {
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

core::Document toCoreDocument(const DocumentVersion& v) {
    core::Document d;
    if (v.rootRaw.is_object() && v.rootRaw.contains("schemaVersion") && v.rootRaw["schemaVersion"].is_string())
        d.schemaVersion = v.rootRaw["schemaVersion"].get<std::string>();
    d.entities.reserve(v.entities.size());
    for (const auto& node : v.entities) {
        core::Entity e = node->typed;
        e.brushes.clear();
        e.brushes.reserve(node->brushes.size());
        for (const auto& b : node->brushes) e.brushes.push_back(b->brush);
        d.entities.push_back(std::move(e));
    }
    d.prefabInstances.reserve(v.instances.size());
    for (const auto& node : v.instances) d.prefabInstances.push_back(node->typed);
    return d;
}

Json toJson(const DocumentVersion& v) {
    Json root = v.rootRaw.is_object() ? v.rootRaw : Json::object();
    if (v.hasEntitiesKey) {
        Json entities = Json::array();
        for (const auto& node : v.entities) {
            Json e = node->raw;
            if (node->hasBrushesKey) {
                Json brushes = Json::array();
                for (const auto& b : node->brushes) brushes.push_back(b->raw);
                e["brushes"] = std::move(brushes);
            }
            entities.push_back(std::move(e));
        }
        root["entities"] = std::move(entities);
    }
    if (v.hasInstancesKey) {
        Json instances = Json::array();
        for (const auto& node : v.instances) instances.push_back(node->raw);
        root["prefabInstances"] = std::move(instances);
    }
    return root;
}

} // namespace native::doc
