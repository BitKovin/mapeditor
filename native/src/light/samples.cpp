#include "light/samples.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace native::light {

// ---- encodings ------------------------------------------------------------------------------------------------------------------

namespace {

float signNotZero(float v) { return v >= 0.f ? 1.f : -1.f; }

uint32_t packSnorm16(float v) {
    const float c = std::clamp(v, -1.f, 1.f);
    return static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(std::lround(c * 32767.f))));
}

float unpackSnorm16(uint32_t bits) { return std::max(-1.f, static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(bits))) / 32767.f); }

} // namespace

uint32_t encodeNormalOct(const float n[3]) {
    const float l1 = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
    float x = l1 > 0.f ? n[0] / l1 : 0.f, y = l1 > 0.f ? n[1] / l1 : 0.f;
    if (n[2] < 0.f) {
        const float ox = (1.f - std::fabs(y)) * signNotZero(x), oy = (1.f - std::fabs(x)) * signNotZero(y);
        x = ox;
        y = oy;
    }
    return packSnorm16(x) | (packSnorm16(y) << 16);
}

void decodeNormalOct(uint32_t packed, float out[3]) {
    float x = unpackSnorm16(packed & 0xffffu), y = unpackSnorm16(packed >> 16);
    float z = 1.f - std::fabs(x) - std::fabs(y);
    if (z < 0.f) {
        const float ox = (1.f - std::fabs(y)) * signNotZero(x), oy = (1.f - std::fabs(x)) * signNotZero(y);
        x = ox;
        y = oy;
    }
    const float len = std::sqrt(x * x + y * y + z * z);
    out[0] = x / len;
    out[1] = y / len;
    out[2] = z / len;
}

