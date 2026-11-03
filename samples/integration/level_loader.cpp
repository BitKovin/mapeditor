// Minimal loader for a level exported with the editor's OBJ exporter (REQUIREMENTS §31, DESIGN "Exporters"), and the three things a game does with it:
//   1. static light   - the lightmap of a surface point              (vl coordinates + lm_NNNN.exr pages)
//   2. dynamic light  - the light at a moving entity                  (.lightvol)
//   3. entities       - classnames, origins, brush entities -> OBJ objects (.json)
// Standalone: C++17, no dependencies (it carries its own small zlib inflate for the EXR pages).
//   g++ -std=c++17 -O2 level_loader.cpp -o level_loader          cl /std:c++17 /EHsc /O2 level_loader.cpp
// Usage: level_loader <path/to/map.obj> [assetsRoot]      assetsRoot (the game folder the editor exported from) only checks that the textures exist.
// Reads <map>.obj, <map>.json, <map>.lightvol and <map>/lm_NNNN.exr; writes <map>_lm_0000.ppm (page 0, to look at).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---- math -------------------------------------------------------------------------------------------------------------------------------------
struct Vec3 { float x = 0, y = 0, z = 0; };
static Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float luminance(Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
static Vec3 normalize(Vec3 a) { const float l = std::sqrt(dot(a, a)); return l > 1e-12f ? a * (1.0f / l) : Vec3{0, 1, 0}; }

static std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---- 1. zlib inflate (after zlib's "puff"): EXR's ZIP blocks are zlib streams --------------------------------------------------------------------
namespace inflate {
struct Bits {
    const uint8_t* p; size_t n, pos = 0; uint32_t buf = 0; int cnt = 0;
    uint32_t get(int need) {
        uint32_t v = buf;
        while (cnt < need) { if (pos >= n) throw std::runtime_error("inflate: out of input"); v |= uint32_t(p[pos++]) << cnt; cnt += 8; }
        buf = v >> need; cnt -= need;
        return v & ((1u << need) - 1);
    }
};
struct Huff { uint16_t count[16] = {}, symbol[320] = {}; };
static void build(Huff& h, const uint8_t* len, int n) {
    std::fill(h.count, h.count + 16, uint16_t(0));
    for (int i = 0; i < n; ++i) h.count[len[i]]++;
    uint16_t offs[16] = {0, 0};
    for (int l = 1; l < 15; ++l) offs[l + 1] = uint16_t(offs[l] + h.count[l]);
    for (int i = 0; i < n; ++i) if (len[i]) h.symbol[offs[len[i]]++] = uint16_t(i);
}
static int decode(Bits& b, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= 15; ++l) {
        code |= int(b.get(1));
        const int count = h.count[l];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    throw std::runtime_error("inflate: bad code");
}
static void run(Bits& b, std::vector<uint8_t>& out) {
    static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (bool last = false; !last;) {
        last = b.get(1) != 0;
        const uint32_t type = b.get(2);
        if (type == 0) {  // stored
            b.buf = 0; b.cnt = 0;
            if (b.pos + 4 > b.n) throw std::runtime_error("inflate: truncated");
            const size_t len = b.p[b.pos] | b.p[b.pos + 1] << 8;
            b.pos += 4;
            if (b.pos + len > b.n) throw std::runtime_error("inflate: truncated");
            out.insert(out.end(), b.p + b.pos, b.p + b.pos + len);
            b.pos += len;
            continue;
        }
        if (type == 3) throw std::runtime_error("inflate: bad block");
        Huff lit, dist;
        uint8_t len[320];
        if (type == 1) {  // fixed codes
            int i = 0;
            for (; i < 144; ++i) len[i] = 8;
            for (; i < 256; ++i) len[i] = 9;
            for (; i < 280; ++i) len[i] = 7;
            for (; i < 288; ++i) len[i] = 8;
            build(lit, len, 288);
            for (i = 0; i < 30; ++i) len[i] = 5;
            build(dist, len, 30);
        } else {  // dynamic codes
            const int nlen = int(b.get(5)) + 257, ndist = int(b.get(5)) + 1, ncode = int(b.get(4)) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19] = {};
            for (int i = 0; i < ncode; ++i) cl[order[i]] = uint8_t(b.get(3));
            Huff lencode;
            build(lencode, cl, 19);
            int idx = 0;
            while (idx < nlen + ndist) {
                const int sym = decode(b, lencode);
                if (sym < 16) { len[idx++] = uint8_t(sym); continue; }
                uint8_t prev = 0; int rep;
                if (sym == 16) { if (idx == 0) throw std::runtime_error("inflate: bad lengths"); prev = len[idx - 1]; rep = 3 + int(b.get(2)); }
                else if (sym == 17) rep = 3 + int(b.get(3));
                else rep = 11 + int(b.get(7));
                if (idx + rep > nlen + ndist) throw std::runtime_error("inflate: bad lengths");
                while (rep--) len[idx++] = prev;
            }
            build(lit, len, nlen);
            build(dist, len + nlen, ndist);
        }
        for (;;) {
            int sym = decode(b, lit);
            if (sym < 256) { out.push_back(uint8_t(sym)); continue; }
            if (sym == 256) break;
            sym -= 257;
            if (sym >= 29) throw std::runtime_error("inflate: bad length");
            const size_t l = lbase[sym] + b.get(lext[sym]);
            const int ds = decode(b, dist);
            if (ds >= 30) throw std::runtime_error("inflate: bad distance");
            const size_t d = dbase[ds] + b.get(dext[ds]);
            if (d > out.size()) throw std::runtime_error("inflate: distance too far");
            for (size_t i = 0; i < l; ++i) out.push_back(out[out.size() - d]);
        }
    }
}
// a zlib stream: 2 byte header, deflate data, adler32 (not checked)
static std::vector<uint8_t> zlib(const uint8_t* data, size_t n) {
    if (n < 2) throw std::runtime_error("inflate: truncated");
    Bits b{data + 2, n - 2};
    std::vector<uint8_t> out;
    run(b, out);
    return out;
}
}  // namespace inflate

// ---- 2. Lightmap pages: OpenEXR, RGB half floats, ZIP (16 scanlines per block), linear -------------------------------------------------------------
struct Page {
    int w = 0, h = 0;
    std::vector<float> rgb;  // w * h * 3, row 0 = the file's first row = v 0
    Vec3 texel(int x, int y) const {
        x = std::clamp(x, 0, w - 1); y = std::clamp(y, 0, h - 1);
        const float* p = &rgb[(size_t(y) * w + x) * 3];
        return {p[0], p[1], p[2]};
    }
};
static uint32_t u32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
static float halfToFloat(uint16_t h) {
    const int e = (h >> 10) & 31; const float m = float(h & 1023), s = (h & 0x8000) ? -1.0f : 1.0f;
    if (e == 0) return s * std::ldexp(m, -24);
    if (e == 31) return s * (m ? NAN : INFINITY);
    return s * std::ldexp(1024.0f + m, e - 25);
}
static Page loadExr(const fs::path& file) {
    const std::vector<uint8_t> d = readFile(file);
    const size_t n = d.size();
    if (n < 8 || u32(d.data()) != 20000630u) throw std::runtime_error(file.string() + ": not an EXR");
    size_t p = 8;
    struct Chan { std::string name; int type; };  // type 1 = half, 2 = float
    std::vector<Chan> chans;
    int compression = -1, x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    auto cstr = [&]() { std::string s; while (p < n && d[p]) s.push_back(char(d[p++])); ++p; return s; };
    for (;;) {  // header attributes: name, type, size, value; an empty name ends them
        const std::string name = cstr();
        if (name.empty() || p >= n) break;
        cstr();  // type name
        const uint32_t size = u32(&d[p]); p += 4;
        const uint8_t* v = &d[p];
        if (name == "channels") {
            for (size_t q = 0; q < size && v[q];) {
                Chan c;
                while (q < size && v[q]) c.name.push_back(char(v[q++]));
                ++q; c.type = int(u32(v + q)); q += 16;  // pixel type, pLinear + reserved, xSampling, ySampling
                chans.push_back(c);
            }
        } else if (name == "compression") compression = v[0];
        else if (name == "dataWindow") { x0 = int(u32(v)); y0 = int(u32(v + 4)); x1 = int(u32(v + 8)); y1 = int(u32(v + 12)); }
        p += size;
    }
    if (compression != 0 && compression != 2 && compression != 3) throw std::runtime_error(file.string() + ": only none / ZIPS / ZIP EXR files are supported");
    Page pg; pg.w = x1 - x0 + 1; pg.h = y1 - y0 + 1;
    pg.rgb.assign(size_t(pg.w) * pg.h * 3, 0.0f);
    size_t pixelBytes = 0;
    for (const Chan& c : chans) pixelBytes += c.type == 1 ? 2 : 4;
    const int lines = compression == 3 ? 16 : 1, blocks = (pg.h + lines - 1) / lines;
    for (int k = 0; k < blocks; ++k) {  // the offset table (8 bytes per block) comes right after the header
        const size_t at = size_t(u32(&d[p + size_t(k) * 8])) | size_t(u32(&d[p + size_t(k) * 8 + 4])) << 32;
        const int ys = int(u32(&d[at])) - y0, len = int(u32(&d[at + 4])), ye = std::min(pg.h, ys + lines);
        const size_t expect = size_t(ye - ys) * pg.w * pixelBytes;
        std::vector<uint8_t> raw;
        if (size_t(len) == expect) raw.assign(&d[at + 8], &d[at + 8] + len);  // stored uncompressed
        else {
            std::vector<uint8_t> t = inflate::zlib(&d[at + 8], size_t(len));
            if (t.size() != expect) throw std::runtime_error(file.string() + ": corrupt block");
            // undo OpenEXR's ZIP pre-filter: running sum of byte deltas, then interleave the two halves
            for (size_t i = 1; i < t.size(); ++i) t[i] = uint8_t(int(t[i - 1]) + int(t[i]) - 128);
            raw.resize(t.size());
            const size_t half = (t.size() + 1) / 2;
            for (size_t i = 0; i < raw.size(); ++i) raw[i] = t[(i & 1) ? half + i / 2 : i / 2];
        }
        size_t q = 0;  // per scanline, per channel (alphabetical: B, G, R), a whole row of pixels
        for (int y = ys; y < ye; ++y)
            for (const Chan& c : chans) {
                const int slot = c.name == "R" ? 0 : c.name == "G" ? 1 : c.name == "B" ? 2 : -1;
                for (int x = 0; x < pg.w; ++x) {
                    float f;
                    if (c.type == 1) { f = halfToFloat(uint16_t(raw[q] | raw[q + 1] << 8)); q += 2; }
                    else { const uint32_t u = u32(&raw[q]); std::memcpy(&f, &u, 4); q += 4; }
                    if (slot >= 0) pg.rgb[(size_t(y) * pg.w + x) * 3 + slot] = f;
                }
            }
    }
    return pg;
}

// Cubic B-spline filtered lightmap fetch, what the editor and the engine do (bilinear has slope jumps at texel borders that show as creases in soft
// shadows). (u, v) are the OBJ's `vl` coordinates (0..1 over the page, v = 0 at the file's first row). Reads up to 2 texels around the point:
// the baker leaves 2 texels of padding around every chart for exactly this.
static Vec3 sampleLightmap(const Page& pg, float u, float v) {
    auto weights = [](float f, float w[4]) {
        w[0] = (1 - f) * (1 - f) * (1 - f) / 6;
        w[1] = (3 * f * f * f - 6 * f * f + 4) / 6;
        w[2] = (-3 * f * f * f + 3 * f * f + 3 * f + 1) / 6;
        w[3] = f * f * f / 6;
    };
    const float x = u * pg.w - 0.5f, y = v * pg.h - 0.5f;  // texel centres sit at (i + 0.5) / size
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    float wx[4], wy[4];
    weights(x - ix, wx); weights(y - iy, wy);
    Vec3 sum;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) sum = sum + pg.texel(ix - 1 + i, iy - 1 + j) * (wx[i] * wy[j]);
    return sum;
}

