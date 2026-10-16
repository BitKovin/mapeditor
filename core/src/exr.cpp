#include "core/exr.h"
#include "core/live_bake.h"  // floatToHalf / halfToFloat
#include <miniz.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace core {

namespace {

constexpr int kZipLines = 16;   // ZIP_COMPRESSION: 16 scanlines per block
constexpr uint8_t kNone = 0, kZips = 2, kZip = 3;

void put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void put64(std::vector<uint8_t>& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void putF(std::vector<uint8_t>& b, float f) { uint32_t u; std::memcpy(&u, &f, 4); put32(b, u); }
void putStr(std::vector<uint8_t>& b, const char* s) { while (*s) b.push_back(static_cast<uint8_t>(*s++)); b.push_back(0); }
void attr(std::vector<uint8_t>& b, const char* name, const char* type, const std::vector<uint8_t>& value) {
    putStr(b, name);
    putStr(b, type);
    put32(b, static_cast<uint32_t>(value.size()));
    b.insert(b.end(), value.begin(), value.end());
}

// OpenEXR's ZIP pre-filter: split even / odd bytes, then store byte deltas (+128).
void zipFilter(const std::vector<uint8_t>& raw, std::vector<uint8_t>& out) {
    out.resize(raw.size());
    const size_t half = (raw.size() + 1) / 2;
    for (size_t i = 0; i < raw.size(); ++i) out[(i & 1) ? half + i / 2 : i / 2] = raw[i];
    int p = out.empty() ? 0 : out[0];
    for (size_t i = 1; i < out.size(); ++i) {
        const int d = int(out[i]) - p + (128 + 256);
        p = out[i];
        out[i] = static_cast<uint8_t>(d);
    }
}
void zipUnfilter(std::vector<uint8_t>& t, std::vector<uint8_t>& raw) {
    for (size_t i = 1; i < t.size(); ++i) t[i] = static_cast<uint8_t>(int(t[i - 1]) + int(t[i]) - 128);
    raw.resize(t.size());
    const size_t half = (t.size() + 1) / 2;
    for (size_t i = 0; i < raw.size(); ++i) raw[i] = t[(i & 1) ? half + i / 2 : i / 2];
}

uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32; }

}  // namespace

std::vector<uint8_t> encodeExr(int width, int height, const float* rgb) {
    std::vector<uint8_t> b;
    put32(b, 20000630u);  // magic 76 2f 31 01
    put32(b, 2u);         // version 2, single part scanline
    // channels, alphabetical: B G R, all HALF
    std::vector<uint8_t> ch;
    for (const char* n : {"B", "G", "R"}) {
        putStr(ch, n);
        put32(ch, 1);                       // HALF
        ch.push_back(0); ch.push_back(0); ch.push_back(0); ch.push_back(0);  // pLinear + reserved
        put32(ch, 1); put32(ch, 1);         // sampling
    }
    ch.push_back(0);
    attr(b, "channels", "chlist", ch);
    attr(b, "compression", "compression", {kZip});
    std::vector<uint8_t> box;
    put32(box, 0); put32(box, 0); put32(box, static_cast<uint32_t>(width - 1)); put32(box, static_cast<uint32_t>(height - 1));
    attr(b, "dataWindow", "box2i", box);
    attr(b, "displayWindow", "box2i", box);
    attr(b, "lineOrder", "lineOrder", {0});
    std::vector<uint8_t> one; putF(one, 1.0f);
    attr(b, "pixelAspectRatio", "float", one);
    std::vector<uint8_t> centre; putF(centre, 0); putF(centre, 0);
    attr(b, "screenWindowCenter", "v2f", centre);
    attr(b, "screenWindowWidth", "float", one);
    b.push_back(0);  // end of header

    const int blocks = (height + kZipLines - 1) / kZipLines;
    const size_t tableAt = b.size();
    b.resize(b.size() + static_cast<size_t>(blocks) * 8);
    std::vector<uint8_t> raw, filtered, packed;
    for (int k = 0; k < blocks; ++k) {
        const int y0 = k * kZipLines, y1 = std::min(height, y0 + kZipLines);
        raw.clear();
        for (int y = y0; y < y1; ++y)
            for (int c = 2; c >= 0; --c)  // B, G, R
                for (int x = 0; x < width; ++x) {
                    const uint16_t h = floatToHalf(rgb[(static_cast<size_t>(y) * width + x) * 3 + c]);
                    raw.push_back(static_cast<uint8_t>(h)); raw.push_back(static_cast<uint8_t>(h >> 8));
                }
        zipFilter(raw, filtered);
        mz_ulong len = mz_compressBound(static_cast<mz_ulong>(filtered.size()));
        packed.resize(len);
        const bool ok = mz_compress2(packed.data(), &len, filtered.data(), static_cast<mz_ulong>(filtered.size()), MZ_DEFAULT_COMPRESSION) == MZ_OK;
        const std::vector<uint8_t>& data = ok && len < raw.size() ? packed : raw;  // a block that does not shrink is stored raw (readers accept it)
        const size_t dataLen = ok && len < raw.size() ? len : raw.size();
        const uint64_t at = b.size();
        for (int i = 0; i < 8; ++i) b[tableAt + static_cast<size_t>(k) * 8 + i] = static_cast<uint8_t>(at >> (8 * i));
        put32(b, static_cast<uint32_t>(y0));
        put32(b, static_cast<uint32_t>(dataLen));
        b.insert(b.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(dataLen));
    }
    return b;
}

