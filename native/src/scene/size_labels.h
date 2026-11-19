// scene/size_labels -- the size of the selection in the views (port of shell/src/model/sizeLabels.ts, REQUIREMENTS section 33 H7): one label per axis of the
// selected brushes' box (never the same number twice on screen), or a selected face's width and height.
//
// Pure: the caller says how world points land on the screen and how many metres one pixel is at a point; the result is text and a world position the text
// is drawn at (just outside the edge it measures).
// Thread: any (pure).
// Spec: 05_RENDERER.md section 5, 08_MILESTONES.md T3.6.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/brush.h"
#include "scene/point_box.h"

namespace native::scene {

struct SizeLabel {
    std::string text;
    V3 at;
};
struct ScreenPoint {
    double x = 0, y = 0;
};
using ScreenOf = std::function<std::optional<ScreenPoint>(const V3&)>;
using MetresPerPixel = std::function<double(const V3&)>;

// "2 m", "0.25 m": at most three decimals, no trailing zeros.
std::string fmtSize(double metres);
// Drops labels whose text would cover an earlier one on screen (a small or far box).
std::vector<SizeLabel> withoutOverlaps(const std::vector<SizeLabel>& labels, const ScreenOf& screen);
// Labels for an axis-aligned box: for each axis except `skipAxis` (the view direction of an orthographic view, -1 = none) the edge along it that lies furthest
// out on screen from the box's centre, its label just outside it.
std::vector<SizeLabel> boxSizeLabels(const V3& min, const V3& max, const ScreenOf& screen, const MetresPerPixel& mpp, int skipAxis = -1);
// Width (along the face's longest edge) and height of a face, written outside the face's own rectangle: below it and to its right.
std::vector<SizeLabel> faceSizeLabels(const core::Brush& brush, const core::BrushFace& face, const MetresPerPixel& mpp);

} // namespace native::scene
