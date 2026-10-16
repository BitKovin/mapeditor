#include "core/albedo.h"
#include <nlohmann/json.hpp>
#include "core/textures.h"
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

namespace core {

// ---- sRGB ------------------------------------------------------------------------------------------------------------------
namespace {
const std::array<float, 256>& srgbTable() {
    static const std::array<float, 256> t = [] {
        std::array<float, 256> a{};
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            a[static_cast<size_t>(i)] = static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
        }
        return a;
    }();
    return t;
}
}  // namespace

float srgbToLinear(uint8_t v) { return srgbTable()[v]; }
uint8_t linearToSrgb(float v) {
    v = std::clamp(v, 0.f, 1.f);
    const double c = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(static_cast<double>(v), 1.0 / 2.4) - 0.055;
    return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(c * 255.0)), 0, 255));
}

// ---- inflate (RFC 1951) for PNG ---------------------------------------------------------------------------------------------
namespace {
struct BitReader {
    const uint8_t* p;
    size_t n, pos = 0;
    uint32_t bits = 0;
    int count = 0;
    bool bad = false;
    uint32_t get(int k) {
        while (count < k) {
            if (pos >= n) { bad = true; return 0; }
            bits |= static_cast<uint32_t>(p[pos++]) << count;
            count += 8;
        }
        const uint32_t v = bits & ((1u << k) - 1u);
        bits >>= k;
        count -= k;
        return v;
    }
};
struct Huffman {
    uint16_t counts[16] = {};
    uint16_t symbols[320] = {};
    bool build(const uint8_t* lengths, int n) {
        std::memset(counts, 0, sizeof(counts));
        for (int i = 0; i < n; ++i) ++counts[lengths[i]];
        counts[0] = 0;
        int left = 1;
        for (int len = 1; len < 16; ++len) { left <<= 1; left -= counts[len]; if (left < 0) return false; }
        uint16_t offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<uint16_t>(offs[len] + counts[len]);
        for (int i = 0; i < n; ++i) if (lengths[i]) symbols[offs[lengths[i]]++] = static_cast<uint16_t>(i);
        return true;
    }
    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= static_cast<int>(br.get(1));
            if (br.bad) return -1;
            const int count = counts[len];
            if (code - count < first) return symbols[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

bool inflateData(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t expected) {
    static const uint16_t lbase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint16_t lext[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint16_t dext[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    if (n < 2) return false;
    BitReader br{src + 2, n - 2};  // skip the zlib header
    out.reserve(expected);
    int last = 0;
    do {
        last = static_cast<int>(br.get(1));
        const uint32_t type = br.get(2);
        if (br.bad) return false;
        if (type == 0) {
            br.bits = 0; br.count = 0;
            if (br.pos + 4 > br.n) return false;
            const size_t len = br.p[br.pos] | (static_cast<size_t>(br.p[br.pos + 1]) << 8);
            br.pos += 4;
            if (br.pos + len > br.n) return false;
            out.insert(out.end(), br.p + br.pos, br.p + br.pos + len);
            br.pos += len;
        } else if (type == 1 || type == 2) {
            Huffman lit, dist;
            if (type == 1) {
                uint8_t l[288];
                for (int i = 0; i < 144; ++i) l[i] = 8;
                for (int i = 144; i < 256; ++i) l[i] = 9;
                for (int i = 256; i < 280; ++i) l[i] = 7;
                for (int i = 280; i < 288; ++i) l[i] = 8;
                lit.build(l, 288);
                uint8_t d[30];
                for (auto& x : d) x = 5;
                dist.build(d, 30);
            } else {
                const int nlen = static_cast<int>(br.get(5)) + 257, ndist = static_cast<int>(br.get(5)) + 1, ncode = static_cast<int>(br.get(4)) + 4;
                if (br.bad || nlen > 286 || ndist > 30) return false;
                static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                uint8_t lengths[320] = {};
                for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<uint8_t>(br.get(3));
                Huffman code;
                if (!code.build(lengths, 19)) return false;
                int index = 0;
                uint8_t all[320] = {};
                while (index < nlen + ndist) {
                    int sym = code.decode(br);
                    if (sym < 0) return false;
                    if (sym < 16) { all[index++] = static_cast<uint8_t>(sym); continue; }
                    uint8_t prev = 0;
                    int rep;
                    if (sym == 16) { if (index == 0) return false; prev = all[index - 1]; rep = 3 + static_cast<int>(br.get(2)); }
                    else if (sym == 17) rep = 3 + static_cast<int>(br.get(3));
                    else rep = 11 + static_cast<int>(br.get(7));
                    if (index + rep > nlen + ndist) return false;
                    while (rep--) all[index++] = prev;
                }
                if (!lit.build(all, nlen)) return false;
                if (!dist.build(all + nlen, ndist)) return false;
            }
            for (;;) {
                const int sym = lit.decode(br);
                if (sym < 0 || br.bad) return false;
                if (sym < 256) { out.push_back(static_cast<uint8_t>(sym)); continue; }
                if (sym == 256) break;
                const int si = sym - 257;
                if (si >= 29) return false;
                const int len = lbase[si] + static_cast<int>(br.get(lext[si]));
                const int ds = dist.decode(br);
                if (ds < 0 || ds >= 30) return false;
                const size_t d = dbase[ds] + br.get(dext[ds]);
                if (br.bad || d > out.size()) return false;
                for (int i = 0; i < len; ++i) out.push_back(out[out.size() - d]);
            }
        } else {
            return false;
        }
        if (out.size() > expected + 1024) return false;
    } while (!last);
    return true;
}

uint32_t be32(const unsigned char* p) { return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3]; }
}  // namespace

// ---- PNG ---------------------------------------------------------------------------------------------------------------------
std::optional<DecodedImage> decodePng(const std::vector<unsigned char>& b) {
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (b.size() < 33 || std::memcmp(b.data(), sig, 8) != 0) return std::nullopt;
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = 0, interlace = 0;
    std::vector<uint8_t> idat, palette, trns;
    size_t pos = 8;
    bool header = false;
    while (pos + 12 <= b.size()) {
        const uint32_t len = be32(&b[pos]);
        const std::string type(reinterpret_cast<const char*>(&b[pos + 4]), 4);
        if (pos + 12 + static_cast<size_t>(len) > b.size()) break;
        const unsigned char* d = &b[pos + 8];
        if (type == "IHDR" && len >= 13) {
            w = be32(d); h = be32(d + 4); depth = d[8]; ctype = d[9]; interlace = d[12]; header = true;
        } else if (type == "PLTE") palette.assign(d, d + len);
        else if (type == "tRNS") trns.assign(d, d + len);
        else if (type == "IDAT") idat.insert(idat.end(), d, d + len);
        else if (type == "IEND") break;
        pos += 12 + len;
    }
    if (!header || w == 0 || h == 0 || w > 16384 || h > 16384 || interlace != 0) return std::nullopt;
    int channels = 0;
    switch (ctype) { case 0: channels = 1; break; case 2: channels = 3; break; case 3: channels = 1; break; case 4: channels = 2; break; case 6: channels = 4; break; default: return std::nullopt; }
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return std::nullopt;
    if (ctype == 3 && palette.empty()) return std::nullopt;
    const size_t bitsPerPixel = static_cast<size_t>(channels) * static_cast<size_t>(depth);
    const size_t stride = (static_cast<size_t>(w) * bitsPerPixel + 7) / 8, bpp = std::max<size_t>(1, bitsPerPixel / 8);
    std::vector<uint8_t> raw;
    if (!inflateData(idat.data(), idat.size(), raw, (stride + 1) * h) || raw.size() < (stride + 1) * h) return std::nullopt;
    // unfilter in place
    std::vector<uint8_t> img(stride * h);
    for (size_t y = 0; y < h; ++y) {
        const uint8_t ft = raw[y * (stride + 1)];
        const uint8_t* in = &raw[y * (stride + 1) + 1];
        uint8_t* cur = &img[y * stride];
        const uint8_t* up = y ? &img[(y - 1) * stride] : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= bpp ? cur[x - bpp] : 0, bb = up ? up[x] : 0, c = (up && x >= bpp) ? up[x - bpp] : 0;
            int v = in[x];
            switch (ft) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += bb; break;
                case 3: v += (a + bb) / 2; break;
                case 4: { const int p = a + bb - c, pa = std::abs(p - a), pb = std::abs(p - bb), pc = std::abs(p - c); v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? bb : c); break; }
                default: return std::nullopt;
            }
            cur[x] = static_cast<uint8_t>(v);
        }
    }
    DecodedImage out;
    out.width = static_cast<int>(w);
    out.height = static_cast<int>(h);
    out.rgba.assign(static_cast<size_t>(w) * h * 4, 255);
    auto sampleAt = [&](const uint8_t* row, size_t x, int ch) -> int {  // returns an 8 bit value (16 bit: high byte; <8 bit: raw index scaled by the caller)
        if (depth == 8) return row[x * static_cast<size_t>(channels) + static_cast<size_t>(ch)];
        if (depth == 16) return row[(x * static_cast<size_t>(channels) + static_cast<size_t>(ch)) * 2];
        const size_t bit = x * static_cast<size_t>(depth);
        return (row[bit / 8] >> (8 - depth - static_cast<int>(bit % 8))) & ((1 << depth) - 1);
    };
    for (size_t y = 0; y < h; ++y) {
        const uint8_t* row = &img[y * stride];
        for (size_t x = 0; x < w; ++x) {
            uint8_t* o = &out.rgba[(y * w + x) * 4];
            if (ctype == 3) {
                const size_t i = static_cast<size_t>(sampleAt(row, x, 0));
                if (i * 3 + 2 < palette.size()) { o[0] = palette[i * 3]; o[1] = palette[i * 3 + 1]; o[2] = palette[i * 3 + 2]; }
                if (i < trns.size()) o[3] = trns[i];
            } else if (ctype == 0 || ctype == 4) {
                int g = sampleAt(row, x, 0);
                if (depth < 8) g = g * 255 / ((1 << depth) - 1);
                o[0] = o[1] = o[2] = static_cast<uint8_t>(g);
                if (ctype == 4) o[3] = static_cast<uint8_t>(sampleAt(row, x, 1));
            } else {
                o[0] = static_cast<uint8_t>(sampleAt(row, x, 0));
                o[1] = static_cast<uint8_t>(sampleAt(row, x, 1));
                o[2] = static_cast<uint8_t>(sampleAt(row, x, 2));
                if (ctype == 6) o[3] = static_cast<uint8_t>(sampleAt(row, x, 3));
            }
        }
    }
    return out;
}

