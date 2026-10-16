#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace core {

// OpenEXR files with half float RGB channels and ZIP compression (16 scanlines per block), the lightmap format of the rBSP export
// (REQUIREMENTS §25 E2). A small writer / reader of exactly that subset on top of miniz (zlib): the files are standard EXR, any tool opens them;
// the reader also accepts uncompressed blocks and ZIPS (1 scanline) blocks, which other writers may produce.

struct ExrImage {
    int width = 0, height = 0;
    std::vector<float> rgb;  // width * height * 3, row 0 = top (EXR's data window starts at the top)
};

// Encodes `rgb` (width * height * 3 floats, linear) as half floats. Negative / NaN values become 0, values above 65504 saturate.
std::vector<uint8_t> encodeExr(int width, int height, const float* rgb);
// False (and `error`) when the bytes are not an EXR this reader understands (scanline, R G B half or float channels, none / ZIPS / ZIP).
bool decodeExr(const uint8_t* data, size_t size, ExrImage& out, std::string* error = nullptr);

bool writeExr(const std::string& path, int width, int height, const float* rgb);
bool readExr(const std::string& path, ExrImage& out, std::string* error = nullptr);

}  // namespace core
