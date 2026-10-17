#pragma once
// Shared helpers for the test suite: fixture builders that produce the JSON
// the Shell would send, and a temp directory for file-based tests.
#include "core/brush.h"
#include "core/document.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace testutil {

using nlohmann::json;
using namespace core;

inline json vec3Json(const Vec3& v) { return json::array({v.x, v.y, v.z}); }

inline json brushJson(const Brush& b) {
    json faces = json::array();
    for (const auto& f : b.faces) {
        json fj = {{"id", f.id}, {"loop", f.loop}, {"texture", f.texture}};
        if (!f.uvs.empty()) {
            json uvs = json::array();
            for (const auto& uv : f.uvs) uvs.push_back(json::array({uv.u, uv.v}));
            fj["uvs"] = uvs;
        }
        faces.push_back(fj);
    }
    json verts = json::array();
    for (const auto& v : b.vertices) verts.push_back(vec3Json(v));
    return {{"id", b.id}, {"op", b.op == BrushOp::Add ? "add" : "subtract"}, {"vertices", verts}, {"faces", faces}};
}

// Prism from an XZ profile extruded along Y. Winding is fixed up so faces
// point outward whichever way the profile was given.
inline Brush makePrism(const std::string& id, const std::vector<std::pair<double, double>>& xz, double y0,
                        double y1) {
    Brush b;
    b.id = id;
    const uint32_t n = static_cast<uint32_t>(xz.size());
    for (auto& p : xz) b.vertices.push_back({p.first, y0, p.second});
    for (auto& p : xz) b.vertices.push_back({p.first, y1, p.second});
    BrushFace bottom, top;
    bottom.id = 0;
    top.id = 1;
    for (uint32_t i = 0; i < n; ++i) {
        bottom.loop.push_back(n - 1 - i);
        top.loop.push_back(n + i);
    }
    b.faces = {bottom, top};
    for (uint32_t i = 0; i < n; ++i) {
        BrushFace s;
        s.id = 2 + i;
        s.loop = {i, (i + 1) % n, n + (i + 1) % n, n + i};
        b.faces.push_back(s);
    }
    if (b.signedVolume() < 0)
        for (auto& f : b.faces) std::reverse(f.loop.begin(), f.loop.end());
    return b;
}

// One brush made of two closed solids, `b` appended to `a` (vertices, faces with ids + 100). Where they touch, both faces stay, back to back:
// a concave brush as mesh editing leaves them (e.g. a hollow building of one brush).
inline Brush gluedBrush(Brush a, const Brush& b) {
    const uint32_t base = static_cast<uint32_t>(a.vertices.size());
    a.vertices.insert(a.vertices.end(), b.vertices.begin(), b.vertices.end());
    for (BrushFace f : b.faces) {
        for (auto& i : f.loop) i += base;
        f.id += 100;
        a.faces.push_back(f);
    }
    return a;
}

inline Brush lPrism(const std::string& id) {
    return makePrism(id, {{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}}, 0, 1);
}

inline json entityJson(const std::string& id, const std::string& classname, const std::vector<Brush>& brushes,
                        const Vec3& origin = {0, 0, 0}) {
    json bs = json::array();
    for (auto& b : brushes) bs.push_back(brushJson(b));
    return {{"id", id}, {"classname", classname}, {"origin", vec3Json(origin)}, {"brushes", bs}};
}

inline json instanceJson(const std::string& id, const std::string& file, const Vec3& origin = {0, 0, 0}) {
    return {{"id", id}, {"file", file}, {"origin", vec3Json(origin)}};
}

inline json docJson(const std::vector<json>& entities, const std::vector<json>& instances = {}) {
    return {{"schemaVersion", kSchemaVersion}, {"entities", entities}, {"prefabInstances", instances}};
}

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("mapeditor_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    void write(const std::string& rel, const json& j) const {
        auto full = path / rel;
        std::filesystem::create_directories(full.parent_path());
        std::error_code ec;
        const bool existed = std::filesystem::exists(full, ec);
        const auto prev = existed ? std::filesystem::last_write_time(full, ec) : std::filesystem::file_time_type{};
        std::ofstream(full) << j.dump(2);
        // Scene's prefab cache is keyed on mtime. Filesystem timestamps can be coarse (about 15 ms on Windows),
        // so a rewrite right after the first write may keep the old mtime. Force it to advance.
        if (existed) {
            const auto now = std::filesystem::last_write_time(full, ec);
            if (!ec && now <= prev) std::filesystem::last_write_time(full, prev + std::chrono::seconds(1), ec);
        }
    }
    json read(const std::string& rel) const {
        std::ifstream in(path / rel);
        return json::parse(in);
    }
    bool exists(const std::string& rel) const { return std::filesystem::exists(path / rel); }
};

}  // namespace testutil