bool decodeExr(const uint8_t* d, size_t n, ExrImage& out, std::string* error) {
    auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
    if (n < 8 || get32(d) != 20000630u) return fail("not an EXR file");
    if ((get32(d + 4) & 0xff) != 2 || (get32(d + 4) & 0x1a00) != 0) return fail("only single part scanline EXR files are supported");
    size_t p = 8;
    auto str = [&](std::string& s) -> bool {
        s.clear();
        while (p < n && d[p]) s.push_back(static_cast<char>(d[p++]));
        if (p >= n) return false;
        ++p;
        return true;
    };
    struct Channel { std::string name; int type = 0; };
    std::vector<Channel> chans;
    int compression = -1;
    int32_t xMin = 0, yMin = 0, xMax = -1, yMax = -1;
    for (;;) {
        std::string name, type;
        if (!str(name)) return fail("truncated header");
        if (name.empty()) break;
        if (!str(type) || p + 4 > n) return fail("truncated header");
        const uint32_t size = get32(d + p);
        p += 4;
        if (p + size > n) return fail("truncated header");
        const uint8_t* v = d + p;
        if (name == "channels") {
            size_t q = 0;
            while (q < size && v[q]) {
                Channel c;
                while (q < size && v[q]) c.name.push_back(static_cast<char>(v[q++]));
                ++q;
                if (q + 16 > size) return fail("bad channel list");
                c.type = static_cast<int>(get32(v + q));
                if (get32(v + q + 8) != 1 || get32(v + q + 12) != 1) return fail("subsampled channels are not supported");
                q += 16;
                chans.push_back(c);
            }
        } else if (name == "compression" && size >= 1) {
            compression = v[0];
        } else if (name == "dataWindow" && size >= 16) {
            xMin = static_cast<int32_t>(get32(v)); yMin = static_cast<int32_t>(get32(v + 4));
            xMax = static_cast<int32_t>(get32(v + 8)); yMax = static_cast<int32_t>(get32(v + 12));
        }
        p += size;
    }
    if (compression != kNone && compression != kZips && compression != kZip) return fail("unsupported compression " + std::to_string(compression));
    const int w = xMax - xMin + 1, h = yMax - yMin + 1;
    if (w <= 0 || h <= 0 || w > 65536 || h > 65536) return fail("bad data window");
    size_t pixelBytes = 0;
    for (const auto& c : chans) {
        if (c.type != 1 && c.type != 2) return fail("channel " + c.name + ": only half and float channels are supported");
        pixelBytes += c.type == 1 ? 2 : 4;
    }
    const int lines = compression == kZip ? kZipLines : 1;
    const int blocks = (h + lines - 1) / lines;
    if (p + static_cast<size_t>(blocks) * 8 > n) return fail("truncated offset table");
    out.width = w; out.height = h;
    out.rgb.assign(static_cast<size_t>(w) * h * 3, 0.0f);
    std::vector<uint8_t> buf, raw;
    for (int k = 0; k < blocks; ++k) {
        const uint64_t at = get64(d + p + static_cast<size_t>(k) * 8);
        if (at + 8 > n) return fail("bad block offset");
        const int y0 = static_cast<int32_t>(get32(d + at)) - yMin;
        const uint32_t len = get32(d + at + 4);
        if (at + 8 + len > n || y0 < 0 || y0 >= h) return fail("bad block");
        const int y1 = std::min(h, y0 + lines);
        const size_t expect = static_cast<size_t>(y1 - y0) * w * pixelBytes;
        if (len == expect) {
            raw.assign(d + at + 8, d + at + 8 + len);
        } else {
            buf.resize(expect);
            mz_ulong got = static_cast<mz_ulong>(expect);
            if (mz_uncompress(buf.data(), &got, d + at + 8, len) != MZ_OK || got != expect) return fail("corrupt block");
            zipUnfilter(buf, raw);
        }
        size_t q = 0;
        for (int y = y0; y < y1; ++y)
            for (const auto& c : chans) {
                const int slot = c.name == "R" ? 0 : c.name == "G" ? 1 : c.name == "B" ? 2 : -1;
                for (int x = 0; x < w; ++x) {
                    float v;
                    if (c.type == 1) { v = halfToFloat(static_cast<uint16_t>(raw[q] | raw[q + 1] << 8)); q += 2; }
                    else { uint32_t u = get32(&raw[q]); std::memcpy(&v, &u, 4); q += 4; }
                    if (slot >= 0) out.rgb[(static_cast<size_t>(y) * w + x) * 3 + slot] = v;
                }
            }
    }
    return true;
}

bool writeExr(const std::string& path, int width, int height, const float* rgb) {
    const std::vector<uint8_t> b = encodeExr(width, height, rgb);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

bool readExr(const std::string& path, ExrImage& out, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (error) *error = "cannot open " + path; return false; }
    const std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return decodeExr(b.data(), b.size(), out, error);
}

}  // namespace core
