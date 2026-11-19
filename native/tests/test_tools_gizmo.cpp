// Tests of tools/gizmo_overlay: the lines of the three gizmo modes.
#include <doctest.h>

#include "tools/gizmo_overlay.h"

using namespace native;

TEST_CASE("gizmo overlay: line counts per mode and the active axis is yellow") {
    const render::Vec3 pivot{1, 2, 3};
    const auto move = tools::gizmoLines(pivot, 2.0, render::GizmoMode::Move, -1);
    CHECK(move.size() == 3 * (1 + 4) * 2);           // per axis a shaft and four arrowhead lines
    const auto rot = tools::gizmoLines(pivot, 2.0, render::GizmoMode::Rotate, -1);
    CHECK(rot.size() == 3 * 48 * 2);                  // three rings of 48 segments
    const auto scale = tools::gizmoLines(pivot, 2.0, render::GizmoMode::Scale, -1);
    CHECK(scale.size() == 3 * (1 + 12) * 2);          // a shaft and a cube of 12 edges
    // the move gizmo starts at the pivot
    CHECK(move[0].pos[0] == doctest::Approx(1.0));
    CHECK(move[0].pos[1] == doctest::Approx(2.0));
    CHECK(move[1].pos[0] == doctest::Approx(3.0));
    // the highlighted axis differs in colour from the same axis not highlighted
    const auto hot = tools::gizmoLines(pivot, 2.0, render::GizmoMode::Move, 1);
    CHECK(hot[0].rgba == move[0].rgba);                // x unchanged
    CHECK(hot[10].rgba != move[10].rgba);              // y highlighted (second axis starts at vertex 10)
}
