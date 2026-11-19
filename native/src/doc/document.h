// doc/document -- the in-memory document: immutable versions made of shared, immutable nodes.
//
// Owns: BrushNode / EntityNode / InstanceNode / DocumentVersion (03_DOCUMENT.md §2), the lookup tables of a version (built
// lazily, once), version numbers, and conversion back to a typed core::Document and to the JSON tree that is saved. A node is
// never changed after it is published: an edit copies the node it changes into a new version that shares every other node
// (structural sharing), so undo / redo keep several versions at the cost of the changed nodes only.
// `raw` JSON is the source of truth for saving; keys the editor does not model live only there and survive unchanged.
// Thread: versions are immutable, readable from any thread; the lookup tables are built under a lock.
// Spec: 03_DOCUMENT.md §1 - §3, 08_MILESTONES.md T2.2.
#pragma once

#include <core/document.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace native::doc {

using Json = nlohmann::json;

struct BrushNode {
    core::Brush brush;                   // typed: id, op, vertices, faces, smoothEdges
    Json raw;                            // the brush's JSON (faces included)
    uint64_t contentHash = 0;            // base::hashJson(raw), computed once when the node is made
};

struct EntityNode {
    core::Entity typed;                  // without brushes (typed.brushes stays empty; the brushes live below)
    std::vector<std::shared_ptr<const BrushNode>> brushes;
    Json raw;                            // the entity's JSON WITHOUT its "brushes" array
    bool hasBrushesKey = false;          // the file had a "brushes" key (possibly empty): written back even when empty
};

struct InstanceNode {
    core::PrefabInstance typed;
    Json raw;
};

class DocumentVersion {
public:
    DocumentVersion() = default;
    // Copies the data; the lookup tables are rebuilt for the copy (an edit makes a copy and changes it before publishing).
    DocumentVersion(const DocumentVersion& o);
    DocumentVersion& operator=(const DocumentVersion& o);

    uint64_t version = 0;                // strictly increasing per process (nextVersionNumber)
    std::vector<std::shared_ptr<const EntityNode>> entities;
    std::vector<std::shared_ptr<const InstanceNode>> instances;
    Json rootRaw;                        // every top-level key except "entities" and "prefabInstances"
    bool hasEntitiesKey = true;          // the file had these keys (possibly empty): written back
    bool hasInstancesKey = false;
    std::string path;                    // assets-relative, "" = unsaved

    // Lookup tables (03 §2), rebuilt lazily per version. -1 when not found.
    int findEntity(const std::string& entityId) const;
    // Index of the brush within entities[entityIndex]->brushes.
    int findBrush(const std::string& entityId, const std::string& brushId) const;
    size_t brushCount() const;

private:
    struct Index {
        std::unordered_map<std::string, int> entities;
        std::unordered_map<std::string, int> brushes;   // entityId + 0x1f + brushId
        size_t brushCount = 0;
    };
    std::shared_ptr<const Index> index() const;

    mutable std::mutex indexMutex_;
    mutable std::shared_ptr<const Index> index_;
};

// A fresh, strictly increasing version number (any thread).
uint64_t nextVersionNumber();

// The typed document of a version (brush nodes copied into their entities): for core::validateDocument, the lighting code and tests.
core::Document toCoreDocument(const DocumentVersion& v);

// The JSON tree that is saved: rootRaw + entities (each node's raw plus its brushes' raws) + prefabInstances. Keys come out sorted
// (nlohmann's std::map order), so dump(2) + "\n" is the Core's file format (core/src/scene.cpp saveDocument).
Json toJson(const DocumentVersion& v);

} // namespace native::doc
