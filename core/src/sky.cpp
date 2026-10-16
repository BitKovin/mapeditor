// Sky (REQUIREMENTS §28, DESIGN §12 "Sky"): the engine's skybox direction mapping and the sky light's radiance tables.
#include "core/sky.h"
#include "core/albedo.h"
#include <algorithm>
#include <cmath>

namespace core {

namespace {
constexpr double kPi = 3.14159265358979323846;

double longitude(const Vec3& d) { return std::atan2(d.x, -d.z); }  // 0 = north (-Z), +pi / 2 = east (+X)
double elevation(const Vec3& d) {
    const double l = d.length();
    return l > 0 ? std::asin(std::clamp(d.y / l, -1.0, 1.0)) : 0.0;
}
Vec3 fromAngles(double lon, double elev) { return {std::sin(lon) * std::cos(elev), std::sin(elev), -std::cos(lon) * std::cos(elev)}; }

Vec3 texel(const DecodedImage& img, double u, double v) {
    const int x = std::clamp(static_cast<int>(u * img.width), 0, img.width - 1), y = std::clamp(static_cast<int>(v * img.height), 0, img.height - 1);
    const uint8_t* p = &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
    return {srgbToLinear(p[0]), srgbToLinear(p[1]), srgbToLinear(p[2])};
}
}  // namespace

SkyTexel cubemapLookup(const Vec3& dir) {
    // fs_bsp_cube: the direction with x negated, then the standard cube face selection (bgfx = D3D / GL: +X -X +Y -Y +Z -Z)
    const Vec3 s{-dir.x, dir.y, dir.z};
    const double ax = std::fabs(s.x), ay = std::fabs(s.y), az = std::fabs(s.z);
    SkyTexel t;
    double sc, tc, ma;
    if (ax >= ay && ax >= az) { ma = ax; if (s.x > 0) { t.face = 0; sc = -s.z; tc = -s.y; } else { t.face = 1; sc = s.z; tc = -s.y; } }
    else if (ay >= az) { ma = ay; if (s.y > 0) { t.face = 2; sc = s.x; tc = s.z; } else { t.face = 3; sc = s.x; tc = -s.z; } }
    else { ma = az; if (s.z > 0) { t.face = 4; sc = s.x; tc = -s.y; } else { t.face = 5; sc = -s.x; tc = -s.y; } }
    if (ma <= 0) return t;
    const double u = (sc / ma + 1) * 0.5, v = (tc / ma + 1) * 0.5;  // in the face as uploaded
    // TextureCube.hpp turned _up 90 degrees clockwise and _dn counter-clockwise on load: back to the file's own pixels
    if (t.face == 2) { t.u = v; t.v = 1 - u; }
    else if (t.face == 3) { t.u = 1 - v; t.v = u; }
    else { t.u = u; t.v = v; }
    return t;
}

Vec2 panoramaLookup(const Vec3& dir) {
    double u = longitude(dir) / (2 * kPi) + 0.5;
    if (u >= 1) u -= 1;
    return {u, 0.5 - elevation(dir) / kPi};
}

Vec3 SkyRadiance::at(const Vec3& dir) const {
    if (!(dir.y > 0) || radiance.size() != static_cast<size_t>(kLon * kLat)) return {};
    const int i = std::clamp(static_cast<int>(std::floor((longitude(dir) + kPi) / (2 * kPi) * kLon)), 0, kLon - 1);
    const int j = std::clamp(static_cast<int>(std::floor(elevation(dir) / (kPi / 2) * kLat)), 0, kLat - 1);
    return radiance[static_cast<size_t>(j * kLon + i)];
}

Vec3 SkyRadiance::irradianceAt(const Vec3& n) const {
    if (irradiance.size() != static_cast<size_t>(kNLon * kNLat)) return {};
    const double fu = (longitude(n) + kPi) / (2 * kPi) * kNLon, fv = (elevation(n) + kPi / 2) / kPi * (kNLat - 1);
    const int i0 = static_cast<int>(std::floor(fu)), j0 = std::clamp(static_cast<int>(std::floor(fv)), 0, kNLat - 2);
    const double a = fu - i0, b = std::clamp(fv - j0, 0.0, 1.0);
    const int ia = ((i0 % kNLon) + kNLon) % kNLon, ib = (ia + 1) % kNLon;
    auto e = [&](int i, int j) { return irradiance[static_cast<size_t>(j * kNLon + i)]; };
    return (e(ia, j0) * (1 - a) + e(ib, j0) * a) * (1 - b) + (e(ia, j0 + 1) * (1 - a) + e(ib, j0 + 1) * a) * b;
}

void SkyRadiance::integrate() {
    irradiance.assign(static_cast<size_t>(kNLon * kNLat), Vec3{});
    constexpr int kSub = 4;
    const double dLon = 2 * kPi / kLon / kSub, dEl = kPi / 2 / kLat / kSub;
    for (int nj = 0; nj < kNLat; ++nj)
        for (int ni = 0; ni < kNLon; ++ni) {
            const Vec3 n = fromAngles(-kPi + ni * 2 * kPi / kNLon, -kPi / 2 + nj * kPi / (kNLat - 1));
            Vec3 sum;
            for (int j = 0; j < kLat * kSub; ++j) {
                const double e0 = j * dEl, e1 = e0 + dEl, solid = dLon * (std::sin(e1) - std::sin(e0));
                for (int i = 0; i < kLon * kSub; ++i) {
                    const Vec3 w = fromAngles(-kPi + (i + 0.5) * dLon, e0 + dEl * 0.5);
                    const double c = n.dot(w);
                    if (c > 0) sum = sum + radiance[static_cast<size_t>((j / kSub) * kLon + i / kSub)] * (c * solid / kPi);
                }
            }
            irradiance[static_cast<size_t>(nj * kNLon + ni)] = sum;
        }
}

std::shared_ptr<SkyRadiance> makeSkyRadiance(const std::vector<DecodedImage>& images, bool panorama, const std::string& texture) {
    if (images.size() != (panorama ? 1u : 6u)) return nullptr;
    for (const auto& im : images) if (im.width <= 0 || im.height <= 0 || im.rgba.size() < static_cast<size_t>(im.width) * im.height * 4) return nullptr;
    auto out = std::make_shared<SkyRadiance>();
    out->texture = texture;
    out->radiance.assign(static_cast<size_t>(SkyRadiance::kLon * SkyRadiance::kLat), Vec3{});
    // each cell: the mean of 16 x 16 directions spread over it, sampled where the engine samples them
    constexpr int kSub = 16;
    for (int j = 0; j < SkyRadiance::kLat; ++j)
        for (int i = 0; i < SkyRadiance::kLon; ++i) {
            Vec3 sum;
            double weight = 0;
            for (int b = 0; b < kSub; ++b)
                for (int a = 0; a < kSub; ++a) {
                    const double lon = -kPi + (i + (a + 0.5) / kSub) * 2 * kPi / SkyRadiance::kLon, el = (j + (b + 0.5) / kSub) * (kPi / 2) / SkyRadiance::kLat;
                    const Vec3 d = fromAngles(lon, el);
                    Vec3 c;
                    if (panorama) { const Vec2 p = panoramaLookup(d); c = texel(images[0], p.u, p.v); }
                    else { const SkyTexel t = cubemapLookup(d); c = texel(images[static_cast<size_t>(t.face)], t.u, t.v); }
                    const double w = std::cos(el);  // solid angle of the sub-cell
                    sum = sum + c * w;
                    weight += w;
                }
            out->radiance[static_cast<size_t>(j * SkyRadiance::kLon + i)] = sum * (1.0 / weight);
        }
    out->integrate();
    return out;
}

}  // namespace core