// ---- 3. The OBJ: geometry, groups = (texture, lightmap page), non standard `vl` lines ------------------------------------------------------------
struct Vertex { Vec3 pos, normal; float u = 0, v = 0;  /* texture */ float lu = 0, lv = 0;  /* lightmap */ };
struct Group {
    std::string texture;        // the editor's texture name, relative to the game's texture root ("dev/wall"); "none" = no texture
    int page = -1;              // lightmap page (lm_NNNN.exr) of every triangle in the group; -1 = no lightmap: light such a surface from the light volume
    std::vector<uint32_t> idx;  // triangles, counter-clockwise, indices into Level::vertices
};
struct Object { std::string name; std::vector<Group> groups; };  // "world", "model_1", "model_2", ...
struct Level {
    std::vector<Vertex> vertices;
    std::vector<Object> objects;
    std::vector<Page> pages;  // lm_0000.exr ... in file order; only the ones the OBJ refers to are meaningful (see the guide: style layers)
};

static void loadObj(const fs::path& file, Level& lv) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("cannot open " + file.string());
    std::vector<Vec3> pos, nrm; std::vector<float> tex, lm;  // flat u v pairs
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 2 || line[0] == '#') continue;
        const char* s = line.c_str();
        if (line.compare(0, 2, "v ") == 0) { Vec3 p; std::sscanf(s + 2, "%f %f %f", &p.x, &p.y, &p.z); pos.push_back(p); }
        else if (line.compare(0, 3, "vn ") == 0) { Vec3 p; std::sscanf(s + 3, "%f %f %f", &p.x, &p.y, &p.z); nrm.push_back(p); }
        else if (line.compare(0, 3, "vt ") == 0) { float a, b; std::sscanf(s + 3, "%f %f", &a, &b); tex.push_back(a); tex.push_back(b); }
        else if (line.compare(0, 3, "vl ") == 0) { float a, b; std::sscanf(s + 3, "%f %f", &a, &b); lm.push_back(a); lm.push_back(b); }
        else if (line.compare(0, 2, "o ") == 0) lv.objects.push_back({line.substr(2), {}});
        else if (line.compare(0, 2, "g ") == 0) {
            if (lv.objects.empty()) lv.objects.push_back({"world", {}});
            // "<texture>@lm_0003": the page of the group's lightmap; no "@": no lightmap
            Group g; const std::string name = line.substr(2);
            const size_t at = name.find('@');
            g.texture = name.substr(0, at);
            if (at != std::string::npos) g.page = std::atoi(name.c_str() + at + 4);  // skip "@lm_"
            lv.objects.back().groups.push_back(g);
        } else if (line.compare(0, 2, "f ") == 0) {
            if (lv.objects.empty() || lv.objects.back().groups.empty()) throw std::runtime_error("face before any object / group");
            // The exporter writes v, vt, vn and vl in lock step, so a corner "a/a/a" is ONE vertex index. A general OBJ reader would have to
            // combine v / vt / vn per corner; this one just refuses anything else.
            uint32_t c[3]; int k = 0;
            for (const char* p = s + 2; *p && k < 3;) {
                char* end; const long v = std::strtol(p, &end, 10);
                long t = v, n = v;
                if (*end == '/') { t = std::strtol(end + 1, &end, 10); if (*end == '/') n = std::strtol(end + 1, &end, 10); }
                if (t != v || n != v) throw std::runtime_error("unexpected face corner (v/vt/vn differ)");
                c[k++] = uint32_t(v - 1);
                p = end; while (*p == ' ') ++p;
            }
            if (k == 3) for (uint32_t i : c) lv.objects.back().groups.back().idx.push_back(i);
        }
    }
    if (tex.size() != pos.size() * 2 || lm.size() != pos.size() * 2 || nrm.size() != pos.size()) throw std::runtime_error("v / vt / vn / vl counts differ");
    lv.vertices.resize(pos.size());
    for (size_t i = 0; i < pos.size(); ++i)
        lv.vertices[i] = {pos[i], nrm[i], tex[i * 2], tex[i * 2 + 1], lm[i * 2], lm[i * 2 + 1]};
}

