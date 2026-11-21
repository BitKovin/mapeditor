// scene/model_store -- the models of entities, loaded once per path and shared by every viewport (port of shell/src/state/models.ts).
//
// `.glb` files are parsed here (scene/gltf) and their images decoded; `.obj` files are read by the Core (`core::loadObjModel`, which also finds the textures: a primitive's `texture` is the
// path the texture table loads). The loading runs on the job pool; a model that is still loading or cannot be read leaves the entity's box as it is. Installing the store as the model
// bounds provider (`install`) makes `pointBox` outline and pick the model's own box.
// Thread: `get` / `bounds` from any thread; loading on the pool (or inline without one).
// Spec: 08_MILESTONES.md T11.9b.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/jobs.h"
#include "scene/model.h"

namespace native::scene {

/// An image of a .glb, decoded to RGBA8 (rows top to bottom); empty `rgba` = could not be decoded.
struct DecodedModelImage {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

struct LoadedModel {
    std::string path;                          // as the entity names it
    ModelData data;
    std::vector<DecodedModelImage> images;     // one per ModelData::images
    std::vector<std::string> warnings;
};

class ModelStore {
public:
    enum class Status { Loading, Ready, Error };
    struct Slot {
        Status status = Status::Loading;
        std::string error;
        std::shared_ptr<const LoadedModel> model;   // Ready only
    };

    ModelStore();
    ModelStore(const ModelStore&) = delete;
    ModelStore& operator=(const ModelStore&) = delete;
    ~ModelStore();

    /// Where models are read from and what runs the loading (null pool = inline). `onChange` runs (on the loading thread) whenever a model became ready or failed.
    void setSource(const std::filesystem::path& assetsRoot, base::JobPool* pool, std::function<void()> onChange);
    /// The slot of a model; the first ask starts loading it. `path` as `modelPathOf` gives it.
    Slot get(const std::string& path);
    /// The rest pose bounds of a ready model (no loading is started).
    std::optional<Box> bounds(const std::string& path) const;
    /// Forgets everything (another assets folder, or files changed on disk); the next ask reads again.
    void clear();
    /// Grows whenever a model finished loading or the store was cleared.
    uint64_t revision() const;

    /// Makes this store the source of `scene::entityModelBox` (and takes it out again when it is destroyed or `uninstall` is called).
    void install();
    void uninstall();

    /// Loads one model file synchronously (tests, tools): the .glb / .obj at `assetsRoot / path`. Null with `error` set on failure.
    static std::shared_ptr<const LoadedModel> load(const std::filesystem::path& assetsRoot, const std::string& path, std::string& error);

private:
    struct Shared;   // what the loading jobs share with the store (a job that finishes after the store is gone only finds an empty listener)
    static void finish(Shared& s, uint64_t generation, const std::string& path, Slot slot);

    std::shared_ptr<Shared> s_;
    bool installed_ = false;
};

} // namespace native::scene