// ---- TGA ---------------------------------------------------------------------------------------------------------------------
std::optional<DecodedImage> decodeTga(const std::vector<unsigned char>& b) {
    if (b.size() < 18) return std::nullopt;
    const int idLen = b[0], cmType = b[1], type = b[2];
    const int cmStart = b[3] | (b[4] << 8), cmLen = b[5] | (b[6] << 8), cmBits = b[7];
    const int w = b[12] | (b[13] << 8), h = b[14] | (b[15] << 8), bits = b[16], desc = b[17];
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return std::nullopt;
    const bool rle = type >= 9 && type <= 11, mapped = type == 1 || type == 9, gray = type == 3 || type == 11;
    if (!(type == 1 || type == 2 || type == 3 || rle)) return std::nullopt;
    if (!(bits == 8 || bits == 15 || bits == 16 || bits == 24 || bits == 32)) return std::nullopt;
    const size_t pxBytes = static_cast<size_t>((bits + 7) / 8);
    size_t pos = 18 + static_cast<size_t>(idLen);
    std::vector<uint8_t> cmap;
    if (cmType == 1) {
        const size_t eb = static_cast<size_t>((cmBits + 7) / 8), total = eb * static_cast<size_t>(cmLen);
        if (pos + total > b.size()) return std::nullopt;
        cmap.assign(b.begin() + static_cast<std::ptrdiff_t>(pos), b.begin() + static_cast<std::ptrdiff_t>(pos + total));
        pos += total;
    }
    if (mapped && (cmap.empty() || bits != 8)) return std::nullopt;
    DecodedImage out;
    out.width = w;
    out.height = h;
    out.rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 255);
    auto pixel = [&](const unsigned char* p, uint8_t* o) {
        if (mapped) {
            const size_t eb = static_cast<size_t>((cmBits + 7) / 8), i = (static_cast<size_t>(p[0]) - static_cast<size_t>(cmStart)) * eb;
            if (i + eb <= cmap.size() && eb >= 3) { o[0] = cmap[i + 2]; o[1] = cmap[i + 1]; o[2] = cmap[i]; o[3] = eb == 4 ? cmap[i + 3] : 255; }
        } else if (gray) {
            o[0] = o[1] = o[2] = p[0];
            if (bits == 16) o[3] = p[1];
        } else if (bits == 24 || bits == 32) {
            o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = bits == 32 ? p[3] : 255;
        } else {  // 15 / 16: A1 R5 G5 B5
            const int v = p[0] | (p[1] << 8);
            o[0] = static_cast<uint8_t>(((v >> 10) & 31) * 255 / 31);
            o[1] = static_cast<uint8_t>(((v >> 5) & 31) * 255 / 31);
            o[2] = static_cast<uint8_t>((v & 31) * 255 / 31);
            o[3] = 255;
        }
    };
    const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<uint8_t> px(total * 4, 255);
    size_t i = 0;
    if (!rle) {
        if (pos + total * pxBytes > b.size()) return std::nullopt;
        for (; i < total; ++i) pixel(&b[pos + i * pxBytes], &px[i * 4]);
    } else {
        while (i < total) {
            if (pos >= b.size()) return std::nullopt;
            const int head = b[pos++], count = (head & 127) + 1;
            if (head & 128) {
                if (pos + pxBytes > b.size()) return std::nullopt;
                uint8_t v[4];
                pixel(&b[pos], v);
                pos += pxBytes;
                for (int k = 0; k < count && i < total; ++k, ++i) std::memcpy(&px[i * 4], v, 4);
            } else {
                for (int k = 0; k < count && i < total; ++k, ++i) {
                    if (pos + pxBytes > b.size()) return std::nullopt;
                    pixel(&b[pos], &px[i * 4]);
                    pos += pxBytes;
                }
            }
        }
    }
    const bool topDown = (desc & 0x20) != 0, rightToLeft = (desc & 0x10) != 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int sy = topDown ? y : h - 1 - y, sx = rightToLeft ? w - 1 - x : x;
            std::memcpy(&out.rgba[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4], &px[(static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(sx)) * 4], 4);
        }
    return out;
}

