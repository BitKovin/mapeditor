// render/glyph_atlas -- the text of the overlays: a glyph atlas (printable ASCII) made once with QPainter, drawn as textured quads.
//
// Owns: GlyphAtlas (an RGBA8 image: R = coverage of the glyph, G = coverage of the glyph with its outline; the metrics of every glyph) and the layout of a
// text into glyph quads around an anchor. The atlas is made by ui/glyph_atlas_qt (the only place that needs Qt), the render thread puts the image into the
// texture table and draws the quads with the label pipeline of mesh_pass: white text with a dark outline, in pixels of the viewport.
// Thread: the atlas is immutable once made (shared between threads).
// Spec: 05_RENDERER.md section 5 (text), 08_MILESTONES.md T3.6.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace native::render {

constexpr int kFirstGlyph = 32, kLastGlyph = 126;   // printable ASCII

struct Glyph {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;   // the cell in the image (outline margin included)
    float w = 0, h = 0;                     // the cell in pixels
    float advance = 0;                      // pen movement in pixels
};

struct GlyphAtlas {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;              // width * height * 4
    Glyph glyphs[kLastGlyph - kFirstGlyph + 1];
    float lineHeight = 0;                   // pixels, the height of a cell
    float margin = 0;                       // the outline margin inside a cell, pixels

    bool ok() const { return width > 0 && height > 0 && rgba.size() == static_cast<size_t>(width) * height * 4; }
    const Glyph* glyph(char c) const {
        const int i = static_cast<unsigned char>(c);
        return i >= kFirstGlyph && i <= kLastGlyph ? &glyphs[i - kFirstGlyph] : nullptr;
    }
    float textWidth(const std::string& text) const {
        float w = 0;
        for (char c : text)
            if (const Glyph* g = glyph(c)) w += g->advance;
        return w;
    }
};

// One vertex of a label quad: the world anchor, the offset from it in pixels (y down), the atlas coordinates, a colour (r | g << 8 | b << 16 | a << 24).
struct LabelVertex {
    float anchor[3];
    float offset[2];
    float uv[2];
    uint32_t rgba;
};
static_assert(sizeof(LabelVertex) == 32);

// Appends the quads (two triangles a glyph) of `text` centred on `anchor` (x centred, y centred on the line).
void layoutText(const GlyphAtlas& atlas, const std::string& text, const double anchor[3], uint32_t rgba, std::vector<LabelVertex>& out);

} // namespace native::render