uint32_t randomIdOfTexel(uint32_t texelIndex) {
    // PCG output hash
    uint32_t state = texelIndex * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// ---- preparing one chart ----------------------------------------------------------------------------------------------------------

std::vector<std::vector<core::LightSurface>> piecesByChart(const core::LightmapLayout& layout, const std::vector<core::LightSurface>& surfaces) {
    std::vector<std::vector<core::LightSurface>> pieces(layout.charts.size());
    for (const core::LightSurface& s : surfaces) {
        if (s.verts.size() < 3) continue;
        const core::LightmapFace* f = layout.findForFragment(s.instance, s.entityId, s.brushInstance, s.brushId, s.faceId, s.verts);
        if (f && f->chart >= 0) pieces[static_cast<size_t>(f->chart)].push_back(s);
    }
    return pieces;
}

PreparedChart prepareChart(const LiveLayout& live, int chart, const std::vector<core::LightSurface>& pieces, const core::SolidSet* solids, int giScale) {
    PreparedChart out;
    out.chart = chart;
    const LiveChart& lc = live.charts()[static_cast<size_t>(chart)];
    out.page = static_cast<uint32_t>(lc.where.page);
    const core::TexelSamples raw = core::buildChartSamples(live.layout(), chart, pieces, solids, &out.hidden);
    const size_t n = raw.count();
    if (n == 0) return out;
    const int atlas = raw.atlas;
    const int pad = live.settings().padding, ct = live.chunkTexels();
    const int innerX = lc.where.x + pad, innerY = lc.where.y + pad;
    const int innerW = std::max(1, lc.where.w - 2 * pad), innerH = std::max(1, lc.where.h - 2 * pad);
    const int nx = (innerW + ct - 1) / ct, ny = (innerH + ct - 1) / ct;

    // chunk of every sample (a texel in the padding belongs to the nearest chunk), then a stable counting sort
    std::vector<uint32_t> chunkOf(n);
    std::vector<uint32_t> start(static_cast<size_t>(nx) * ny + 1, 0);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t t = raw.texel[i];
        const int x = std::clamp(static_cast<int>(t % static_cast<uint32_t>(atlas)), innerX, innerX + innerW - 1);
        const int y = std::clamp(static_cast<int>((t / static_cast<uint32_t>(atlas)) % static_cast<uint32_t>(atlas)), innerY, innerY + innerH - 1);
        chunkOf[i] = static_cast<uint32_t>(((y - innerY) / ct) * nx + (x - innerX) / ct);
        ++start[chunkOf[i] + 1];
    }
    for (size_t c = 0; c < static_cast<size_t>(nx) * ny; ++c) start[c + 1] += start[c];
    std::vector<uint32_t> order(n);
    {
        std::vector<uint32_t> fill(start.begin(), start.end() - 1);
        for (size_t i = 0; i < n; ++i) order[fill[chunkOf[i]]++] = static_cast<uint32_t>(i);
    }
    core::TexelSamples sorted;
    sorted.atlas = raw.atlas;
    sorted.pageCount = raw.pageCount;
    sorted.pos.resize(n * 4);
    sorted.nrm.resize(n * 4);
    sorted.texel.resize(n);
    const bool hasSub = raw.hasSub();
    if (hasSub) sorted.sub.resize(n * 12);
    for (size_t k = 0; k < n; ++k) {
        const size_t i = order[k];
        std::memcpy(&sorted.pos[k * 4], &raw.pos[i * 4], 4 * sizeof(float));
        std::memcpy(&sorted.nrm[k * 4], &raw.nrm[i * 4], 4 * sizeof(float));
        sorted.texel[k] = raw.texel[i];
        if (hasSub) std::memcpy(&sorted.sub[k * 12], &raw.sub[i * 12], 12 * sizeof(float));
    }
    core::GiSamples gi;
    if (giScale > 1 && hasSub) gi = core::buildGiSamples(sorted, giScale);
    out.gridScale = gi.scale;
    out.count = static_cast<uint32_t>(n);
    out.gridCount = static_cast<uint32_t>(gi.count());

    // records
    out.samples.resize(n);
    if (hasSub) out.sub.assign(sorted.sub.begin(), sorted.sub.end());
    out.grid.resize(out.gridCount);
    const uint32_t perPage = static_cast<uint32_t>(atlas) * static_cast<uint32_t>(atlas);
    for (size_t k = 0; k < n; ++k) {
        SampleRecord& r = out.samples[k];
        const uint32_t t = sorted.texel[k];
        std::memcpy(r.pos, &sorted.pos[k * 4], 3 * sizeof(float));
        r.randomId = randomIdOfTexel(t);
        r.normalOct = encodeNormalOct(&sorted.nrm[k * 4]);
        r.xy = (t % static_cast<uint32_t>(atlas)) | (((t / static_cast<uint32_t>(atlas)) % static_cast<uint32_t>(atlas)) << 16);
        r.page = t / perPage;
        r.aux = 0;
    }
    if (out.gridCount > 0) {
        const uint32_t fine = static_cast<uint32_t>(gi.s.atlas), finePerPage = fine * fine;
        for (size_t j = 0; j < out.gridCount; ++j) {
            SampleRecord& r = out.grid[j];
            const uint32_t t = gi.s.texel[j];
            std::memcpy(r.pos, &gi.s.pos[j * 4], 3 * sizeof(float));
            r.randomId = randomIdOfTexel(t);
            r.normalOct = encodeNormalOct(&gi.s.nrm[j * 4]);
            r.xy = (t % fine) | (((t / fine) % fine) << 16);
            r.page = t / finePerPage;
            r.aux = gi.owner[j];   // chart-relative until upload() adds the slot base
        }
    }

    // chunk rows: the non-empty chunks
    for (size_t c = 0; c < static_cast<size_t>(nx) * ny; ++c) {
        const uint32_t first = start[c], count = start[c + 1] - start[c];
        if (count == 0) continue;
        PreparedChart::Chunk k;
        const int cx = static_cast<int>(c % static_cast<size_t>(nx)), cy = static_cast<int>(c / static_cast<size_t>(nx));
        k.x = static_cast<uint32_t>(innerX + cx * ct);
        k.y = static_cast<uint32_t>(innerY + cy * ct);
        k.w = static_cast<uint32_t>(std::min(ct, innerW - cx * ct));
        k.h = static_cast<uint32_t>(std::min(ct, innerH - cy * ct));
        k.first = first;
        k.count = count;
        if (out.gridCount > 0) {
            k.gridFirst = gi.begin[first];
            k.gridCount = gi.begin[first + count] - gi.begin[first];
        }
        for (int a = 0; a < 3; ++a) { k.boundsMin[a] = 1e30f; k.boundsMax[a] = -1e30f; }
        for (uint32_t i = first; i < first + count; ++i)
            for (int a = 0; a < 3; ++a) {
                k.boundsMin[a] = std::min(k.boundsMin[a], sorted.pos[i * 4 + static_cast<size_t>(a)]);
                k.boundsMax[a] = std::max(k.boundsMax[a], sorted.pos[i * 4 + static_cast<size_t>(a)]);
            }
        out.chunks.push_back(k);
    }
    return out;
}

// ---- the store ---------------------------------------------------------------------------------------------------------------------

SampleStore::SampleStore(gpu::Device& device, uint32_t slotCapacity, uint32_t gridCapacity, uint32_t chunkCapacity)
    : slotCapacity_(slotCapacity),
      samples_(device, static_cast<uint64_t>(slotCapacity) * sizeof(SampleRecord), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "light samples"),
      sub_(device, static_cast<uint64_t>(slotCapacity) * 48, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "light sub texel data"),
      grid_(device, static_cast<uint64_t>(std::max<uint32_t>(gridCapacity, 1)) * sizeof(SampleRecord), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "light grid"),
      table_(device, static_cast<uint64_t>(chunkCapacity) * sizeof(GpuChunk), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "light chunks") {}

