// doc/edit -- builds the next DocumentVersion from a base version.
//
// Owns: VersionEditor, the one way tools and commands make an edited version: it copies the base (a vector of shared node pointers),
// replaces / adds / removes only the nodes the edit touches (writing the typed value into the node's old raw JSON with the json_write
// writers, so unknown keys stay), validates what changed (03_DOCUMENT.md §4: brush structure, convexity where the entity class needs it,
// id uniqueness) and publishes a new version with a fresh number. The typed value of every new node is parsed back from its JSON, so a
// node is always exactly what loading its JSON gives.
// Thread: UI thread (one editor per edit; the base version is immutable and may be read elsewhere).
// Spec: 03_DOCUMENT.md §4, §2, 08_MILESTONES.md T2.4.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "doc/document.h"

namespace native::doc {

class VersionEditor {
public:
    explicit VersionEditor(std::shared_ptr<const DocumentVersion> base);

    // Every call returns false and records error() when the edit is invalid (unknown id, duplicate id, invalid brush); the first error
    // stays, later calls still work on the rest. finish() then refuses to publish.
    bool replaceBrush(const std::string& entityId, const core::Brush& brush);   // the brush with brush.id
    // `rawBase` (the JSON of the node this one is copied from) keeps the keys the editor does not model; the typed value wins where both have a key.
    bool addBrush(const std::string& entityId, const core::Brush& brush, const Json& rawBase = Json());       // appended to the entity
    bool removeBrush(const std::string& entityId, const std::string& brushId);
    bool replaceEntity(const core::Entity& entity);                             // its own keys; entity.brushes is ignored
    // appended; entity.brushes (if any) become its brush nodes (`brushRaws[i]` = the base JSON of brush i, when given)
    bool addEntity(const core::Entity& entity, const Json& rawBase = Json(), const std::vector<Json>& brushRaws = {});
    bool removeEntity(const std::string& entityId);
    bool replaceInstance(const core::PrefabInstance& instance);
    bool addInstance(const core::PrefabInstance& instance, const Json& rawBase = Json());
    bool removeInstance(const std::string& instanceId);

    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }
    bool changed() const { return changed_; }

    // The edited version (a new version number), the base itself when nothing changed, null when an edit was refused.
    std::shared_ptr<const DocumentVersion> finish();

private:
    bool fail(std::string message);
    bool checkBrush(const core::Brush& brush, const std::string& classname);
    // The entity at `index` as a node this editor may modify (copied from the base on first use).
    EntityNode& mutableEntity(size_t index);
    void reindexEntities();
    void reindexInstances();
    std::unordered_map<std::string, size_t>& brushIndexOf(size_t entityIndex);

    std::shared_ptr<const DocumentVersion> base_;
    std::unique_ptr<DocumentVersion> working_;
    std::vector<std::shared_ptr<EntityNode>> mine_;                    // parallel to working_->entities: the nodes made here
    std::unordered_map<std::string, size_t> entityIndex_, instanceIndex_;
    std::unordered_map<size_t, std::unordered_map<std::string, size_t>> brushIndex_;   // per entity index, built when first needed
    std::string error_;
    bool changed_ = false;
};

} // namespace native::doc