// ---- 4. The entities: a JSON array of flat objects, every value a string ------------------------------------------------------------------------
using Entity = std::map<std::string, std::string>;
static std::vector<Entity> loadEntities(const fs::path& file) {  // parser for exactly this shape; use any JSON library in a real game
    const std::vector<uint8_t> raw = readFile(file);
    const std::string t(raw.begin(), raw.end());
    std::vector<Entity> out;
    size_t i = 0;
    auto str = [&]() {  // at the opening quote
        std::string s;
        for (++i; i < t.size() && t[i] != '"'; ++i) {
            if (t[i] != '\\') { s.push_back(t[i]); continue; }
            const char e = t[++i];
            s.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e);  // good enough: keys / values are plain text (\uXXXX is not handled)
        }
        ++i;
        return s;
    };
    while (i < t.size()) {
        if (t[i] == '{') out.emplace_back();
        else if (t[i] == '"') {
            const std::string k = str();
            while (i < t.size() && t[i] != '"') ++i;
            out.back()[k] = str();
            continue;
        }
        ++i;
    }
    return out;
}
static Vec3 parseVec(const std::string& s) { Vec3 v; std::sscanf(s.c_str(), "%f %f %f", &v.x, &v.y, &v.z); return v; }

// ---- 5. The light volume: lights for things that move (they own no lightmap) -----------------------------------------------------------------------
struct Probe { Vec3 ambient, directed, dir{0, 1, 0}; bool valid = false; };
struct LightVolume {
    Vec3 origin; float cell = 1; int nx = 0, ny = 0, nz = 0;
    std::vector<int> styles;                 // light style id of each layer (0 = always on)
    std::vector<std::vector<Probe>> layers;  // one grid per style, x fastest, then y, then z

