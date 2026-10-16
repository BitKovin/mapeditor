#pragma once
#include "core/math3.h"
#include "core/profile.h"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

// Albedo textures for the light bounces (DESIGN §6.5 "Textured bounces"). A bounce ray that hits a surface picks up that surface's colour:
// albedo = texture(face albedo uv) instead of one constant. The Core decodes the game's textures itself (TGA, PNG, baseline JPEG), keeps
// them small (the bounce light is blurry anyway: at most kAlbedoMaxSize texels on the long side) with a full mip chain, and shares them
// between the CPU evaluator and the GPU (where they become one array of sampled images indexed from the shader: "bindless").

inline constexpr int kAlbedoMaxSize = 256;

/// Decoded 8 bit image, rows top to bottom, 4 bytes per pixel (RGBA). nullopt when the format is unknown or damaged.
struct DecodedImage {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};
std::optional<DecodedImage> decodeImage(const std::vector<unsigned char>& bytes, const std::string& extension);
std::optional<DecodedImage> decodeTga(const std::vector<unsigned char>& bytes);
std::optional<DecodedImage> decodePng(const std::vector<unsigned char>& bytes);
std::optional<DecodedImage> decodeJpeg(const std::vector<unsigned char>& bytes);

/// sRGB (8 bit) -> linear, and back.
float srgbToLinear(uint8_t v);
uint8_t linearToSrgb(float v);

/// How light passes a texture (REQUIREMENTS §23), resolved for one image (the stored mode "auto" becomes Cutout or Opaque).
///   Cutout: alpha < cutoff is a hole, the rest is solid. Translucent: transmission = (1 - opacity * alpha) * mix(white, colour, tint).
enum class TransparencyMode : int { Opaque = 0, Cutout = 1, Translucent = 2 };
struct Transparency {
    TransparencyMode mode = TransparencyMode::Opaque;
    float cutoff = 0.5f, opacity = 0.5f, tint = 1.f;
};
/// What an image's alpha channel holds: nothing (every texel >= 240), on / off holes (fences: texels clear or solid, a few soft edge texels),
/// or real half transparency (glass: a good share of the non-solid texels lie between 16 and 239).
enum class AlphaKind : int { None = 0, Cutout = 1, Translucent = 2 };
AlphaKind classifyAlpha(const uint8_t* rgba, size_t texels);
/// The `transparency` object of a texture's properties file (its JSON text; empty = none) resolved for an image with that kind of alpha.
///   auto: None -> opaque, Cutout -> cutout, Translucent -> translucent with opacity 1 (the texel alpha IS the opacity: glass as painted).
///   cutout without any alpha -> opaque (nothing to cut). translucent: opacity as set (times the texel alpha when the image has one).
Transparency resolveTransparency(const std::string& propsJson, AlphaKind alpha);

inline constexpr int kAlphaMaskMaxSize = 1024;

struct AlbedoImage {
    int width = 0, height = 0;                 // size of mip 0
    std::vector<std::vector<uint8_t>> mips;    // RGBA8, sRGB encoded (alpha unused), mip l is max(1, w >> l) x max(1, h >> l)
    // Alpha of the source image at up to kAlphaMaskMaxSize on the long side (nearest texels: cutouts stay crisp), rows top to bottom; empty when the
    // image's alpha is AlphaKind::None. Read by the light baker's transparency (OccluderMesh::alphaWords).
    int alphaWidth = 0, alphaHeight = 0;
    std::vector<uint8_t> alpha;
    bool hasAlpha() const { return !alpha.empty(); }
    AlphaKind alphaKind = AlphaKind::None;
    Transparency transparency;                 // set by AlbedoLibrary from the texture's properties
    int mipCount() const { return static_cast<int>(mips.size()); }
    int mipWidth(int l) const { return std::max(1, width >> l); }
    int mipHeight(int l) const { return std::max(1, height >> l); }
    Vec3 average() const;                      // mean linear colour (the last mip)
    /// Linear colour at (u, v) in texture repeats (wraps), level of detail `lod` (0 = full size, fractions blend two levels): the same filter
    /// the GPU sampler applies (bilinear inside a level, linear between levels, repeat).
    Vec3 sample(double u, double v, double lod) const;
};

/// Builds the mip chain of an image (long side limited to kAlbedoMaxSize by box filtering first).
AlbedoImage makeAlbedoImage(const DecodedImage& image);

/// Every texture a mesh refers to, index = OccluderMesh texture id. A texture that could not be loaded is a null entry (white albedo).
struct AlbedoSet {
    std::vector<std::string> names;
    std::vector<std::shared_ptr<const AlbedoImage>> images;
    size_t loaded() const { size_t n = 0; for (const auto& i : images) n += i != nullptr; return n; }
};

/// Loads (and caches) textures by Quake style path ("base_wall/concrete01"). Thread safe. A file that changed on disk (size or time) is reloaded.
class AlbedoLibrary {
public:
    AlbedoLibrary(std::filesystem::path assetsRoot, GameProfile profile);
    /// null when missing or undecodable (the failure is remembered until the file changes).
    std::shared_ptr<const AlbedoImage> get(const std::string& texture);
    size_t cached() const;
private:
    struct Entry { std::shared_ptr<const AlbedoImage> image; uintmax_t size = 0; int64_t time = 0; uintmax_t propsSize = 0; int64_t propsTime = 0; };
    std::filesystem::path root_;
    GameProfile profile_;
    mutable std::unordered_map<std::string, Entry> cache_;
    mutable std::mutex mutex_;
};
using AlbedoLoader = std::function<std::shared_ptr<const AlbedoImage>(const std::string&)>;

}  // namespace core
