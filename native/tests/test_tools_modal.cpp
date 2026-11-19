// T4.1: the maths of the modal operators and the transform gizmo, converted from shell/tests/modal.test.ts (mouse to parameter; the mesh picking cases belong to
// M7) plus the gizmo hit tests. Headless.
#include <doctest.h>

#include <cmath>

#include "tools/gizmo.h"
#include "tools/modal.h"

using namespace native::tools;

namespace {
Ray ray(double x, double y) { return Ray{{x, y, 10}, {0, 0, -1}}; }
constexpr double kPi = 3.14159265358979323846;
} // namespace

TEST_CASE("numeric entry builds numbers key by key") {
    std::string b;
    for (const char* k : {"1", ".", "5"}) b = *typeInto(b, k);
    CHECK(b == "1.5");
    CHECK(*typedValue(b) == 1.5);
    CHECK(*typeInto("1.5", ".") == "1.5");   // one decimal point only
    CHECK(*typeInto("", ".") == "0.");
    CHECK(*typeInto("2", "-") == "-2");
    CHECK(*typeInto("-2", "-") == "2");
    CHECK(*typeInto("12", "Backspace") == "1");
    CHECK_FALSE(typeInto("1", "x").has_value());   // not numeric: the caller treats it as an axis key etc.
    CHECK_FALSE(typedValue("").has_value());
    CHECK_FALSE(typedValue("-").has_value());
}

TEST_CASE("grab on the view plane follows the mouse, with grid snapping") {
    const Vec3 d = grabDelta(ray(3.2, 1.1), {1, 1, 0}, {1, 1, 0}, {0, 0, -1}, -1, 0.5);
    CHECK(d.x == 2);
    CHECK(d.y == 0);
    CHECK(d.z == 0);
    CHECK(grabDelta(ray(3.2, 1.1), {1, 1, 0}, {1, 1, 0}, {0, 0, -1}, -1, 0).x == doctest::Approx(2.2));
}

TEST_CASE("grab along an axis only moves along it") {
    const Vec3 d = grabDelta(ray(4, 7), {1, 0, 0}, {1, 0, 0}, {0, 0, -1}, 0, 0);
    CHECK(d.y == 0);
    CHECK(d.z == 0);
    CHECK(d.x == doctest::Approx(3));
}

TEST_CASE("the plane through a point facing the camera") {
    const auto p = planePoint(ray(2, 3), {0, 0, 0}, {0, 0, 1});
    REQUIRE(p.has_value());
    CHECK(p->x == 2);
    CHECK(p->y == 3);
    CHECK(p->z == 0);
    CHECK_FALSE(planePoint(Ray{{0, 0, 1}, {1, 0, 0}}, {0, 0, 0}, {0, 0, 1}).has_value());
}

TEST_CASE("rotation follows the angle around the pivot, snapping to steps") {
    // the mouse moved counter-clockwise on screen
    CHECK(rotateAngle({0, 0}, {10, 0}, {0, -10}, true, 0) == doctest::Approx(kPi / 2));
    CHECK(rotateAngle({0, 0}, {10, 0}, {0, -10}, false, 0) == doctest::Approx(-kPi / 2));
    CHECK(rotateAngle({0, 0}, {10, 0}, {10, -1}, true, kPi / 12) == doctest::Approx(0).epsilon(1e-9));
}

TEST_CASE("scale is the ratio of distances, vectors respect the axis") {
    CHECK(scaleFactor({0, 0}, {10, 0}, {20, 0}, 0) == doctest::Approx(2));
    CHECK(scaleFactor({0, 0}, {1, 0}, {50, 0}, 0) == 1);   // started on the pivot: no scaling
    const Vec3 u = scaleVector(2, -1), y = scaleVector(2, 1);
    CHECK((u.x == 2 && u.y == 2 && u.z == 2));
    CHECK((y.x == 1 && y.y == 2 && y.z == 1));
}

TEST_CASE("width for inset / bevel never goes negative and snaps") {
    CHECK(widthFromMouse({0, 0}, {30, 40}, 0.01, 0) == doctest::Approx(0.5));
    CHECK(widthFromMouse({0, 0}, {30, 40}, 0.01, 0.25) == doctest::Approx(0.5));
    CHECK(metresPerPixelAt(10, kPi / 2, 1000) == doctest::Approx(0.02));
}

