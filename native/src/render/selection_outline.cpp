// render/selection_outline -- see selection_outline.h.
#include "render/selection_outline.h"

#include <string>

namespace native::render {

namespace {

constexpr double kPointBoxHalf = 0.25;   // the picking box of a point entity (picking.cpp)

void addLine(std::vector<LineVertex>& out, double ax, double ay, double az, double bx, double by, double bz, uint32_t color) {
    out.push_back({{static_cast<float>(ax), static_cast<float>(ay), static_cast<float>(az)}, color});
    out.push_back({{static_cast<float>(bx), static_cast<float>(by), static_cast<float>(bz)}, color});
}

void addBox(std::vector<LineVertex>& out, const doc::Box& b, uint32_t color) {
    const double x[2] = {b.mn.x, b.mx.x}, y[2] = {b.mn.y, b.mx.y}, z[2] = {b.mn.z, b.mx.z};
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            addLine(out, x[0], y[i], z[j], x[1], y[i], z[j], color);
            addLine(out, x[i], y[0], z[j], x[i], y[1], z[j], color);
            addLine(out, x[i], y[j], z[0], x[i], y[j], z[1], color);
        }
}

void addLoop(std::vector<LineVertex>& out, const core::Brush& brush, const core::BrushFace& f, uint32_t color) {
    const size_t n = f.loop.size();
    for (size_t i = 0; i < n; ++i) {
        const uint32_t a = f.loop[i], b = f.loop[(i + 1) % n];
        if (a >= brush.vertices.size() || b >= brush.vertices.size()) continue;
        const core::Vec3& p = brush.vertices[a];
        const core::Vec3& q = brush.vertices[b];
        addLine(out, p.x, p.y, p.z, q.x, q.y, q.z, color);
    }
}

void addBrush(std::vector<LineVertex>& out, const core::Brush& brush, uint32_t color) {
    for (const core::BrushFace& f : brush.faces) addLoop(out, brush, f, color);
}

} // namespace

std::vector<LineVertex> selectionOutline(const scene::SceneSnapshot& snap, const std::vector<doc::SelRef>& items) {
    std::vector<LineVertex> out;
    if (!snap.doc || items.empty()) return out;
    const doc::DocumentVersion& doc = *snap.doc;
    const uint32_t brushColor = packColor(1.0f, 0.62f, 0.12f);
    const uint32_t faceColor = packColor(1.0f, 0.93f, 0.25f);
    const uint32_t entityColor = packColor(1.0f, 0.62f, 0.12f);
    const uint32_t instanceColor = packColor(0.35f, 0.85f, 1.0f);

    for (const doc::SelRef& s : items) {
        if (!s.instancePath.empty()) {
            // prefab content: the box around everything the instance made
            const std::string& id = s.instancePath.front();
            doc::Box box;
            for (const auto& [key, p] : snap.objects) {
                const size_t slash = key.instance.find('/');
                const std::string first = slash == std::string::npos ? key.instance : key.instance.substr(0, slash);
                if (first == id && p.bounds.valid()) box.add(p.bounds);
            }
            if (box.valid()) addBox(out, box, instanceColor);
            continue;
        }
        const int ei = doc.findEntity(s.entityId);
        if (ei < 0) continue;
        const auto& entity = doc.entities[static_cast<size_t>(ei)];
        if (s.brushId.empty()) {
            if (entity->brushes.empty()) {
                const core::Vec3& o = entity->typed.origin;
                doc::Box b;
                b.mn = {o.x - kPointBoxHalf, o.y - kPointBoxHalf, o.z - kPointBoxHalf};
                b.mx = {o.x + kPointBoxHalf, o.y + kPointBoxHalf, o.z + kPointBoxHalf};
                addBox(out, b, entityColor);
            } else {
                for (const auto& bp : entity->brushes) addBrush(out, bp->brush, entityColor);
            }
            continue;
        }
        const int bi = doc.findBrush(s.entityId, s.brushId);
        if (bi < 0) continue;
        const core::Brush& brush = entity->brushes[static_cast<size_t>(bi)]->brush;
        addBrush(out, brush, brushColor);
        if (s.faceId)
            for (const core::BrushFace& f : brush.faces)
                if (f.id == *s.faceId) addLoop(out, brush, f, faceColor);
    }
    return out;
}

} // namespace native::render