    bool load(const fs::path& file) {
        std::error_code ec;
        if (!fs::exists(file, ec)) return false;  // no bake: no file
        const std::vector<uint8_t> b = readFile(file);
        if (b.size() < 40 || std::memcmp(b.data(), "LVOL", 4) != 0 || u32(&b[4]) != 1) return false;
        float hdr[4]; int32_t dims[4];
        std::memcpy(hdr, &b[8], 16); std::memcpy(dims, &b[24], 16);
        origin = {hdr[0], hdr[1], hdr[2]}; cell = hdr[3]; nx = dims[0]; ny = dims[1]; nz = dims[2];
        const size_t count = size_t(nx) * ny * nz;
        size_t at = 40;
        for (int s = 0; s < dims[3]; ++s) {
            if (at + 4 + count * 9 > b.size()) return false;
            int32_t style; std::memcpy(&style, &b[at], 4); at += 4;
            std::vector<Probe> grid(count);
            for (Probe& p : grid) {
                const uint8_t* q = &b[at]; at += 9;
                // bytes are value / 2 * 255: the range is 0..2 and the colours are linear, like the lightmaps
                p.ambient = {q[0] / 255.0f * 2, q[1] / 255.0f * 2, q[2] / 255.0f * 2};
                p.directed = {q[3] / 255.0f * 2, q[4] / 255.0f * 2, q[5] / 255.0f * 2};
                const float lat = q[6] / 255.0f * 3.14159265f, lng = q[7] / 255.0f * 6.28318531f - 3.14159265f;
                p.dir = {std::sin(lat) * std::cos(lng), std::cos(lat), std::sin(lat) * std::sin(lng)};  // towards the light, Y up
                p.valid = (q[8] & 1) != 0;  // 0: the probe lies inside a wall, its colours are filled from neighbours: do not trust it alone
            }
            styles.push_back(style); layers.push_back(std::move(grid));
        }
        return true;
    }
    // Trilinear over the 8 probes around p; invalid ones (inside walls) are skipped; the direction is brightness weighted so dark probes do not steer it.
    Probe sample(const std::vector<Probe>& grid, Vec3 p) const {
        const Vec3 g = {(p.x - origin.x) / cell, (p.y - origin.y) / cell, (p.z - origin.z) / cell};
        const int x0 = int(std::floor(g.x)), y0 = int(std::floor(g.y)), z0 = int(std::floor(g.z));
        const float fx = g.x - x0, fy = g.y - y0, fz = g.z - z0;
        Probe out; Vec3 dir; float wsum = 0;
        for (int c = 0; c < 8; ++c) {
            const int dx = c & 1, dy = (c >> 1) & 1, dz = c >> 2;
            const Probe& q = grid[(size_t(std::clamp(z0 + dz, 0, nz - 1)) * ny + std::clamp(y0 + dy, 0, ny - 1)) * nx + std::clamp(x0 + dx, 0, nx - 1)];
            const float w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * (dz ? fz : 1 - fz);
            if (!q.valid || w <= 0) continue;
            out.ambient = out.ambient + q.ambient * w;
            out.directed = out.directed + q.directed * w;
            dir = dir + q.dir * (w * (luminance(q.directed) + 1e-6f));
            wsum += w;
        }
        if (wsum <= 0) return out;
        out.ambient = out.ambient * (1 / wsum); out.directed = out.directed * (1 / wsum);
        out.dir = normalize(dir); out.valid = true;
        return out;
    }
    // The light that reaches a surface with normal n at p: sum over the style layers of (ambient + directed * max(0, n . dir)) * styleValue(style, time).
    Vec3 lightAt(Vec3 p, Vec3 n, const std::map<int, float>& styleValue) const {
        Vec3 sum;
        for (size_t i = 0; i < layers.size(); ++i) {
            const auto v = styleValue.find(styles[i]);
            const float k = styles[i] == 0 ? 1.0f : (v == styleValue.end() ? 1.0f : v->second);
            const Probe pr = sample(layers[i], p);
            if (pr.valid) sum = sum + (pr.ambient + pr.directed * std::fmax(0.0f, dot(n, pr.dir))) * k;
        }
        return sum;
    }
};

