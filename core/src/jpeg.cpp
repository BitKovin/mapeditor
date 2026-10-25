#include "core/jpeg.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace core {

namespace {

constexpr uint8_t kZigZag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// Annex K.1 tables (natural order)
constexpr uint8_t kLumaQ[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,  14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,  49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr uint8_t kChromaQ[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                                  99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// Annex K.3 Huffman tables: bits[1..16] counts, then the symbols
constexpr uint8_t kDcLumaBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
constexpr uint8_t kDcChromaBits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
constexpr uint8_t kDcVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr uint8_t kAcLumaBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
constexpr uint8_t kAcLumaVals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa};
constexpr uint8_t kAcChromaBits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
constexpr uint8_t kAcChromaVals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa};

struct Huff {
    uint16_t code[256] = {};
    uint8_t size[256] = {};
    void build(const uint8_t bits[16], const uint8_t* vals) {
        int k = 0;
        uint32_t c = 0;
        for (int len = 1; len <= 16; ++len) {
            for (int i = 0; i < bits[len - 1]; ++i) { code[vals[k]] = static_cast<uint16_t>(c++); size[vals[k]] = static_cast<uint8_t>(len); ++k; }
            c <<= 1;
        }
    }
};

struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t acc = 0;
    int n = 0;
    explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}
    void put(uint32_t bits, int len) {
        if (len == 0) return;
        acc = (acc << len) | (bits & ((1u << len) - 1));
        n += len;
        while (n >= 8) {
            const uint8_t b = static_cast<uint8_t>(acc >> (n - 8));
            out.push_back(b);
            if (b == 0xFF) out.push_back(0);  // byte stuffing
            n -= 8;
        }
        acc &= (1u << n) - 1;
    }
    void flush() { if (n > 0) put(0x7F, 8 - n); }
};

void marker(std::vector<uint8_t>& o, uint8_t m) { o.push_back(0xFF); o.push_back(m); }
void be16(std::vector<uint8_t>& o, int v) { o.push_back(static_cast<uint8_t>(v >> 8)); o.push_back(static_cast<uint8_t>(v)); }

void dqt(std::vector<uint8_t>& o, int id, const uint8_t q[64]) {
    marker(o, 0xDB);
    be16(o, 67);
    o.push_back(static_cast<uint8_t>(id));
    for (int i = 0; i < 64; ++i) o.push_back(q[kZigZag[i]]);
}

void dht(std::vector<uint8_t>& o, int tcth, const uint8_t bits[16], const uint8_t* vals, int count) {
    marker(o, 0xC4);
    be16(o, 2 + 1 + 16 + count);
    o.push_back(static_cast<uint8_t>(tcth));
    for (int i = 0; i < 16; ++i) o.push_back(bits[i]);
    for (int i = 0; i < count; ++i) o.push_back(vals[i]);
}

int bitLength(int v) { int n = 0; while (v) { ++n; v >>= 1; } return n; }

// forward DCT of an 8x8 block (separable, float), input level shifted
void fdct(const float in[64], float out[64]) {
    static float c[8][8];
    static bool init = false;
    if (!init) {
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x) c[u][x] = static_cast<float>((u == 0 ? std::sqrt(0.125) : 0.5) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0));
        init = true;
    }
    float tmp[64];
    for (int y = 0; y < 8; ++y)
        for (int u = 0; u < 8; ++u) {
            float s = 0;
            for (int x = 0; x < 8; ++x) s += c[u][x] * in[y * 8 + x];
            tmp[y * 8 + u] = s;
        }
    for (int u = 0; u < 8; ++u)
        for (int v = 0; v < 8; ++v) {
            float s = 0;
            for (int y = 0; y < 8; ++y) s += c[v][y] * tmp[y * 8 + u];
            out[v * 8 + u] = s;
        }
}

}  // namespace

