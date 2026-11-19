// render/glyph_atlas -- see glyph_atlas.h.
#include "render/glyph_atlas.h"

namespace native::render {

void layoutText(const GlyphAtlas& atlas, const std::string& text, const double anchor[3], uint32_t rgba, std::vector<LabelVertex>& out) {
    if (!atlas.ok()) return;
    float x = -atlas.textWidth(text) / 2 - atlas.margin;
    const float y0 = -atlas.lineHeight / 2, y1 = atlas.lineHeight / 2;
    const float ax = static_cast<float>(anchor[0]), ay = static_cast<float>(anchor[1]), az = static_cast<float>(anchor[2]);
    for (char c : text) {
        const Glyph* g = atlas.glyph(c);
        if (!g) continue;
        if (c != ' ') {
            const auto v = [&](float px, float py, float u, float w) { return LabelVertex{{ax, ay, az}, {px, py}, {u, w}, rgba}; };
            const LabelVertex a = v(x, y0, g->u0, g->v0), b = v(x + g->w, y0, g->u1, g->v0), cc = v(x + g->w, y1, g->u1, g->v1), d = v(x, y1, g->u0, g->v1);
            out.insert(out.end(), {a, b, cc, a, cc, d});
        }
        x += g->advance;
    }
}

} // namespace native::render