// A light style is a looping pattern, one letter per 0.1 s: 'a' = 0, 'm' = 1, 'z' = 25/12. Style 0 is always 1. The patterns are the engine's table
// (game_profile.json lighting.lightStyles can override them); style 6 here is the editor's default for 6.
static float styleValue(const std::string& pattern, double seconds) {
    if (pattern.empty()) return 1.0f;
    const char c = pattern[size_t(seconds * 10) % pattern.size()];
    return (c - 'a') / 12.0f;
}

// ---- demo -------------------------------------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: level_loader <map.obj> [assetsRoot]\n"); return 2; }
    try {
        const fs::path objFile = argv[1], stem = objFile.parent_path() / objFile.stem();
        Level lv;
        loadObj(objFile, lv);
        const std::vector<Entity> ents = loadEntities(stem.string() + ".json");
        LightVolume vol;
        const bool hasVol = vol.load(stem.string() + ".lightvol");
        // The pages live in a folder named like the map: <stem>/lm_0000.exr, lm_0001.exr ... Load the ones the OBJ refers to (the highest page + 1 of
        // all groups are base style pages; further pages are other light style layers, see the guide).
        int maxPage = -1;
        for (const Object& o : lv.objects) for (const Group& g : o.groups) maxPage = std::max(maxPage, g.page);
        for (int i = 0; i <= maxPage; ++i) {
            char name[32]; std::snprintf(name, sizeof name, "lm_%04d.exr", i);
            lv.pages.push_back(loadExr(stem / name));
        }

        // -- what was loaded
        size_t tris = 0;
        for (const Object& o : lv.objects) { size_t t = 0; for (const Group& g : o.groups) t += g.idx.size() / 3; tris += t; std::printf("object %-8s %zu groups, %zu triangles\n", o.name.c_str(), o.groups.size(), t); }
        std::printf("%zu vertices, %zu triangles, %zu lightmap page(s) %dx%d, %zu entities\n", lv.vertices.size(), tris, lv.pages.size(), lv.pages.empty() ? 0 : lv.pages[0].w,
                    lv.pages.empty() ? 0 : lv.pages[0].h, ents.size());
        if (hasVol) std::printf("light volume %d x %d x %d, cell %.2f m, origin (%.2f %.2f %.2f), %zu style layer(s)\n", vol.nx, vol.ny, vol.nz, vol.cell, vol.origin.x, vol.origin.y, vol.origin.z, vol.layers.size());

        // -- textures: the editor's names are relative to the game's texture root ("textures" in game_profile.json), the extension is the first of
        //    "textureExtensions" that exists
        if (argc >= 3) {
            std::map<std::string, bool> seen;
            for (const Object& o : lv.objects) for (const Group& g : o.groups) {
                if (seen.count(g.texture)) continue;
                bool found = false;
                for (const char* ext : {".tga", ".jpg", ".png"}) found = found || fs::exists(fs::path(argv[2]) / "textures" / (g.texture + ext));
                seen[g.texture] = found;
            }
            for (const auto& [name, found] : seen) std::printf("texture %-40s %s\n", name.c_str(), found ? "found" : "MISSING (shader-only names like sky have no file)");
        }

        // -- entities. Point entities: origin / angles in the same space as the geometry (metres, Y up). A brush entity says "model": "*N" and its
        //    geometry is the object "model_N", in world coordinates; its "origin" is the pivot it moves / turns about.
        Vec3 playerStart; float exposure = 1, brightness = 0;
        for (const Entity& e : ents) {
            const std::string cls = e.count("classname") ? e.at("classname") : "";
            const Vec3 origin = e.count("origin") ? parseVec(e.at("origin")) : Vec3{};
            if (cls == "worldspawn") {  // display colour correction: shown = albedo * light * exposure + brightness
                if (e.count("_cc_exposure")) exposure = float(std::atof(e.at("_cc_exposure").c_str()));
                if (e.count("_cc_brightness")) brightness = float(std::atof(e.at("_cc_brightness").c_str()));
            }
            if (cls == "info_player_start") playerStart = origin;
            if (e.count("model") && e.at("model").size() > 1 && e.at("model")[0] == '*')
                std::printf("entity %-14s model %s -> object model_%s, pivot (%.2f %.2f %.2f)\n", cls.c_str(), e.at("model").c_str(), e.at("model").c_str() + 1, origin.x, origin.y, origin.z);
        }
        std::printf("player start (%.2f %.2f %.2f), exposure %.2f, brightness %.2f\n", playerStart.x, playerStart.y, playerStart.z, exposure, brightness);

        // -- static light: the triangle of the world closest to the player; its lightmap value at the middle of the triangle. A real renderer
        //    interpolates `vl` across the triangle per pixel and calls sampleLightmap there.
        float best = 1e30f; const Group* bestGroup = nullptr; size_t bestTri = 0;
        for (const Group& g : lv.objects[0].groups) {
            if (g.page < 0) continue;
            for (size_t t = 0; t + 2 < g.idx.size(); t += 3) {
                const Vec3 c = (lv.vertices[g.idx[t]].pos + lv.vertices[g.idx[t + 1]].pos + lv.vertices[g.idx[t + 2]].pos) * (1.0f / 3);
                const float d = dot(c - playerStart, c - playerStart);
                if (d < best) { best = d; bestGroup = &g; bestTri = t; }
            }
        }
        if (bestGroup) {
            const Vertex &a = lv.vertices[bestGroup->idx[bestTri]], &b = lv.vertices[bestGroup->idx[bestTri + 1]], &c = lv.vertices[bestGroup->idx[bestTri + 2]];
            const float lu = (a.lu + b.lu + c.lu) / 3, lvv = (a.lv + b.lv + c.lv) / 3;
            const Vec3 light = sampleLightmap(lv.pages[size_t(bestGroup->page)], lu, lvv);
            const Vec3 albedo = {1, 1, 1};  // the texture's pixel at (u, v), as stored: NO sRGB <-> linear conversion before multiplying
            const Vec3 shown = {albedo.x * light.x * exposure + brightness, albedo.y * light.y * exposure + brightness, albedo.z * light.z * exposure + brightness};
            std::printf("nearest world surface: texture %s, page %d, vl (%.3f %.3f) -> light (%.3f %.3f %.3f) -> shown (%.3f %.3f %.3f)\n", bestGroup->texture.c_str(),
                        bestGroup->page, lu, lvv, light.x, light.y, light.z, shown.x, shown.y, shown.z);
        }

        // -- dynamic light: the light volume at every brush entity (use its bounds' centre, or any point of the thing you light) and the player
        if (hasVol) {
            const std::map<int, float> styles = {{6, styleValue("nmonqnmomnmomomno", 1.0)}};  // style 6 at t = 1 s
            auto report = [&](const char* what, Vec3 p) {
                const Vec3 l = vol.lightAt(p, Vec3{0, 1, 0}, styles);  // a normal of {0,1,0}: light falling on top of the thing; use the thing's own normal per pixel
                std::printf("light volume at %-12s (%.2f %.2f %.2f): (%.3f %.3f %.3f)\n", what, p.x, p.y, p.z, l.x, l.y, l.z);
            };
            report("player start", playerStart);
            for (const Entity& e : ents)
                if (e.count("model") && e.count("origin")) report(e.at("classname").c_str(), parseVec(e.at("origin")));
        }

        // -- surfaces without a lightmap (group name without @lm_): doors and other brush entities, models. They are lit by the light volume at their
        //    position, per vertex or per pixel, with the surface normal.
        size_t unlit = 0;
        for (const Object& o : lv.objects) for (const Group& g : o.groups) if (g.page < 0) unlit += g.idx.size() / 3;
        std::printf("%zu triangle(s) have no lightmap (light them from the light volume)\n", unlit);

        // -- write page 0 so it can be looked at (linear value clamped to 0..1 and gamma 2.2: only for viewing it)
        if (!lv.pages.empty()) {
            const Page& pg = lv.pages[0];
            std::ofstream ppm(stem.string() + "_lm_0000.ppm", std::ios::binary);
            ppm << "P6\n" << pg.w << ' ' << pg.h << "\n255\n";
            for (float f : pg.rgb) ppm.put(char(std::lround(std::pow(std::clamp(f, 0.0f, 1.0f), 1 / 2.2f) * 255)));
            std::printf("wrote %s_lm_0000.ppm\n", stem.string().c_str());
        }
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        return 1;
    }
    return 0;
}