// ---- JPEG (baseline / extended sequential Huffman, 8 bit, 1 or 3 components) ---------------------------------------------------
namespace {
struct JpegHuff {
    uint8_t bits[17] = {};
    uint8_t vals[256] = {};
    int mincode[17] = {}, maxcode[18] = {}, valptr[17] = {};
    bool present = false;
    void finish() {
        int code = 0, k = 0;
        for (int l = 1; l <= 16; ++l) {
            valptr[l] = k;
            mincode[l] = code;
            code += bits[l];
            k += bits[l];
            maxcode[l] = bits[l] ? code - 1 : -1;
            code <<= 1;
        }
        maxcode[17] = 0x7fffffff;
        present = true;
    }
};
struct JpegComp { int id = 0, h = 1, v = 1, tq = 0, td = 0, ta = 0, pred = 0; std::vector<uint8_t> plane; int pw = 0, ph = 0; };

const uint8_t kZigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
                             35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct JpegBits {
    const unsigned char* p;
    size_t n, pos;
    uint32_t buf = 0;
    int cnt = 0;
    bool hitMarker = false;
    int bit() {
        if (cnt == 0) {
            int c = 0;
            if (hitMarker || pos >= n) { hitMarker = true; c = 0; }
            else {
                c = p[pos++];
                if (c == 0xFF) {
                    const int c2 = pos < n ? p[pos] : 0;
                    if (c2 == 0) ++pos;
                    else { --pos; hitMarker = true; c = 0; }
                }
            }
            buf = static_cast<uint32_t>(c);
            cnt = 8;
        }
        --cnt;
        return static_cast<int>((buf >> cnt) & 1u);
    }
    int receive(int k) { int v = 0; for (int i = 0; i < k; ++i) v = (v << 1) | bit(); return v; }
    static int extend(int v, int t) { return t && v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }
    int decode(const JpegHuff& h) {
        int code = 0;
        for (int l = 1; l <= 16; ++l) {
            code = (code << 1) | bit();
            if (h.maxcode[l] >= 0 && code <= h.maxcode[l] && code >= h.mincode[l]) return h.vals[h.valptr[l] + code - h.mincode[l]];
        }
        return 0;
    }
    void reset() { cnt = 0; hitMarker = false; }
};
}  // namespace

