// ui/glyph_atlas_qt -- makes the glyph atlas of the overlay text with QPainter (the Shell's label canvas: 12 px semi-bold sans-serif, white with a dark outline).
// Thread: UI thread (QPainter).
// Spec: 05_RENDERER.md section 5, 08_MILESTONES.md T3.6.
#pragma once

#include <memory>

#include "render/glyph_atlas.h"

namespace native::ui {

// `scale` = device pixel ratio of the window the text is drawn in (the atlas is made at that size: one atlas pixel = one screen pixel).
std::shared_ptr<const render::GlyphAtlas> makeGlyphAtlas(double scale);

} // namespace native::ui
