// doc/edit -- see edit.h.
#include "doc/edit.h"

#include <core/entity_class.h>

#include "base/hash.h"
#include "doc/json_write.h"

namespace native::doc {

namespace {

std::shared_ptr<const BrushNode> makeBrushNode(const core::Brush& brush, const Json& oldRaw) {
    auto node = std::make_shared<BrushNode>();
    node->raw = writeBrush(brush, oldRaw);
    node->brush = core::brushFromJson(node->raw);   // the node is what its JSON reads back to
    node->contentHash = base::hashJson(node->raw);
    return node;
}

} // namespace

VersionEditor::VersionEditor(std::shared_ptr<const DocumentVersion> base) : base_(std::move(base)) {
    if (!base_) {
        error_ = "no document";
        return;
    }
    working_ = std::make_unique<DocumentVersion>(*base_);
    mine_.resize(working_->entities.size());
    reindexEntities();
    reindexInstances();
}

bool VersionEditor::fail(std::string message) {
    if (error_.empty()) error_ = std::move(message);
    return false;
}

void VersionEditor::reindexEntities() {
    entityIndex_.clear();
    entityIndex_.reserve(working_->entities.size());
    for (size_t i = 0; i < working_->entities.size(); ++i) entityIndex_.emplace(working_->entities[i]->typed.id, i);
    brushIndex_.clear();
}

void VersionEditor::reindexInstances() {
    instanceIndex_.clear();
    for (size_t i = 0; i < working_->instances.size(); ++i) instanceIndex_.emplace(working_->instances[i]->typed.id, i);
}

EntityNode& VersionEditor::mutableEntity(size_t index) {
    if (!mine_[index]) {
        mine_[index] = std::make_shared<EntityNode>(*working_->entities[index]);
        working_->entities[index] = mine_[index];
    }
    return *mine_[index];
}

std::unordered_map<std::string, size_t>& VersionEditor::brushIndexOf(size_t entityIndex) {
    auto it = brushIndex_.find(entityIndex);
    if (it != brushIndex_.end()) return it->second;
    auto& m = brushIndex_[entityIndex];
    const auto& brushes = working_->entities[entityIndex]->brushes;
    m.reserve(brushes.size());
    for (size_t i = 0; i < brushes.size(); ++i) m.emplace(brushes[i]->brush.id, i);
    return m;
}

bool VersionEditor::checkBrush(const core::Brush& brush, const std::string& classname) {
    if (brush.id.empty()) return fail("a brush needs an id");
    if (auto err = brush.checkStructure()) return fail("brush " + brush.id + ": " + *err);
    if (core::EntityClassRegistry::requiresConvexBrushes(classname))
        if (auto err = brush.checkConvexSolid()) return fail("brush " + brush.id + " of " + classname + " must be a convex solid: " + *err);
    return true;
}

bool VersionEditor::replaceBrush(const std::string& entityId, const core::Brush& brush) {
    if (!working_) return false;
    auto e = entityIndex_.find(entityId);
    if (e == entityIndex_.end()) return fail("no entity " + entityId);
    auto& bi = brushIndexOf(e->second);
    auto b = bi.find(brush.id);
    if (b == bi.end()) return fail("entity " + entityId + " has no brush " + brush.id);
    if (!checkBrush(brush, working_->entities[e->second]->typed.classname)) return false;
    EntityNode& node = mutableEntity(e->second);
    node.brushes[b->second] = makeBrushNode(brush, node.brushes[b->second]->raw);
    changed_ = true;
    return true;
}

bool VersionEditor::addBrush(const std::string& entityId, const core::Brush& brush, const Json& rawBase) {
    if (!working_) return false;
    auto e = entityIndex_.find(entityId);
    if (e == entityIndex_.end()) return fail("no entity " + entityId);
    auto& bi = brushIndexOf(e->second);
    if (bi.count(brush.id)) return fail("entity " + entityId + " already has a brush " + brush.id);
    if (!checkBrush(brush, working_->entities[e->second]->typed.classname)) return false;
    EntityNode& node = mutableEntity(e->second);
    node.brushes.push_back(makeBrushNode(brush, rawBase));
    node.hasBrushesKey = true;
    bi.emplace(brush.id, node.brushes.size() - 1);
    changed_ = true;
    return true;
}

bool VersionEditor::removeBrush(const std::string& entityId, const std::string& brushId) {
    if (!working_) return false;
    auto e = entityIndex_.find(entityId);
    if (e == entityIndex_.end()) return fail("no entity " + entityId);
    auto& bi = brushIndexOf(e->second);
    auto b = bi.find(brushId);
    if (b == bi.end()) return fail("entity " + entityId + " has no brush " + brushId);
    EntityNode& node = mutableEntity(e->second);
    node.brushes.erase(node.brushes.begin() + static_cast<std::ptrdiff_t>(b->second));
    brushIndex_.erase(e->second);   // the indices after the removed brush moved
    changed_ = true;
    return true;
}

bool VersionEditor::replaceEntity(const core::Entity& entity) {
    if (!working_) return false;
    auto e = entityIndex_.find(entity.id);
    if (e == entityIndex_.end()) return fail("no entity " + entity.id);
    const EntityNode& old = *working_->entities[e->second];
    if (core::EntityClassRegistry::requiresConvexBrushes(entity.classname) && !core::EntityClassRegistry::requiresConvexBrushes(old.typed.classname))
        for (const auto& bn : old.brushes)
            if (!checkBrush(bn->brush, entity.classname)) return false;   // the new class has stricter rules than the old one
    EntityNode& node = mutableEntity(e->second);
    node.raw = writeEntity(entity, node.raw);
    node.typed = core::entityFromJson(node.raw);
    changed_ = true;
    return true;
}

bool VersionEditor::addEntity(const core::Entity& entity, const Json& rawBase, const std::vector<Json>& brushRaws) {
    if (!working_) return false;
    if (entity.id.empty()) return fail("an entity needs an id");
    if (entityIndex_.count(entity.id)) return fail("entity " + entity.id + " already exists");
    auto node = std::make_shared<EntityNode>();
    node->raw = writeEntity(entity, rawBase);
    node->typed = core::entityFromJson(node->raw);
    for (size_t i = 0; i < entity.brushes.size(); ++i) {
        const core::Brush& b = entity.brushes[i];
        if (!checkBrush(b, entity.classname)) return false;
        node->brushes.push_back(makeBrushNode(b, i < brushRaws.size() ? brushRaws[i] : Json()));
    }
    node->hasBrushesKey = !node->brushes.empty();
    working_->entities.push_back(node);
    mine_.push_back(node);
    entityIndex_.emplace(entity.id, working_->entities.size() - 1);
    changed_ = true;
    return true;
}

bool VersionEditor::removeEntity(const std::string& entityId) {
    if (!working_) return false;
    auto e = entityIndex_.find(entityId);
    if (e == entityIndex_.end()) return fail("no entity " + entityId);
    const auto at = static_cast<std::ptrdiff_t>(e->second);
    working_->entities.erase(working_->entities.begin() + at);
    mine_.erase(mine_.begin() + at);
    reindexEntities();
    changed_ = true;
    return true;
}

bool VersionEditor::replaceInstance(const core::PrefabInstance& instance) {
    if (!working_) return false;
    auto i = instanceIndex_.find(instance.id);
    if (i == instanceIndex_.end()) return fail("no prefab instance " + instance.id);
    auto node = std::make_shared<InstanceNode>();
    node->raw = writeInstance(instance, working_->instances[i->second]->raw);
    node->typed = core::instanceFromJson(node->raw);
    working_->instances[i->second] = std::move(node);
    changed_ = true;
    return true;
}

bool VersionEditor::addInstance(const core::PrefabInstance& instance, const Json& rawBase) {
    if (!working_) return false;
    if (instance.id.empty()) return fail("a prefab instance needs an id");
    if (instanceIndex_.count(instance.id)) return fail("prefab instance " + instance.id + " already exists");
    auto node = std::make_shared<InstanceNode>();
    node->raw = writeInstance(instance, rawBase);
    node->typed = core::instanceFromJson(node->raw);
    working_->instances.push_back(std::move(node));
    working_->hasInstancesKey = true;
    instanceIndex_.emplace(instance.id, working_->instances.size() - 1);
    changed_ = true;
    return true;
}

bool VersionEditor::removeInstance(const std::string& instanceId) {
    if (!working_) return false;
    auto i = instanceIndex_.find(instanceId);
    if (i == instanceIndex_.end()) return fail("no prefab instance " + instanceId);
    working_->instances.erase(working_->instances.begin() + static_cast<std::ptrdiff_t>(i->second));
    reindexInstances();
    changed_ = true;
    return true;
}

std::shared_ptr<const DocumentVersion> VersionEditor::finish() {
    if (!ok() || !working_) return nullptr;
    if (!changed_) return base_;
    working_->version = nextVersionNumber();
    mine_.clear();
    return std::shared_ptr<const DocumentVersion>(std::move(working_));
}

} // namespace native::doc