std::optional<DecodedImage> decodeJpeg(const std::vector<unsigned char>& b) {
    if (b.size() < 4 || b[0] != 0xFF || b[1] != 0xD8) return std::nullopt;
    uint16_t qt[4][64] = {};
    JpegHuff dc[4], ac[4];
    std::vector<JpegComp> comps;
    int W = 0, H = 0, restart = 0;
    bool frame = false;
    size_t pos = 2;
    while (pos + 4 <= b.size()) {
        if (b[pos] != 0xFF) { ++pos; continue; }
        const int marker = b[pos + 1];
        if (marker == 0xFF || marker == 0x00 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) { pos += marker == 0xFF ? 1 : 2; continue; }
        if (marker == 0xD9) break;
        const size_t len = (static_cast<size_t>(b[pos + 2]) << 8) | b[pos + 3];
        if (len < 2 || pos + 2 + len > b.size()) return std::nullopt;
        const unsigned char* d = &b[pos + 4];
        const size_t dl = len - 2;
        if (marker == 0xDB) {
            size_t i = 0;
            while (i < dl) {
                const int pq = d[i] >> 4, tq = d[i] & 15;
                ++i;
                if (tq > 3) return std::nullopt;
                for (int k = 0; k < 64; ++k) {
                    if (pq) { if (i + 1 >= dl + 1) return std::nullopt; qt[tq][kZigzag[k]] = static_cast<uint16_t>((d[i] << 8) | d[i + 1]); i += 2; }
                    else { if (i >= dl) return std::nullopt; qt[tq][kZigzag[k]] = d[i++]; }
                }
            }
        } else if (marker == 0xC4) {
            size_t i = 0;
            while (i + 17 <= dl) {
                const int tc = d[i] >> 4, th = d[i] & 15;
                if (th > 3) return std::nullopt;
                JpegHuff& H = tc ? ac[th] : dc[th];
                int total = 0;
                for (int l = 1; l <= 16; ++l) { H.bits[l] = d[i + static_cast<size_t>(l)]; total += H.bits[l]; }
                i += 17;
                if (total > 256 || i + static_cast<size_t>(total) > dl) return std::nullopt;
                std::memcpy(H.vals, d + i, static_cast<size_t>(total));
                i += static_cast<size_t>(total);
                H.finish();
            }
        } else if (marker == 0xC0 || marker == 0xC1) {
            if (dl < 6 || d[0] != 8) return std::nullopt;
            H = (d[1] << 8) | d[2];
            W = (d[3] << 8) | d[4];
            const int nc = d[5];
            if ((nc != 1 && nc != 3) || dl < 6 + static_cast<size_t>(nc) * 3 || W <= 0 || H <= 0 || W > 16384 || H > 16384) return std::nullopt;
            comps.assign(static_cast<size_t>(nc), JpegComp{});
            for (int c = 0; c < nc; ++c) {
                comps[static_cast<size_t>(c)].id = d[6 + c * 3];
                comps[static_cast<size_t>(c)].h = d[7 + c * 3] >> 4;
                comps[static_cast<size_t>(c)].v = d[7 + c * 3] & 15;
                comps[static_cast<size_t>(c)].tq = d[8 + c * 3] & 3;
                if (comps[static_cast<size_t>(c)].h < 1 || comps[static_cast<size_t>(c)].h > 4 || comps[static_cast<size_t>(c)].v < 1 || comps[static_cast<size_t>(c)].v > 4) return std::nullopt;
            }
            frame = true;
        } else if (marker == 0xC2 || (marker >= 0xC3 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)) {
            return std::nullopt;  // progressive, lossless, arithmetic: not supported
        } else if (marker == 0xDD) {
            if (dl >= 2) restart = (d[0] << 8) | d[1];
        } else if (marker == 0xDA) {
            if (!frame || dl < 1) return std::nullopt;
            const int ns = d[0];
            if (ns != static_cast<int>(comps.size()) || dl < 1 + static_cast<size_t>(ns) * 2 + 3) return std::nullopt;
            for (int s = 0; s < ns; ++s) {
                for (auto& c : comps) if (c.id == d[1 + s * 2]) { c.td = d[2 + s * 2] >> 4; c.ta = d[2 + s * 2] & 15; }
            }
            for (auto& c : comps) if (c.td > 3 || c.ta > 3 || !dc[c.td].present || !ac[c.ta].present) return std::nullopt;
            int hmax = 1, vmax = 1;
            for (auto& c : comps) { hmax = std::max(hmax, c.h); vmax = std::max(vmax, c.v); }
            const int mcuW = 8 * hmax, mcuH = 8 * vmax, mcusX = (W + mcuW - 1) / mcuW, mcusY = (H + mcuH - 1) / mcuH;
            for (auto& c : comps) { c.pw = mcusX * c.h * 8; c.ph = mcusY * c.v * 8; c.plane.assign(static_cast<size_t>(c.pw) * static_cast<size_t>(c.ph), 0); }
            // cosine table for the inverse DCT
            static const std::array<float, 64> cosT = [] {
                std::array<float, 64> t{};
                for (int x = 0; x < 8; ++x)
                    for (int u = 0; u < 8; ++u) t[static_cast<size_t>(x * 8 + u)] = static_cast<float>((u == 0 ? std::sqrt(0.125) : 0.5) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0));
                return t;
            }();
            JpegBits br{b.data(), b.size(), pos + 2 + len};
            int sinceRestart = 0;
            for (int my = 0; my < mcusY; ++my)
                for (int mx = 0; mx < mcusX; ++mx) {
                    if (restart && sinceRestart == restart) {
                        br.reset();
                        while (br.pos + 1 < br.n && !(br.p[br.pos] == 0xFF && br.p[br.pos + 1] >= 0xD0 && br.p[br.pos + 1] <= 0xD7)) ++br.pos;
                        br.pos += 2;
                        for (auto& c : comps) c.pred = 0;
                        sinceRestart = 0;
                    }
                    ++sinceRestart;
                    for (auto& c : comps)
                        for (int by = 0; by < c.v; ++by)
                            for (int bx = 0; bx < c.h; ++bx) {
                                float blk[64] = {};
                                const int t = br.decode(dc[c.td]);
                                c.pred += JpegBits::extend(br.receive(t & 15), t & 15);
                                blk[0] = static_cast<float>(c.pred * qt[c.tq][0]);
                                for (int k = 1; k < 64;) {
                                    const int rs = br.decode(ac[c.ta]), r = rs >> 4, sz = rs & 15;
                                    if (sz == 0) { if (r == 15) { k += 16; continue; } break; }
                                    k += r;
                                    if (k > 63) break;
                                    const int z = kZigzag[k];
                                    blk[z] = static_cast<float>(JpegBits::extend(br.receive(sz), sz) * qt[c.tq][z]);
                                    ++k;
                                }
                                float tmp[64];
                                for (int y = 0; y < 8; ++y)       // columns (over v)
                                    for (int x = 0; x < 8; ++x) {
                                        float s = 0;
                                        for (int v = 0; v < 8; ++v) s += cosT[static_cast<size_t>(y * 8 + v)] * blk[v * 8 + x];
                                        tmp[y * 8 + x] = s;
                                    }
                                const int ox = (mx * c.h + bx) * 8, oy = (my * c.v + by) * 8;
                                for (int y = 0; y < 8; ++y)
                                    for (int x = 0; x < 8; ++x) {
                                        float s = 0;
                                        for (int u = 0; u < 8; ++u) s += cosT[static_cast<size_t>(x * 8 + u)] * tmp[y * 8 + u];
                                        c.plane[static_cast<size_t>(oy + y) * static_cast<size_t>(c.pw) + static_cast<size_t>(ox + x)] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(s + 128.f)), 0, 255));
                                    }
                            }
                }
            DecodedImage out;
            out.width = W;
            out.height = H;
            out.rgba.assign(static_cast<size_t>(W) * static_cast<size_t>(H) * 4, 255);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    float v[3] = {0, 0, 0};
                    for (size_t c = 0; c < comps.size(); ++c) {
                        const JpegComp& cc = comps[c];
                        const int sx = x * cc.h / hmax, sy = y * cc.v / vmax;
                        v[c] = cc.plane[static_cast<size_t>(sy) * static_cast<size_t>(cc.pw) + static_cast<size_t>(sx)];
                    }
                    uint8_t* o = &out.rgba[(static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)) * 4];
                    if (comps.size() == 1) { o[0] = o[1] = o[2] = static_cast<uint8_t>(v[0]); continue; }
                    const float Y = v[0], cb = v[1] - 128.f, cr = v[2] - 128.f;
                    o[0] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(Y + 1.402f * cr)), 0, 255));
                    o[1] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(Y - 0.344136f * cb - 0.714136f * cr)), 0, 255));
                    o[2] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(Y + 1.772f * cb)), 0, 255));
                }
            return out;
        }
        pos += 2 + len;
    }
    return std::nullopt;
}

