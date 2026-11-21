// scene/model_store -- see model_store.h.
#include "scene/model_store.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

#include "base/log.h"
#include "core/albedo.h"
#include "core/obj_model.h"
#include "scene/gltf.h"
#include "scene/model_entity.h"

namespace native::scene {

namespace fs = std::filesystem;

namespace {

std::string lowerExt(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    std::string e = dot == std::string::npos ? std::string() : path.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

// `rel` inside `root` (no `..` out of it): the file's path, or empty.
fs::path inside(const fs::path& root, const std::string& rel) {
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(root, ec);
    if (ec) return {};
    const fs::path file = fs::weakly_canonical(base / fs::path(rel), ec);
    if (ec) return {};
    const auto rootStr = base.generic_string();
    const auto fileStr = file.generic_string();
    if (fileStr.size() <= rootStr.size() || fileStr.compare(0, rootStr.size(), rootStr) != 0 || fileStr[rootStr.size()] != '/') return {};
    return file;
}

} // namespace

struct ModelStore::Shared {
    std::mutex mutex;
    std::map<std::string, Slot> slots;
    fs::path root;
    base::JobPool* pool = nullptr;
    std::function<void()> onChange;
    uint64_t generation = 0, revision = 0;
};

ModelStore::ModelStore() : s_(std::make_shared<Shared>()) {}

ModelStore::~ModelStore() {
    uninstall();
    std::lock_guard<std::mutex> lock(s_->mutex);
    s_->onChange = {};
    ++s_->generation;
}

void ModelStore::setSource(const fs::path& assetsRoot, base::JobPool* pool, std::function<void()> onChange) {
    std::lock_guard<std::mutex> lock(s_->mutex);
    s_->root = assetsRoot;
    s_->pool = pool;
    s_->onChange = std::move(onChange);
    s_->slots.clear();
    ++s_->generation;
    ++s_->revision;
}

void ModelStore::clear() {
    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        s_->slots.clear();
        ++s_->generation;
        ++s_->revision;
        notify = s_->onChange;
    }
    if (notify) notify();
}

uint64_t ModelStore::revision() const {
    std::lock_guard<std::mutex> lock(s_->mutex);
    return s_->revision;
}

ModelStore::Slot ModelStore::get(const std::string& path) {
    fs::path root;
    base::JobPool* pool = nullptr;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        const auto it = s_->slots.find(path);
        if (it != s_->slots.end()) return it->second;
        s_->slots[path] = Slot{};
        root = s_->root;
        pool = s_->pool;
        generation = s_->generation;
    }
    const auto run = [s = s_, root, path, generation] {
        std::string error;
        Slot slot;
        slot.model = load(root, path, error);
        slot.status = slot.model ? Status::Ready : Status::Error;
        slot.error = error;
        finish(*s, generation, path, std::move(slot));
    };
    if (pool) pool->submit(run);
    else run();
    std::lock_guard<std::mutex> lock(s_->mutex);
    const auto it = s_->slots.find(path);
    return it != s_->slots.end() ? it->second : Slot{};
}

void ModelStore::finish(Shared& s, uint64_t generation, const std::string& path, Slot slot) {
    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (generation != s.generation) return;   // the source changed while it was loading
        s.slots[path] = std::move(slot);
        ++s.revision;
        notify = s.onChange;
    }
    if (notify) notify();
}

std::optional<Box> ModelStore::bounds(const std::string& path) const {
    std::lock_guard<std::mutex> lock(s_->mutex);
    const auto it = s_->slots.find(path);
    if (it == s_->slots.end() || it->second.status != Status::Ready || !it->second.model) return std::nullopt;
    return it->second.model->data.bounds;
}

void ModelStore::install() {
    installed_ = true;
    setModelBoundsProvider([this](const std::string& path) -> std::optional<Box> {
        {
            std::lock_guard<std::mutex> lock(s_->mutex);
            const auto it = s_->slots.find(path);
            if (it != s_->slots.end()) {
                if (it->second.status == Status::Ready && it->second.model) return it->second.model->data.bounds;
                return std::nullopt;
            }
        }
        get(path);   // the first sight starts the load (inline without a pool)
        return bounds(path);
    });
}

void ModelStore::uninstall() {
    if (!installed_) return;
    installed_ = false;
    setModelBoundsProvider({});
}

std::shared_ptr<const LoadedModel> ModelStore::load(const fs::path& assetsRoot, const std::string& path, std::string& error) {
    const std::string ext = lowerExt(path);
    auto out = std::make_shared<LoadedModel>();
    out->path = path;
    try {
        if (ext == ".glb") {
            const fs::path file = inside(assetsRoot, path);
            if (file.empty()) {
                error = "the model is outside the assets folder: " + path;
                return nullptr;
            }
            std::ifstream in(file, std::ios::binary);
            if (!in) {
                error = "cannot read " + path;
                return nullptr;
            }
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            out->data = parseGlb(bytes.data(), bytes.size());
            for (size_t i = 0; i < out->data.images.size(); ++i) {
                const ModelImage& im = out->data.images[i];
                DecodedModelImage d;
                const std::string e = im.mime == "image/png" ? ".png" : im.mime == "image/jpeg" ? ".jpg" : "";
                if (!im.bytes.empty()) {
                    if (auto img = core::decodeImage(im.bytes, e)) {
                        d.width = img->width;
                        d.height = img->height;
                        d.rgba = std::move(img->rgba);
                    } else {
                        out->warnings.push_back("image " + std::to_string(i) + " of " + path + " cannot be decoded");
                    }
                }
                out->images.push_back(std::move(d));
            }
        } else if (ext == ".obj") {
            std::string objError;
            const auto obj = core::loadObjModel(assetsRoot, path, &objError);
            if (!obj) {
                error = objError.empty() ? "cannot read " + path : objError;
                return nullptr;
            }
            out->data = modelFromObj(*obj);
            out->warnings = obj->warnings;
        } else {
            error = "unsupported model format: " + (ext.empty() ? path : ext);
            return nullptr;
        }
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
    if (out->data.primitives.empty()) {
        error = path + " has no triangles";
        return nullptr;
    }
    return out;
}

} // namespace native::scene