TEST_CASE("wheel steps bevel segments within 1..max") {
    CHECK(stepSegments(1, -100, 32) == 2);   // wheel up adds
    CHECK(stepSegments(2, 100, 32) == 1);    // wheel down removes
    CHECK(stepSegments(1, 100, 32) == 1);    // never below 1 (1 = chamfer)
    CHECK(stepSegments(32, -100, 32) == 32); // never above the maximum
    CHECK(stepSegments(5, 0, 32) == 5);
}

TEST_CASE("a locked-pointer modal keeps a virtual cursor that may leave the canvas") {
    Pixel p{10, 10};
    p = advanceVirtualCursor(p, -50, 7);
    p = advanceVirtualCursor(p, -50, 7);
    CHECK(p.x == -90);
    CHECK(p.y == 24);
}

TEST_CASE("the status line of a modal") {
    CHECK(describeModal(ModalKind::Bevel, 0.1, -1, "", 4).find("4 segments (wheel)") != std::string::npos);
    CHECK(describeModal(ModalKind::Bevel, 0.1, -1, "", 1).find("1 segment (wheel)") != std::string::npos);
    CHECK(describeModal(ModalKind::Inset, 0.1, -1, "").find("segment") == std::string::npos);
    CHECK(describeModal(ModalKind::Grab, 1.5, 0, "").find("Move X: 1.500 m") != std::string::npos);
    CHECK(describeModal(ModalKind::Rotate, kPi / 2, -1, "").find("90.0\xC2\xB0") != std::string::npos);
    CHECK(describeModal(ModalKind::Scale, 2, 2, "").find("Scale Z: \xC3\x97" "2.000") != std::string::npos);
    CHECK(describeModal(ModalKind::Extrude, 0, -1, "1.5").find("1.5_") != std::string::npos);
}

TEST_CASE("gizmo: axis handles, dragging along an axis, rings") {
    const Vec3 pivot{0, 0, 0};
    // a ray from above hitting the x handle (length 2) near its middle
    CHECK(hitAxis(Ray{{1, 5, 0}, {0, -1, 0}}, pivot, 2, 0.1) == 0);
    CHECK(hitAxis(Ray{{0, 1, 5}, {0, 0, -1}}, pivot, 2, 0.1) == 1);   // a ray along -z at height 1 meets the y handle
    CHECK(hitAxis(Ray{{5, 5, 5}, {0, -1, 0}}, pivot, 2, 0.1) == -1);
    CHECK(hitDir(Ray{{1, 5, 0}, {0, -1, 0}}, pivot, {1, 0, 0}, 2, 0.1));
    CHECK_FALSE(hitDir(Ray{{3, 5, 0}, {0, -1, 0}}, pivot, {1, 0, 0}, 2, 0.1));   // beyond the handle's end
    // dragging: the ray's closest point on the axis, minus where the drag began, snapped
    const double t0 = *axisDistance(Ray{{1, 5, 0}, {0, -1, 0}}, pivot, 0);
    CHECK(t0 == doctest::Approx(1));
    const Vec3 d = dragDelta(Ray{{3.2, 5, 0}, {0, -1, 0}}, pivot, 0, t0, 0.5);
    CHECK(d.x == doctest::Approx(2));
    CHECK(d.y == 0);
    // the ring about y has radius 1: a ray down at (1, *, 0) passes through it
    CHECK(hitRing(Ray{{1, 5, 0}, {0, -1, 0}}, pivot, 1, 0.1) == 1);
    CHECK(hitRing(Ray{{3, 5, 0}, {0, -1, 0}}, pivot, 1, 0.1) == -1);
    // right-handed angles about y: +z is at 0 degrees, +x at 90 (basis u = z, v = x)
    CHECK(*ringAngle(Ray{{0, 5, 1}, {0, -1, 0}}, pivot, 1) == doctest::Approx(0));
    CHECK(*ringAngle(Ray{{1, 5, 0}, {0, -1, 0}}, pivot, 1) == doctest::Approx(kPi / 2));
    CHECK(angleDelta(3.0, -3.0) == doctest::Approx(2 * kPi - 6.0));
    CHECK(ringLines(pivot, 1, 1.0, 12).size() == 12 * 6);
    CHECK(gizmoLength(10) == doctest::Approx(1.8));
}
