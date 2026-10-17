// Minimal reader for the OBJ exporter's .lightvol file (REQUIREMENTS §31, DESIGN "Exporters"), and the light at one point.
// Standalone: C++17, no dependencies.   cl /std:c++17 /EHsc lightvol_sample.cpp      g++ -std=c++17 lightvol_sample.cpp
// Usage: lightvol_sample <map.lightvol> [x y z] [nx ny nz]      (point in metres, Y up; normal of the surface to light, default up)
//
// File (little endian): "LVOL", uint32 version (1), float origin[3], float cell, int32 nx, ny, nz, styleCount,
// then per style: int32 style, nx*ny*nz probes of 9 bytes, x fastest, then y, then z. Probe (x, y, z) sits at origin + (x, y, z) * cell.
// Probe bytes: ambient rgb, directed rgb, lat, lng, flags. Colour = byte / 255 * 2 (linear). Direction (towards the light):
// lat = byte / 255 * pi, lng = byte / 255 * 2pi - pi, dir = (sin lat cos lng, cos lat, sin lat sin lng). flags bit 0 = valid (not inside a wall).
// Light for a normal n = ambient + directed * max(0, n . dir)   (what the editor's Lit view does for entities).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

struct Vec3 { float x = 0, y = 0, z = 0; };
static Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 normalize(Vec3 a) { const float l = std::sqrt(dot(a, a)); return l > 1e-12f ? a * (1.0f / l) : Vec3{0, 1, 0}; }

struct Probe { Vec3 ambient, directed, dir{0, 1, 0}; bool valid = false; };

struct LightVolume {
    Vec3 origin;
    float cell = 1;
    int nx = 0, ny = 0, nz = 0;
    std::vector<int> styles;                 // light style id of each layer (0 = the base light)
    std::vector<std::vector<Probe>> layers;  // one grid per style

    bool load(const char* path) {
        std::ifstream f(path, std::ios::binary);
        const std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (b.size() < 40 || std::memcmp(b.data(), "LVOL", 4) != 0) return false;
        uint32_t version; float hdr[4]; int32_t dims[4];
        std::memcpy(&version, &b[4], 4);
        std::memcpy(hdr, &b[8], 16);
        std::memcpy(dims, &b[24], 16);
        if (version != 1) return false;
        origin = {hdr[0], hdr[1], hdr[2]}; cell = hdr[3];
        nx = dims[0]; ny = dims[1]; nz = dims[2];
        const size_t count = size_t(nx) * ny * nz;
        size_t at = 40;
        for (int s = 0; s < dims[3]; ++s) {
            if (at + 4 + count * 9 > b.size()) return false;
            int32_t style; std::memcpy(&style, &b[at], 4); at += 4;
            std::vector<Probe> grid(count);
            for (Probe& p : grid) {
                const uint8_t* q = &b[at]; at += 9;
                p.ambient = {q[0] / 255.0f * 2, q[1] / 255.0f * 2, q[2] / 255.0f * 2};
                p.directed = {q[3] / 255.0f * 2, q[4] / 255.0f * 2, q[5] / 255.0f * 2};
                const float lat = q[6] / 255.0f * 3.14159265f, lng = q[7] / 255.0f * 6.28318531f - 3.14159265f;
                p.dir = {std::sin(lat) * std::cos(lng), std::cos(lat), std::sin(lat) * std::sin(lng)};
                p.valid = (q[8] & 1) != 0;
            }
            styles.push_back(style);
            layers.push_back(std::move(grid));
        }
        return true;
    }

    // Trilinear over the 8 probes around `p`, skipping invalid ones (inside walls); the direction is weighted by brightness so dark probes
    // do not steer it. (The editor also falls back to the nearest valid probe when all 8 are invalid; left out here.)
    Probe sample(const std::vector<Probe>& grid, Vec3 p) const {
        const Vec3 g = {(p.x - origin.x) / cell, (p.y - origin.y) / cell, (p.z - origin.z) / cell};
        const int x0 = int(std::floor(g.x)), y0 = int(std::floor(g.y)), z0 = int(std::floor(g.z));
        const float fx = g.x - x0, fy = g.y - y0, fz = g.z - z0;
        Probe out; Vec3 dir; float wsum = 0;
        for (int c = 0; c < 8; ++c) {
            const int dx = c & 1, dy = (c >> 1) & 1, dz = c >> 2;
            auto clampi = [](int v, int n) { return v < 0 ? 0 : (v >= n ? n - 1 : v); };
            const Probe& q = grid[(size_t(clampi(z0 + dz, nz)) * ny + clampi(y0 + dy, ny)) * nx + clampi(x0 + dx, nx)];
            const float w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * (dz ? fz : 1 - fz);
            if (!q.valid || w <= 0) continue;
            out.ambient = out.ambient + q.ambient * w;
            out.directed = out.directed + q.directed * w;
            dir = dir + q.dir * (w * (0.2126f * q.directed.x + 0.7152f * q.directed.y + 0.0722f * q.directed.z + 1e-6f));
            wsum += w;
        }
        if (wsum <= 0) return out;  // not valid: no probe around (e.g. inside a wall)
        out.ambient = out.ambient * (1 / wsum);
        out.directed = out.directed * (1 / wsum);
        out.dir = normalize(dir);
        out.valid = true;
        return out;
    }
};

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: lightvol_sample <map.lightvol> [x y z] [nx ny nz]\n"); return 2; }
    LightVolume vol;
    if (!vol.load(argv[1])) { std::printf("cannot read %s\n", argv[1]); return 1; }
    const Vec3 point = argc >= 5 ? Vec3{float(std::atof(argv[2])), float(std::atof(argv[3])), float(std::atof(argv[4]))}
                                 : vol.origin + Vec3{vol.nx * 0.5f, vol.ny * 0.5f, vol.nz * 0.5f} * vol.cell;  // default: the middle
    const Vec3 normal = argc >= 8 ? normalize({float(std::atof(argv[5])), float(std::atof(argv[6])), float(std::atof(argv[7]))}) : Vec3{0, 1, 0};
    std::printf("grid %d x %d x %d, cell %.2f m, origin (%.2f, %.2f, %.2f), %zu style layer(s)\n", vol.nx, vol.ny, vol.nz, vol.cell, vol.origin.x,
                vol.origin.y, vol.origin.z, vol.layers.size());
    std::printf("point (%.2f, %.2f, %.2f), normal (%.2f, %.2f, %.2f)\n", point.x, point.y, point.z, normal.x, normal.y, normal.z);
    for (size_t i = 0; i < vol.layers.size(); ++i) {
        const Probe p = vol.sample(vol.layers[i], point);
        if (!p.valid) { std::printf("style %d: no valid probe here\n", vol.styles[i]); continue; }
        const Vec3 light = p.ambient + p.directed * std::fmax(0.0f, dot(normal, p.dir));
        std::printf("style %d: ambient (%.3f %.3f %.3f) directed (%.3f %.3f %.3f) dir (%.2f %.2f %.2f) -> light (%.3f %.3f %.3f)\n", vol.styles[i],
                    p.ambient.x, p.ambient.y, p.ambient.z, p.directed.x, p.directed.y, p.directed.z, p.dir.x, p.dir.y, p.dir.z, light.x, light.y, light.z);
    }
    return 0;
}
