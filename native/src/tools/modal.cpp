// tools/modal -- see modal.h.
#include "tools/modal.h"

#include <cmath>
#include <cstdio>

namespace native::tools {

std::optional<std::string> typeInto(const std::string& buf, const std::string& key) {
    if (key.size() == 1 && key[0] >= '0' && key[0] <= '9') return buf + key;
    if (key == "." || key == ",") {
        if (buf.find('.') != std::string::npos) return buf;
        return buf.empty() || buf == "-" ? buf + "0." : buf + ".";
    }
    if (key == "-") return !buf.empty() && buf[0] == '-' ? buf.substr(1) : "-" + buf;
    if (key == "Backspace") return buf.empty() ? buf : buf.substr(0, buf.size() - 1);
    return std::nullopt;
}

std::optional<double> typedValue(const std::string& buf) {
    if (buf.empty() || buf == "-" || buf == ".") return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(buf.c_str(), &end);
    if (end == buf.c_str() || *end != '\0' || !std::isfinite(v)) return std::nullopt;
    return v;
}

std::optional<Vec3> planePoint(const Ray& ray, const Vec3& point, const Vec3& normal) {
    const double d = render::dot(normal, ray.dir);
    if (std::fabs(d) < 1e-9) return std::nullopt;
    const double t = render::dot(normal, point - ray.origin) / d;
    if (!(t > 0)) return std::nullopt;
    return ray.origin + ray.dir * t;
}

Vec3 grabDelta(const Ray& ray, const Vec3& start, const Vec3& pivot, const Vec3& camForward, Axis axis, double step) {
    if (axis < 0) {
        const auto p = planePoint(ray, pivot, camForward);
        if (!p) return {0, 0, 0};
        return {snapTo(p->x - start.x, step), snapTo(p->y - start.y, step), snapTo(p->z - start.z, step)};
    }
    const Vec3 d = axisDir(axis);
    const auto t = lineDistance(ray, pivot, d);
    const auto t0 = lineDistance(Ray{ray.origin, render::normalize(start - ray.origin)}, pivot, d);
    if (!t || !t0) return {0, 0, 0};
    return d * snapTo(*t - *t0, step);
}

double rotateAngle(Pixel pivot, Pixel from, Pixel to, bool flip, double stepRad) {
    const double pi = 3.14159265358979323846;
    const double a0 = std::atan2(from.y - pivot.y, from.x - pivot.x), a1 = std::atan2(to.y - pivot.y, to.x - pivot.x);
    double d = a1 - a0;
    while (d > pi) d -= 2 * pi;
    while (d < -pi) d += 2 * pi;
    if (flip) d = -d;   // screen y points down, so a counter-clockwise mouse move has a negative atan2 change
    return stepRad > 0 ? std::round(d / stepRad) * stepRad : d;
}

double scaleFactor(Pixel pivot, Pixel from, Pixel to, double step) {
    const double d0 = std::hypot(from.x - pivot.x, from.y - pivot.y), d1 = std::hypot(to.x - pivot.x, to.y - pivot.y);
    const double f = d0 < 4 ? 1 : d1 / d0;
    return step > 0 ? std::max(step, std::round(f / step) * step) : f;
}

Vec3 scaleVector(double f, Axis axis) { return axis < 0 ? Vec3{f, f, f} : Vec3{axis == 0 ? f : 1, axis == 1 ? f : 1, axis == 2 ? f : 1}; }

double widthFromMouse(Pixel from, Pixel to, double metresPerPixel, double step) {
    const double w = std::max(0.0, std::hypot(to.x - from.x, to.y - from.y) * metresPerPixel);
    return step > 0 ? std::round(w / step) * step : w;
}

double metresPerPixelAt(double distance, double fovY, double heightPx) { return (2 * distance * std::tan(fovY / 2)) / std::max(1.0, heightPx); }

int stepSegments(int current, double deltaY, int max) { return std::max(1, std::min(max, current + (deltaY < 0 ? 1 : (deltaY > 0 ? -1 : 0)))); }

Pixel advanceVirtualCursor(Pixel p, double dx, double dy) { return {p.x + dx, p.y + dy}; }

std::string describeModal(ModalKind kind, double value, Axis axis, const std::string& typing, int segments) {
    char buf[96];
    std::string ax = axis < 0 ? "" : std::string(" ") + "XYZ"[axis];
    std::string v;
    if (!typing.empty()) {
        v = typing + "_";
    } else if (kind == ModalKind::Rotate) {
        std::snprintf(buf, sizeof buf, "%.1f\xC2\xB0", value * 180.0 / 3.14159265358979323846);
        v = buf;
    } else if (kind == ModalKind::Scale) {
        std::snprintf(buf, sizeof buf, "\xC3\x97%.3f", value);
        v = buf;
    } else {
        std::snprintf(buf, sizeof buf, "%.3f m", value);
        v = buf;
    }
    const char* names[] = {"Move", "Rotate", "Scale", "Extrude", "Inset", "Bevel", "Loop cut"};
    std::string seg;
    if (kind == ModalKind::Bevel && segments >= 0) seg = ", " + std::to_string(segments) + " segment" + (segments == 1 ? "" : "s") + " (wheel)";
    return std::string(names[static_cast<int>(kind)]) + ax + ": " + v + seg + "   (type a number \xC2\xB7 X/Y/Z axis \xC2\xB7 click/Enter confirm \xC2\xB7 Esc cancel)";
}

} // namespace native::tools