std::optional<DecodedImage> decodeImage(const std::vector<unsigned char>& bytes, const std::string& extension) {
    std::string e = extension;
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // trust the content over the extension
    if (bytes.size() > 8 && bytes[0] == 0x89 && bytes[1] == 'P') return decodePng(bytes);
    if (bytes.size() > 3 && bytes[0] == 0xFF && bytes[1] == 0xD8) return decodeJpeg(bytes);
    if (e == ".tga") return decodeTga(bytes);
    if (e == ".png") return decodePng(bytes);
    if (e == ".jpg" || e == ".jpeg") return decodeJpeg(bytes);
    return decodeTga(bytes);
}

// ---- image -> mip chain --------------------------------------------------------------------------------------------------------
AlbedoImage makeAlbedoImage(const DecodedImage& in) {
    // linear float working copy
    int w = in.width, h = in.height;
    std::vector<float> cur(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (size_t i = 0; i < static_cast<size_t>(w) * static_cast<size_t>(h); ++i)
        for (size_t c = 0; c < 3; ++c) cur[i * 3 + c] = srgbToLinear(in.rgba[i * 4 + c]);
    auto halve = [&](const std::vector<float>& src, int sw, int sh, int& dw, int& dh) {
        dw = std::max(1, sw / 2);
        dh = std::max(1, sh / 2);
        std::vector<float> dst(static_cast<size_t>(dw) * static_cast<size_t>(dh) * 3);
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x)
                for (size_t c = 0; c < 3; ++c) {
                    float s = 0;
                    int n = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const int sx = std::min(sw - 1, x * 2 + dx), sy = std::min(sh - 1, y * 2 + dy);
                            s += src[(static_cast<size_t>(sy) * static_cast<size_t>(sw) + static_cast<size_t>(sx)) * 3 + c];
                            ++n;
                        }
                    dst[(static_cast<size_t>(y) * static_cast<size_t>(dw) + static_cast<size_t>(x)) * 3 + c] = s / static_cast<float>(n);
                }
        return dst;
    };
    while (std::max(w, h) > kAlbedoMaxSize) { int nw, nh; cur = halve(cur, w, h, nw, nh); w = nw; h = nh; }
    AlbedoImage img;
    {  // alpha mask: only when some texel is really transparent; nearest downsampling keeps hard cutout edges
        img.alphaKind = classifyAlpha(in.rgba.data(), static_cast<size_t>(in.width) * static_cast<size_t>(in.height));
        if (img.alphaKind != AlphaKind::None) {
            int aw = in.width, ah = in.height;
            while (std::max(aw, ah) > kAlphaMaskMaxSize) { aw = std::max(1, aw / 2); ah = std::max(1, ah / 2); }
            img.alphaWidth = aw;
            img.alphaHeight = ah;
            img.alpha.resize(static_cast<size_t>(aw) * static_cast<size_t>(ah));
            for (int y = 0; y < ah; ++y)
                for (int x = 0; x < aw; ++x) {
                    const int sx = std::min(in.width - 1, static_cast<int>((x + 0.5) * in.width / aw)), sy = std::min(in.height - 1, static_cast<int>((y + 0.5) * in.height / ah));
                    img.alpha[static_cast<size_t>(y) * static_cast<size_t>(aw) + static_cast<size_t>(x)] = in.rgba[(static_cast<size_t>(sy) * static_cast<size_t>(in.width) + static_cast<size_t>(sx)) * 4 + 3];
                }
        }
    }
    img.width = w;
    img.height = h;
    for (;;) {
        std::vector<uint8_t> level(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 255);
        for (size_t i = 0; i < static_cast<size_t>(w) * static_cast<size_t>(h); ++i)
            for (size_t c = 0; c < 3; ++c) level[i * 4 + c] = linearToSrgb(cur[i * 3 + c]);
        img.mips.push_back(std::move(level));
        if (w == 1 && h == 1) break;
        int nw, nh;
        cur = halve(cur, w, h, nw, nh);
        w = nw;
        h = nh;
    }
    return img;
}

