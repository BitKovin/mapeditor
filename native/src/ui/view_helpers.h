// ui/view_helpers -- the little projection helpers the viewport tools share: the ray through a pixel and the pixel of a world point (logical pixels of the window).
// Thread: UI thread.
#pragma once

#include <QPointF>
#include <QSizeF>

#include <algorithm>

#include "render/picking.h"
#include "render/view_state.h"
#include "tools/modal.h"

namespace native::ui {

inline render::Ray rayAt(const render::ViewState& view, QSizeF size, QPointF p) {
    return render::rayThrough(view.viewProj, p.x() / std::max(1.0, size.width()) * 2.0 - 1.0, p.y() / std::max(1.0, size.height()) * 2.0 - 1.0);
}

inline tools::Pixel screenOf(const render::ViewState& view, QSizeF size, const render::Vec3& p) {
    const render::Vec3 n = render::mat4Transform(view.viewProj, p);
    return {(n.x * 0.5 + 0.5) * size.width(), (n.y * 0.5 + 0.5) * size.height()};
}

} // namespace native::ui
