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

ModelStore::~ModelStore() { uninstall(); }

void ModelStore::setSource(const fs::path& assetsRoot, base::JobPool* pool, std::function<void()> onChange) {
    std::lock_guard<std::mutex> lock(mutex_);
    root_ = assetsRoot;
    pool_ = pool;
    onChange_ = std::move(onChange);
    slots_.clear();
    ++generation_;
    ++revision_;
}

void ModelStore::clear() {
    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slots_.clear();
        ++generation_;
        ++revision_;
        notify = onChange_;
    }
    if (notify) notify();
}

uint64_t ModelStore::revision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
}

ModelStore::Slot ModelStore::get(const std::string& path) {
    fs::path root;
    base::JobPool* pool = nullptr;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = slots_.find(path);
        if (it != slots_.end()) return it->second;
        slots_[path] = Slot{};
        root = root_;
        pool = pool_;
        generation = generation_;
    }
    const auto run = [this, root, path, generation] {
        std::string error;
        Slot slot;
        slot.model = load(root, path, error);
        slot.status = slot.model ? Status::Ready : Status::Error;
        slot.error = error;
        finish(generation, path, std::move(slot));
    };
    if (pool) pool->submit(run);
    else run();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = slots_.find(path);
    return it != slots_.end() ? it->second : Slot{};
}

void ModelStore::finish(uint64_t generation, const std::string& path, Slot slot) {
    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_) return;   // the source changed while it was loading
        slots_[path] = std::move(slot);
        ++revision_;
        notify = onChange_;
    }
    if (notify) notify();
}

std::optional<Box> ModelStore::bounds(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = slots_.find(path);
    if (it == slots_.end() || it->second.status != Status::Ready || !it->second.model) return std::nullopt;
    return it->second.model->data.bounds;
}

void ModelStore::install() {
    installed_ = true;
    setModelBoundsProvider([this](const std::string& path) -> std::optional<Box> {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = slots_.find(path);
            if (it != slots_.end()) {
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