Vec3 AlbedoImage::average() const {
    if (mips.empty()) return {1, 1, 1};
    const auto& m = mips.back();
    return {srgbToLinear(m[0]), srgbToLinear(m[1]), srgbToLinear(m[2])};
}

Vec3 AlbedoImage::sample(double u, double v, double lod) const {
    if (mips.empty()) return {1, 1, 1};
    const int top = mipCount() - 1;
    lod = std::clamp(lod, 0.0, static_cast<double>(top));
    const int l0 = static_cast<int>(std::floor(lod)), l1 = std::min(top, l0 + 1);
    const double f = lod - l0;
    auto bilinear = [&](int l) {
        const int w = mipWidth(l), h = mipHeight(l);
        const double x = u * w - 0.5, y = v * h - 0.5;
        const double fx0 = std::floor(x), fy0 = std::floor(y);
        const double fx = x - fx0, fy = y - fy0;
        auto wrap = [](long long i, int n) { long long r = i % n; return static_cast<int>(r < 0 ? r + n : r); };
        const int x0 = wrap(static_cast<long long>(fx0), w), x1 = wrap(static_cast<long long>(fx0) + 1, w);
        const int y0 = wrap(static_cast<long long>(fy0), h), y1 = wrap(static_cast<long long>(fy0) + 1, h);
        const auto& m = mips[static_cast<size_t>(l)];
        Vec3 acc;
        const int xs[2] = {x0, x1}, ys[2] = {y0, y1};
        const double wx[2] = {1 - fx, fx}, wy[2] = {1 - fy, fy};
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const size_t o = (static_cast<size_t>(ys[j]) * static_cast<size_t>(w) + static_cast<size_t>(xs[i])) * 4;
                acc = acc + Vec3{srgbToLinear(m[o]), srgbToLinear(m[o + 1]), srgbToLinear(m[o + 2])} * (wx[i] * wy[j]);
            }
        return acc;
    };
    if (f < 1e-6 || l1 == l0) return bilinear(l0);
    return bilinear(l0) * (1 - f) + bilinear(l1) * f;
}