std::vector<uint8_t> encodeJpeg(int width, int height, const uint8_t* rgb, int quality) {
    quality = std::clamp(quality, 1, 100);
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    uint8_t lq[64], cq[64];
    for (int i = 0; i < 64; ++i) {
        lq[i] = static_cast<uint8_t>(std::clamp((kLumaQ[i] * scale + 50) / 100, 1, 255));
        cq[i] = static_cast<uint8_t>(std::clamp((kChromaQ[i] * scale + 50) / 100, 1, 255));
    }
    Huff dcL, dcC, acL, acC;
    dcL.build(kDcLumaBits, kDcVals);
    dcC.build(kDcChromaBits, kDcVals);
    acL.build(kAcLumaBits, kAcLumaVals);
    acC.build(kAcChromaBits, kAcChromaVals);

    std::vector<uint8_t> o;
    marker(o, 0xD8);
    marker(o, 0xE0);  // JFIF
    be16(o, 16);
    const uint8_t jfif[] = {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    o.insert(o.end(), jfif, jfif + sizeof jfif);
    dqt(o, 0, lq);
    dqt(o, 1, cq);
    marker(o, 0xC0);  // baseline, 3 components 1x1 sampling
    be16(o, 17);
    o.push_back(8);
    be16(o, height);
    be16(o, width);
    o.push_back(3);
    for (int i = 0; i < 3; ++i) { o.push_back(static_cast<uint8_t>(i + 1)); o.push_back(0x11); o.push_back(i == 0 ? 0 : 1); }
    dht(o, 0x00, kDcLumaBits, kDcVals, 12);
    dht(o, 0x10, kAcLumaBits, kAcLumaVals, 162);
    dht(o, 0x01, kDcChromaBits, kDcVals, 12);
    dht(o, 0x11, kAcChromaBits, kAcChromaVals, 162);
    marker(o, 0xDA);
    be16(o, 12);
    o.push_back(3);
    for (int i = 0; i < 3; ++i) { o.push_back(static_cast<uint8_t>(i + 1)); o.push_back(i == 0 ? 0x00 : 0x11); }
    o.push_back(0); o.push_back(63); o.push_back(0);

    BitWriter bw(o);
    int prevDc[3] = {0, 0, 0};
    for (int by = 0; by < height; by += 8)
        for (int bx = 0; bx < width; bx += 8) {
            float planes[3][64];
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const int px = std::min(bx + x, width - 1), py = std::min(by + y, height - 1);  // edge replication
                    const uint8_t* p = rgb + (static_cast<size_t>(py) * width + px) * 3;
                    const float r = p[0], g = p[1], b = p[2];
                    planes[0][y * 8 + x] = 0.299f * r + 0.587f * g + 0.114f * b - 128.f;
                    planes[1][y * 8 + x] = -0.168736f * r - 0.331264f * g + 0.5f * b;
                    planes[2][y * 8 + x] = 0.5f * r - 0.418688f * g - 0.081312f * b;
                }
            for (int c = 0; c < 3; ++c) {
                float dct[64];
                fdct(planes[c], dct);
                const uint8_t* q = c == 0 ? lq : cq;
                int zz[64];
                for (int i = 0; i < 64; ++i) zz[i] = static_cast<int>(std::lround(dct[kZigZag[i]] / q[kZigZag[i]]));
                const Huff& dc = c == 0 ? dcL : dcC;
                const Huff& ac = c == 0 ? acL : acC;
                const int diff = zz[0] - prevDc[c];
                prevDc[c] = zz[0];
                int nb = bitLength(std::abs(diff));
                bw.put(dc.code[nb], dc.size[nb]);
                if (nb) bw.put(static_cast<uint32_t>(diff < 0 ? diff - 1 : diff), nb);
                int run = 0;
                for (int i = 1; i < 64; ++i) {
                    if (zz[i] == 0) { ++run; continue; }
                    while (run > 15) { bw.put(ac.code[0xF0], ac.size[0xF0]); run -= 16; }
                    nb = bitLength(std::abs(zz[i]));
                    const int sym = (run << 4) | nb;
                    bw.put(ac.code[sym], ac.size[sym]);
                    bw.put(static_cast<uint32_t>(zz[i] < 0 ? zz[i] - 1 : zz[i]), nb);
                    run = 0;
                }
                if (run > 0) bw.put(ac.code[0x00], ac.size[0x00]);
            }
        }
    bw.flush();
    marker(o, 0xD9);
    return o;
}

}  // namespace core
