#pragma once
#include "core/math3.h"
#include <memory>
#include <string>
#include <vector>

namespace core {

struct DecodedImage;

// Sky (REQUIREMENTS §28, DESIGN §12 "Sky"): how the engine samples a skybox, and the sky light's radiance taken from it.
//
// The engine (SdlWasmEngine) draws a texture whose name ends in "cube" with bsp/fs_bsp_cube: dir = normalize(v_world - cameraPosition) in its world
// space (= the editor's: metres, Y up), times worldOrientation (identity unless the game turns the world's up vector), then x negated, and
// textureCube(). The cube's faces are the files <base>_lf, _rt, _up, _dn, _ft, _bk as bgfx faces +X, -X, +Y, -Y, +Z, -Z, with _up turned 90 degrees
// clockwise and _dn 90 degrees counter-clockwise on load (TextureCube.hpp). Net effect, in editor directions (north = -Z = Quake +Y, east = +X):
// _lf looks west, _rt east, _ft south (+Z), _bk north (-Z), each upright as seen from inside; _up has west at its top, south at its right.
// A single panorama (no face files) is converted by the engine into such faces; the net effect is lon = atan2(x, -z), lat = asin(y).

/// Which face file (0..5 = lf rt up dn ft bk) and where in it (u right, v down, 0..1) the engine samples for a view direction.
struct SkyTexel { int face = 0; double u = 0, v = 0; };
SkyTexel cubemapLookup(const Vec3& dir);
/// Where in an equirectangular panorama (u right, v down, 0..1) the engine samples for a view direction.
Vec2 panoramaLookup(const Vec3& dir);

/// The sky light's incoming light per direction (REQUIREMENTS §28 K12): the skybox averaged into a small latitude / longitude table over the upper
/// hemisphere (only directions that point up are sky, as for the uniform sky light), linear colour 0..1, plus the irradiance it gives each normal
/// (cosine weighted, divided by pi, so a white sky gives (1 + n.y) / 2 like the uniform one). The same tables and lookups on the CPU and the GPU.
struct SkyRadiance {
    static constexpr int kLon = 32, kLat = 8;     // radiance cells: longitude atan2(x, -z) from -pi, elevation 0 .. pi / 2
    static constexpr int kNLon = 16, kNLat = 9;   // irradiance samples over every normal: longitude as above, elevation -pi / 2 .. pi / 2 (inclusive)
    std::string texture;                          // the skybox it was made from
    std::vector<Vec3> radiance;                   // kLon * kLat, row = elevation band
    std::vector<Vec3> irradiance;                 // kNLon * kNLat
    /// Radiance of the cell `dir` falls in; 0 for a direction that does not point up.
    Vec3 at(const Vec3& dir) const;
    /// Irradiance at a normal (bilinear between the samples).
    Vec3 irradianceAt(const Vec3& n) const;
    /// Fills `irradiance` from `radiance`.
    void integrate();
};

/// The tables for a skybox: six faces (lf rt up dn ft bk) or one panorama, sampled the engine's way. Null when the images do not fit (no faces, sizes).
std::shared_ptr<SkyRadiance> makeSkyRadiance(const std::vector<DecodedImage>& images, bool panorama, const std::string& texture);

}  // namespace core