// ---- library ------------------------------------------------------------------------------------------------------------------
AlbedoLibrary::AlbedoLibrary(std::filesystem::path assetsRoot, GameProfile profile) : root_(std::move(assetsRoot)), profile_(std::move(profile)) {}

size_t AlbedoLibrary::cached() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

AlphaKind classifyAlpha(const uint8_t* rgba, size_t texels) {
    size_t clear = 0, semi = 0;
    for (size_t i = 0; i < texels; ++i) {
        const uint8_t a = rgba[i * 4 + 3];
        if (a < 16) ++clear;
        else if (a < 240) ++semi;
    }
    if (clear + semi == 0) return AlphaKind::None;
    // soft edges of a cutout are a thin rim around its holes; glass is mostly in between
    return semi * 4 >= clear + semi ? AlphaKind::Translucent : AlphaKind::Cutout;
}

Transparency resolveTransparency(const std::string& propsJson, AlphaKind alpha) {
    const bool imageHasAlpha = alpha != AlphaKind::None;
    Transparency t;
    std::string mode = "auto";
    if (!propsJson.empty()) {
        const nlohmann::json j = nlohmann::json::parse(propsJson, nullptr, false);
        if (!j.is_discarded() && j.is_object() && j.contains("transparency") && j["transparency"].is_object()) {
            const auto& o = j["transparency"];
            if (o.contains("mode") && o["mode"].is_string()) mode = o["mode"].get<std::string>();
            auto num = [&](const char* k, float& v) { if (o.contains(k) && o[k].is_number() && std::isfinite(o[k].get<double>())) v = static_cast<float>(std::clamp(o[k].get<double>(), 0.0, 1.0)); };
            num("cutoff", t.cutoff);
            num("opacity", t.opacity);
            num("tint", t.tint);
        }
    }
    if (mode == "cutout") t.mode = imageHasAlpha ? TransparencyMode::Cutout : TransparencyMode::Opaque;  // nothing to cut without alpha
    else if (mode == "translucent") t.mode = TransparencyMode::Translucent;
    else if (mode == "opaque") t.mode = TransparencyMode::Opaque;
    else if (alpha == AlphaKind::Translucent) { t.mode = TransparencyMode::Translucent; t.opacity = 1.f; }  // auto, glass: the alpha is the opacity
    else t.mode = imageHasAlpha ? TransparencyMode::Cutout : TransparencyMode::Opaque;  // auto
    return t;
}

