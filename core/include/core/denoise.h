#pragma once
#include "core/lightbake.h"
#include <string>
#include <vector>

namespace core::denoise {

// Intel Open Image Denoise (OIDN, Apache-2.0) for the global illumination of the lightmaps (DESIGN §6.5 "Denoising").
//
// Like the Vulkan loader, the library is opened at run time (OpenImageDenoise.dll / libOpenImageDenoise.so.2 / .dylib) with the
// vendored C header (core/vendored/oidn), so building needs nothing and a machine without OIDN simply keeps the noisy estimate and
// reports why. Search order: the file named by the environment variable MAPEDITOR_OIDN, the folder of the executable, then the
// system's normal search path. The official binary release also needs its sibling libraries (tbb, OpenImageDenoise_core,
// OpenImageDenoise_device_cpu ...) next to the main one.
//
// The filter is `RTLightmap`: OIDN's network trained on HDR lightmaps. It takes the colour image only (no albedo / normal guides).

enum class Quality { Fast, Balanced, High };

struct Info {
    bool available = false;
    std::string version;  // e.g. "2.5.1" (the vendored header's version: the API the loader was written against)
    std::string error;    // why it is not available
};

// Tries to open the library (once; the result is remembered). Cheap to call repeatedly.
Info probe();

struct Result {
    bool ok = false;
    std::string error;
    double seconds = 0;
};

// Denoises one HDR RGB image (width * height * 3 floats, linear, row major) in place. Thread safe (calls are serialised: one OIDN
// device is shared).
Result filterRGB(std::vector<float>& rgb, int width, int height, Quality quality = Quality::High);

// The lightmap flavour. `values4` holds 4 floats per sample (rgb, unused), in the order of `samples`; the result replaces rgb.
// Per atlas page an image is built from the samples: texels without a sample (unused space, gutters) are filled by growing the
// baked texels outwards (`fillRadius` rings, nearest-first average), so the black of unused atlas space does not bleed into the
// charts. Only the texels that carry a sample are written back; the denoiser never invents values for the rest.
// `pages` (optional): only these pages are processed (others are left untouched).
Result denoiseSamples(const TexelSamples& samples, std::vector<float>& values4, Quality quality = Quality::High, int fillRadius = 8,
                      const std::vector<int>* pages = nullptr);

}  // namespace core::denoise
