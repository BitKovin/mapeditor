// scene/render_mesh -- see render_mesh.h.
#include "scene/render_mesh.h"

#include <core/textures.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace native::scene {

namespace {

int16_t snorm16(float v) { return static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

uint32_t octEncode(float x, float y, float z) {
    const float l = std::fabs(x) + std::fabs(y) + std::fabs(z);
    if (l < 1e-12f) return 0;
    float px = x / l, py = y / l;
    if (z < 0) {
        const float ox = (1.0f - std::fabs(py)) * (px >= 0 ? 1.0f : -1.0f);
        const float oy = (1.0f - std::fabs(px)) * (py >= 0 ? 1.0f : -1.0f);
        px = ox;
        py = oy;
    }
    const uint16_t a = static_cast<uint16_t>(snorm16(px)), b = static_cast<uint16_t>(snorm16(py));
    return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 16);
}

void octDecode(uint32_t packed, float out[3]) {
    const float px = static_cast<int16_t>(packed & 0xFFFF) / 32767.0f, py = static_cast<int16_t>(packed >> 16) / 32767.0f;
    float x = px, y = py, z = 1.0f - std::fabs(px) - std::fabs(py);
    if (z < 0) {
        const float ox = (1.0f - std::fabs(y)) * (x >= 0 ? 1.0f : -1.0f);
        const float oy = (1.0f - std::fabs(x)) * (y >= 0 ? 1.0f : -1.0f);
        x = ox;
        y = oy;
    }
    const float l = std::sqrt(x * x + y * y + z * z);
    out[0] = x / l;
    out[1] = y / l;
    out[2] = z / l;
}

bool isSkyTexture(const std::string& name) {
    if (name.empty()) return false;
    if (core::isCubemapTexture(name)) return true;
    const std::string l = lower(name);
    return (l.size() >= 4 && l.compare(l.size() - 4, 4, "cube") == 0) || l.find("sky") != std::string::npos;
}

size_t RenderMesh::bytes() const {
    size_t n = sizeof(*this) + vertices.capacity() * sizeof(RenderVertex) + indices.capacity() * sizeof(uint32_t) + pieces.capacity() * sizeof(uint32_t);
    for (const auto& t : textures) n += t.capacity() + sizeof(std::string);
    return n;
}

uint32_t RenderMesh::slotOf(const std::string& texture) {
    for (uint32_t i = 0; i < textures.size(); ++i)
        if (textures[i] == texture) return i;
    textures.push_back(texture);
    return static_cast<uint32_t>(textures.size() - 1);
}

void RenderMesh::addTriangle(const RenderVertex& a, const RenderVertex& b, const RenderVertex& c, uint32_t piece) {
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    vertices.push_back(a);
    vertices.push_back(b);
    vertices.push_back(c);
    indices.push_back(base);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    pieces.push_back(piece);
}

std::shared_ptr<const RenderMesh> buildRawMesh(const core::Brush& brush, bool volume) {
    auto mesh = std::make_shared<RenderMesh>();
    mesh->kind = volume ? RenderMesh::Kind::Volume : RenderMesh::Kind::Solid;
    for (const core::BrushFace& f : brush.faces) {
        if (f.loop.size() < 3) continue;
        bool ok = true;
        for (uint32_t i : f.loop) ok = ok && i < brush.vertices.size();
        if (!ok) continue;
        const core::Vec3 n = brush.faceNormal(f);
        const uint32_t octN = octEncode(static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z));
        const std::string texture = f.texture.empty() ? std::string("dev/grey") : f.texture;
        const uint32_t slot = mesh->slotOf(texture);
        const uint16_t flags = static_cast<uint16_t>((isSkyTexture(texture) ? kFlagSky : 0) | (volume ? kFlagVolume : 0));
        const bool ownUv = f.uvs.size() == f.loop.size();
        const auto corner = [&](size_t k) {
            const core::Vec3& p = brush.vertices[f.loop[k]];
            RenderVertex v{};
            v.pos[0] = static_cast<float>(p.x);
            v.pos[1] = static_cast<float>(p.y);
            v.pos[2] = static_cast<float>(p.z);
            v.normal = octN;
            const core::Vec2 uv = ownUv ? f.uvs[k] : core::projectAlbedoUv(n, p, f.projection);
            v.uv[0] = static_cast<float>(uv.u);
            v.uv[1] = static_cast<float>(uv.v);
            v.flags = flags;
            v.texture = slot;
            return v;
        };
        for (size_t k = 1; k + 1 < f.loop.size(); ++k) mesh->addTriangle(corner(0), corner(k), corner(k + 1));
    }
    return mesh;
}

} // namespace native::scene