std::shared_ptr<const AlbedoImage> AlbedoLibrary::get(const std::string& texture) {
    namespace fs = std::filesystem;
    // find the file the way readTexture does (first matching extension), but keep its stat for change detection
    fs::path file;
    std::string ext;
    std::error_code ec;
    if (texture.empty() || texture.find("..") != std::string::npos || texture.front() == '/' || texture.find('\\') != std::string::npos || texture.find(':') != std::string::npos)
        return nullptr;
    for (const auto& e : profile_.textureExtensions) {
        const fs::path p = root_ / profile_.textureRoot / (texture + e);
        if (fs::is_regular_file(p, ec)) { file = p; ext = e; break; }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (file.empty()) { cache_.erase(texture); return nullptr; }
    const uintmax_t size = fs::file_size(file, ec);
    const int64_t time = static_cast<int64_t>(fs::last_write_time(file, ec).time_since_epoch().count());
    // the texture's properties (`<file>.json`, e.g. its transparency) count as part of it
    const fs::path side = fs::path(file.string() + ".json");
    uintmax_t propsSize = 0;
    int64_t propsTime = 0;
    if (fs::is_regular_file(side, ec)) {
        propsSize = fs::file_size(side, ec);
        propsTime = static_cast<int64_t>(fs::last_write_time(side, ec).time_since_epoch().count());
    }
    auto it = cache_.find(texture);
    if (it != cache_.end() && it->second.size == size && it->second.time == time && it->second.propsSize == propsSize && it->second.propsTime == propsTime)
        return it->second.image;
    Entry e;
    e.size = size;
    e.time = time;
    e.propsSize = propsSize;
    e.propsTime = propsTime;
    std::string propsText;
    if (propsSize > 0 && propsSize <= (1u << 20)) {
        std::ifstream ps(side, std::ios::binary);
        propsText.assign((std::istreambuf_iterator<char>(ps)), std::istreambuf_iterator<char>());
    }
    std::ifstream in(file, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() <= 64u * 1024u * 1024u) {
        if (auto img = decodeImage(bytes, ext)) {
            AlbedoImage a = makeAlbedoImage(*img);
            a.transparency = resolveTransparency(propsText, a.alphaKind);
            e.image = std::make_shared<const AlbedoImage>(std::move(a));
        }
    }
    cache_[texture] = e;
    return e.image;
}

}  // namespace core
