// scene/model_entity -- see model_entity.h.
#include "scene/model_entity.h"

#include <algorithm>
#include <memory>
#include <mutex>

#include "core/obj_model.h"

namespace native::scene {

namespace {

struct Registry {
    std::mutex mutex;
    std::shared_ptr<const ModelBoundsProvider> bounds;
    std::shared_ptr<const KeyDefaultProvider> defaults;
};

Registry& registry() {
    static Registry r;
    return r;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// The value of a key: the entity's own, else the game's default.
std::string keyOf(const core::Entity& e, const std::string& key) {
    const auto it = e.keyvalues.find(key);
    if (it != e.keyvalues.end()) return it->second;
    std::shared_ptr<const KeyDefaultProvider> d;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        d = r.defaults;
    }
    return d && *d ? (*d)(e.classname, key) : std::string();
}

} // namespace

void setModelBoundsProvider(ModelBoundsProvider provider) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.bounds = provider ? std::make_shared<const ModelBoundsProvider>(std::move(provider)) : nullptr;
}

void setKeyDefaultProvider(KeyDefaultProvider provider) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.defaults = provider ? std::make_shared<const KeyDefaultProvider>(std::move(provider)) : nullptr;
}

std::string modelPathOf(const core::Entity& e) {
    if (e.isBrushEntity()) return {};
    std::string p = trim(keyOf(e, "model_path"));
    if (p.empty()) p = trim(keyOf(e, "model"));
    std::replace(p.begin(), p.end(), '\\', '/');
    const size_t first = p.find_first_not_of('/');
    return first == std::string::npos ? std::string() : p.substr(first);
}

std::string modelAnimationOf(const core::Entity& e) { return trim(keyOf(e, "model_animation")); }

V3 modelScaleOf(const core::Entity& e) {
    const core::Vec3 s = core::modelScaleOf(e);
    return {s.x, s.y, s.z};
}

std::optional<Box> entityModelBox(const core::Entity& e) {
    const std::string path = modelPathOf(e);
    if (path.empty()) return std::nullopt;
    std::shared_ptr<const ModelBoundsProvider> provider;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        provider = r.bounds;
    }
    if (!provider || !*provider) return std::nullopt;
    const std::optional<Box> b = (*provider)(path);
    if (!b) return std::nullopt;
    const V3 s = native::scene::modelScaleOf(e);
    Box out;
    for (size_t i = 0; i < 3; ++i) {
        const double x = b->min[i] * s[i], y = b->max[i] * s[i];
        out.min[i] = std::min(x, y);
        out.max[i] = std::max(x, y);
    }
    return out;
}

} // namespace native::scene
