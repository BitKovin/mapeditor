#include "light/light_pack.h"

#include <cmath>
#include <cstring>

#include "core/sky.h"

namespace native::light {

namespace {

void pack(const core::Light& L, float* v) {
    auto set = [&](int i, float a, float b, float c, float d) { v[i * 4] = a; v[i * 4 + 1] = b; v[i * 4 + 2] = c; v[i * 4 + 3] = d; };
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    set(0, static_cast<float>(L.position.x), static_cast<float>(L.position.y), static_cast<float>(L.position.z), static_cast<float>(L.radius));
    set(1, static_cast<float>(L.color.x * L.brightness), static_cast<float>(L.color.y * L.brightness), static_cast<float>(L.color.z * L.brightness),
        static_cast<float>(static_cast<int>(L.type)));
    set(2, static_cast<float>(L.forward.x), static_cast<float>(L.forward.y), static_cast<float>(L.forward.z), static_cast<float>(std::cos(L.innerAngle * kDeg)));
    set(3, static_cast<float>(L.right.x), static_cast<float>(L.right.y), static_cast<float>(L.right.z), static_cast<float>(std::cos(L.outerAngle * kDeg)));
    set(4, static_cast<float>(L.up.x), static_cast<float>(L.up.y), static_cast<float>(L.up.z), static_cast<float>(L.sourceSize));
    set(5, static_cast<float>(L.width), static_cast<float>(L.height), L.castShadows ? 1.f : 0.f, static_cast<float>(L.style));
    set(6, static_cast<float>(L.bounce), 0, 0, 0);
}

} // namespace

std::vector<float> packLights(const std::vector<core::Light>& lights) {
    constexpr size_t kSkyTableRecords = (core::SkyRadiance::kLon * core::SkyRadiance::kLat + core::SkyRadiance::kNLon * core::SkyRadiance::kNLat + 7) / 8;
    std::vector<float> out(lights.size() * kLightRecordFloats, 0.f);
    for (size_t k = 0; k < lights.size(); ++k) pack(lights[k], &out[k * kLightRecordFloats]);
    const core::SkyRadiance* sky = nullptr;
    for (const auto& L : lights)
        if (L.type == core::LightType::Sky && L.sky) { sky = L.sky.get(); break; }
    if (!sky) return out;
    const size_t base = lights.size() * 8;   // vec4 index
    for (size_t k = 0; k < lights.size(); ++k)
        if (lights[k].type == core::LightType::Sky && lights[k].sky.get() == sky) {
            out[k * kLightRecordFloats + 28] = static_cast<float>(base);
            out[k * kLightRecordFloats + 29] = 1.f;
        }
    std::vector<float> table;
    for (const core::Vec3& c : sky->radiance) table.insert(table.end(), {static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z), 0.f});
    for (const core::Vec3& c : sky->irradiance) table.insert(table.end(), {static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z), 0.f});
    table.resize(kSkyTableRecords * kLightRecordFloats, 0.f);
    out.insert(out.end(), table.begin(), table.end());
    return out;
}

} // namespace native::light
