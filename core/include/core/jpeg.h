#pragma once
#include <cstdint>
#include <vector>

namespace core {

// A baseline JPEG encoder (the lightmap pages the export stores as .jpg when their light fits 8 bits, DESIGN.md §12): 8-bit RGB, no chroma subsampling
// (4:4:4: subsampled chroma would bleed the colour of one chart into its neighbour), the standard Huffman tables, the Annex K quantisation tables scaled
// by `quality` (1..100, libjpeg's scale). Any decoder reads it; the editor's own baseline decoder (albedo.h decodeImage) does.
std::vector<uint8_t> encodeJpeg(int width, int height, const uint8_t* rgb, int quality = 95);

}  // namespace core
