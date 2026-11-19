// tools/modal -- the maths of the modal operators G / R / S / E / I / bevel / loop cut (port of shell/src/tools/modal.ts): numeric entry, and turning mouse
// positions into a parameter (translation, angle, scale factor, width). The viewport feeds the mouse (or typed digits) and applies the result every move,
// always from the document as it was when the operation began, so a whole modal is one undo step.
// Thread: any (pure).
// Spec: 08_MILESTONES.md T4.1, DESIGN sections 8 and 17.
#pragma once

#include <optional>
#include <string>

#include "tools/gizmo.h"

namespace native::tools {

enum class ModalKind { Grab, Rotate, Scale, Extrude, Inset, Bevel, LoopCut };

struct Pixel {
    double x = 0, y = 0;
};

// Numeric entry: digits, one '.', leading '-', Backspace. Returns the new buffer, nullopt when the key is not numeric. `key` is a Qt-free key name
// ("0".."9", ".", ",", "-", "Backspace").
std::optional<std::string> typeInto(const std::string& buf, const std::string& key);
std::optional<double> typedValue(const std::string& buf);

// Where the ray meets the plane through `point` with `normal` (in front of the ray only).
std::optional<Vec3> planePoint(const Ray& ray, const Vec3& point, const Vec3& normal);
// Grab: the translation. Unconstrained (axis < 0) it slides on the plane through the pivot facing the camera; with an axis it follows the mouse along it.
// `start` is the point under the mouse when the modal began; `step` snaps each component (0 = free).
Vec3 grabDelta(const Ray& ray, const Vec3& start, const Vec3& pivot, const Vec3& camForward, Axis axis, double step);
// Rotation angle (radians) from the mouse's angle around the pivot on screen; `flip` when the axis points away from the viewer.
double rotateAngle(Pixel pivot, Pixel from, Pixel to, bool flip, double stepRad);
// Scale factor: the mouse's distance from the pivot now against at the start.
double scaleFactor(Pixel pivot, Pixel from, Pixel to, double step);
Vec3 scaleVector(double f, Axis axis);
// Width for inset / bevel from how far the mouse moved from where it started, in metres at the pivot's depth.
double widthFromMouse(Pixel from, Pixel to, double metresPerPixel, double step);
double metresPerPixelAt(double distance, double fovY, double heightPx);
// One wheel notch up adds a bevel segment, one down removes one; always within 1..max.
int stepSegments(int current, double deltaY, int max);
// The cursor of a modal that has locked the pointer keeps moving by the raw mouse movement.
Pixel advanceVirtualCursor(Pixel p, double dx, double dy);
// The status line of a running modal. `segments` < 0 = not a bevel.
std::string describeModal(ModalKind kind, double value, Axis axis, const std::string& typing, int segments = -1);

} // namespace native::tools