SampleStore::~SampleStore() = default;

void SampleStore::release(int chart) {
    if (chart < 0 || static_cast<size_t>(chart) >= charts_.size()) return;
    ChartEntry& e = charts_[static_cast<size_t>(chart)];
    if (e.samples.ok()) samples_.free(e.samples);
    if (e.grid.ok()) grid_.free(e.grid);
    if (e.table.ok()) table_.free(e.table);
    e = {};
}

void SampleStore::clear() {
    for (size_t i = 0; i < charts_.size(); ++i) release(static_cast<int>(i));
    charts_.clear();
}

uint32_t SampleStore::liveCharts() const {
    uint32_t n = 0;
    for (const ChartEntry& e : charts_) n += e.samples.ok() ? 1 : 0;
    return n;
}

bool SampleStore::upload(VkCommandBuffer cmd, gpu::UploadRing& ring, const PreparedChart& chart, uint32_t page) {
    if (chart.chart < 0) return false;
    release(chart.chart);
    if (charts_.size() <= static_cast<size_t>(chart.chart)) charts_.resize(static_cast<size_t>(chart.chart) + 1);
    if (chart.samples.empty()) return true;   // a chart without samples has nothing on the GPU
    ChartEntry e;
    e.samples = samples_.allocate(chart.samples.size() * sizeof(SampleRecord), sizeof(SampleRecord));
    if (!e.samples.ok()) return false;
    if (chart.gridCount > 0) {
        e.grid = grid_.allocate(static_cast<VkDeviceSize>(chart.gridCount) * sizeof(SampleRecord), sizeof(SampleRecord));
        if (!e.grid.ok()) {
            samples_.free(e.samples);
            return false;
        }
    }
    e.table = table_.allocate(chart.chunks.size() * sizeof(GpuChunk), 64);
    if (!e.table.ok()) {
        samples_.free(e.samples);
        if (e.grid.ok()) grid_.free(e.grid);
        return false;
    }
    e.slotBase = static_cast<uint32_t>(e.samples.offset / sizeof(SampleRecord));
    e.gridBase = e.grid.ok() ? static_cast<uint32_t>(e.grid.offset / sizeof(SampleRecord)) : 0;
    e.count = chart.count;
    e.gridCount = chart.gridCount;
    e.chunkCount = static_cast<uint32_t>(chart.chunks.size());
    e.firstChunk = static_cast<uint32_t>(e.table.offset / sizeof(GpuChunk));
    std::vector<GpuChunk> rows(chart.chunks.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        const PreparedChart::Chunk& c = chart.chunks[i];
        GpuChunk& g = rows[i];
        g = {};
        g.chart = static_cast<uint32_t>(chart.chart);
        g.page = page;
        g.rectXY = c.x | (c.y << 16);
        g.rectWH = c.w | (c.h << 16);
        g.slotBase = e.slotBase + c.first;
        g.slotCount = c.count;
        g.gridBase = e.gridBase + c.gridFirst;
        g.gridCount = c.gridCount;
        g.state = 1;
        std::memcpy(g.boundsMin, c.boundsMin, sizeof(g.boundsMin));
        std::memcpy(g.boundsMax, c.boundsMax, sizeof(g.boundsMax));
    }
    std::vector<SampleRecord> grid = chart.grid;
    for (SampleRecord& r : grid) r.aux += e.slotBase;   // the owning base sample as a slot
    bool ok = ring.upload(cmd, samples_.buffer(), e.samples.offset, chart.samples.data(), chart.samples.size() * sizeof(SampleRecord)) &&
              ring.upload(cmd, table_.buffer(), e.table.offset, rows.data(), rows.size() * sizeof(GpuChunk));
    if (ok && !chart.sub.empty()) ok = ring.upload(cmd, sub_.buffer(), static_cast<VkDeviceSize>(e.slotBase) * 48, chart.sub.data(), chart.sub.size() * sizeof(float));
    if (ok && !grid.empty()) ok = ring.upload(cmd, grid_.buffer(), e.grid.offset, grid.data(), grid.size() * sizeof(SampleRecord));
    if (!ok) {
        samples_.free(e.samples);
        if (e.grid.ok()) grid_.free(e.grid);
        table_.free(e.table);
        return false;
    }
    if (rows_.size() < e.firstChunk + rows.size()) rows_.resize(e.firstChunk + rows.size());
    std::copy(rows.begin(), rows.end(), rows_.begin() + e.firstChunk);
    charts_[static_cast<size_t>(chart.chart)] = std::move(e);
    return true;
}

} // namespace native::light
