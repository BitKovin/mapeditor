// tools/status_text -- see status_text.h.
#include "tools/status_text.h"

#include <cmath>
#include <cstdio>

namespace native::tools {

std::string shortNumber(double v) {
    if (!std::isfinite(v)) return "0";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() ? "0" : s;
}

std::string gridText(const render::ViewportSettings& s) {
    std::string t = "Grid " + shortNumber(s.gridSize) + " m, snap ";
    t += s.snapEnabled ? "on, rot " + shortNumber(s.rotSnap) + "\xC2\xB0" : "off";
    return t;
}

namespace {
std::string plural(size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }
} // namespace

std::string selectionText(const doc::Selection& selection, const doc::DocumentVersion* doc) {
    size_t entities = 0, brushes = 0, faces = 0, instances = 0;
    for (const doc::SelRef& r : selection.items()) {
        if (r.isFace()) ++faces;
        else if (r.isBrush()) ++brushes;
        else if (r.isEntity()) ++entities;
        else if (r.isInstance()) ++instances;
    }
    const size_t total = entities + brushes + faces + instances;
    if (total == 0) return "Nothing selected";
    if (total == 1 && entities == 1 && doc) {
        const doc::SelRef& r = selection.items().front();
        const int i = doc->findEntity(r.entityId);
        if (i >= 0 && r.instancePath.empty()) return doc->entities[static_cast<size_t>(i)]->typed.classname;
    }
    std::string t;
    const auto add = [&](size_t n, const char* one, const char* many) {
        if (!n) return;
        if (!t.empty()) t += ", ";
        t += plural(n, one, many);
    };
    add(entities, "entity", "entities");
    add(brushes, "brush", "brushes");
    add(faces, "face", "faces");
    add(instances, "prefab instance", "prefab instances");
    return t;
}

std::string folderText(const std::string& assetsRoot) {
    std::string s = assetsRoot;
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    if (s.empty()) return "No game folder";
    const size_t slash = s.find_last_of("/\\");
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

std::string mapText(const doc::DocumentVersion* doc, bool dirty) {
    if (!doc) return "No map";
    std::string t = doc->path.empty() ? "untitled" : doc->path;
    if (dirty) t += " *";
    return t;
}

} // namespace native::tools
