// light/samples -- per-chart texel samples on the GPU: the sample arena and the chunk table.
//
// Owns: the device-local sample buffers and the chunk table (a second arena of 64-byte chunk records, CPU copy + GPU mirror). A chart's samples come from
// core::buildChartSamples (T5.3), are cut into chunks of chunkTexels x chunkTexels (the scheduling unit, 06 §4) and reordered so that every chunk's samples are
// contiguous; empty chunks get no record.
// Threads: prepareChart() is pure CPU and thread safe (run it in jobs); upload() / release() belong to the bake thread (the one that records the upload).
// Freeing a block the GPU may still read is the caller's job to delay (gpu/arena.h).
// Spec: 06_LIGHTING.md §4, 08 T6.2 / T6.4.
//
// GPU layout: structure of arrays addressed by one SLOT per sample. A chart owns a contiguous range of slots, a chunk a sub-range of it:
//   samples[slot]  SampleRecord (32 B): float pos[3]; uint randomId; uint normalOct (two snorm16, octahedral); uint xy (x | y << 16); uint page; uint aux (0)
//   sub[slot]      12 floats: the sub-texel data of buildTexelSamples
//   grid[entry]    SampleRecord of a fine GI grid entry: xy = the fine cell (texel * scale + cell), aux = the slot of the base sample that owns it
//   chunks[id]     GpuChunk (64 B)
// The result buffers of the bake (light/results.h) are indexed by the same slot.
#pragma once

#include <cstdint>
#include <vector>

#include "core/lightbake.h"
#include "gpu/arena.h"
#include "gpu/upload_ring.h"
#include "light/layout.h"

namespace native::light {

struct SampleRecord {
    float pos[3];
    uint32_t randomId;
    uint32_t normalOct;
    uint32_t xy;
    uint32_t page;
    uint32_t aux;
};
static_assert(sizeof(SampleRecord) == 32);

// The chunk table row the passes read: sample i of the chunk is slot `slotBase + i`, grid entry j is `gridBase + j`.
struct GpuChunk {
    uint32_t chart;
    uint32_t page;
    uint32_t rectXY;          // x | y << 16: the chunk's texel rectangle in its page
    uint32_t rectWH;          // w | h << 16
    uint32_t slotBase, slotCount;
    uint32_t gridBase, gridCount;
    uint32_t state;           // bit 0: valid; the scheduler (T6.5) owns the rest
    float boundsMin[3];
    float boundsMax[3];
    uint32_t pad;
};
static_assert(sizeof(GpuChunk) == 64);

// Octahedral unit vector <-> two snorm16 in one word (the shaders decode the same way).
uint32_t encodeNormalOct(const float n[3]);
void decodeNormalOct(uint32_t packed, float out[3]);
// The id a sample hashes its random numbers with: a function of its texel only, so re-evaluating a chunk reproduces what a full bake would have drawn.
uint32_t randomIdOfTexel(uint32_t texelIndex);

// The CPU form of one chart's data, ready to upload (thread safe to make).
struct PreparedChart {
    int chart = -1;
    std::vector<SampleRecord> samples;   // chunk-major order
    std::vector<float> sub;              // 12 floats per sample, same order (empty without sub-texel data)
    std::vector<SampleRecord> grid;      // fine GI grid entries, base-sample order; aux = chart-relative index of the owner
    uint32_t count = 0, gridCount = 0;
    int gridScale = 1;
    struct Chunk {
        uint32_t x = 0, y = 0, w = 0, h = 0;     // page rectangle of the chunk
        uint32_t first = 0, count = 0;           // samples (indices in the chart)
        uint32_t gridFirst = 0, gridCount = 0;   // grid entries
        float boundsMin[3] = {0, 0, 0}, boundsMax[3] = {0, 0, 0};
    };
    std::vector<Chunk> chunks;           // non-empty chunks, in chunk order
    uint32_t page = 0;
    size_t hidden = 0;                   // texels dropped because they lie inside a solid
};

// The pieces of every chart: surfaces[i] goes to the chart whose face contains it (pieces without a chart are dropped).
std::vector<std::vector<core::LightSurface>> piecesByChart(const core::LightmapLayout& layout, const std::vector<core::LightSurface>& surfaces);

PreparedChart prepareChart(const LiveLayout& live, int chart, const std::vector<core::LightSurface>& pieces, const core::SolidSet* solids, int giScale = 1);

class SampleStore {
public:
    // slotCapacity: samples (= result slots) the store can hold; gridCapacity: fine grid entries; chunkCapacity: rows of the chunk table.
    SampleStore(gpu::Device& device, uint32_t slotCapacity, uint32_t gridCapacity, uint32_t chunkCapacity);
    ~SampleStore();
    SampleStore(const SampleStore&) = delete;
    SampleStore& operator=(const SampleStore&) = delete;

    bool valid() const { return samples_.valid() && sub_.buffer() != VK_NULL_HANDLE && grid_.valid() && table_.valid(); }
    VkBuffer sampleBuffer() const { return samples_.buffer(); }
    VkBuffer subBuffer() const { return sub_.buffer(); }
    VkBuffer gridBuffer() const { return grid_.buffer(); }
    VkBuffer chunkBuffer() const { return table_.buffer(); }
    uint32_t slotCapacity() const { return slotCapacity_; }

    // Gives the chart its slots (its old ones, if any, are freed first), records the copies of its samples and chunk rows. False when an arena is full (the chart
    // then has no data). The caller orders transfer -> use with a barrier after the frame's uploads.
    bool upload(VkCommandBuffer cmd, gpu::UploadRing& ring, const PreparedChart& chart, uint32_t page);
    void release(int chart);
    void clear();

    struct ChartEntry {
        gpu::Arena::Block samples, grid, table;
        uint32_t slotBase = 0, gridBase = 0;   // first slot / first grid entry
        uint32_t count = 0, gridCount = 0, chunkCount = 0;
        uint32_t firstChunk = 0;               // chunk id of its first row (rows are consecutive)
    };
    const ChartEntry* chart(int chart) const { return chart >= 0 && static_cast<size_t>(chart) < charts_.size() && charts_[static_cast<size_t>(chart)].samples.ok() ? &charts_[static_cast<size_t>(chart)] : nullptr; }
    // The CPU copy of the chunk table, by chunk id (rows of released charts keep stale content, use chart() to know what is live).
    const GpuChunk& chunk(uint32_t id) const { return rows_[id]; }
    size_t chunkRows() const { return rows_.size(); }
    uint32_t usedSlots() const { return static_cast<uint32_t>(samples_.usedBytes() / sizeof(SampleRecord)); }
    uint64_t chunkBytes() const { return table_.usedBytes(); }
    uint32_t liveCharts() const;

private:
    uint32_t slotCapacity_;
    gpu::Arena samples_;   // its allocations decide the slots; sub_ (and the results) are only buffers indexed by the same slot numbers
    gpu::Arena sub_;
    gpu::Arena grid_;
    gpu::Arena table_;
    std::vector<ChartEntry> charts_;
    std::vector<GpuChunk> rows_;
};

} // namespace native::light
