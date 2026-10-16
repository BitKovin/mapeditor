#pragma once
// Stand-in for the Vulkan LiveSession (tests, bench/bench_live.cpp): the light is random noise, the cost is a few memory passes plus an optional
// emulated GPU time, so everything around the light can be exercised and timed without a GPU.
#include "gpu_bake.h"
#include <chrono>
#include <thread>

namespace testutil {
using namespace core;

// Stand-in for the GPU session: results are noise, the cost is a few memory passes plus an optional emulated GPU time.
class NoiseSession final : public gpu::LiveSession {
public:
    NoiseSession(size_t atlasTexels, size_t m, bool gather, double gpuMs) : m_(m), gather_(gather), gpuMs_(gpuMs), atlas_(atlasTexels * 4, 0.f) {}
    bool setLights(const LightScene&) override { return true; }
    bool run(const std::vector<Job>& jobs, std::vector<float>& d, std::vector<float>& db, std::vector<float>* ind) override {
        size_t total = 0, gi = 0;
        for (const Job& j : jobs) { total += j.count; gi += j.giCount; }
        auto fill = [&](std::vector<float>& v, size_t count) {
            v.resize(count * 4);
            for (size_t i = 0; i < v.size(); ++i) {
                seed_ = seed_ * 1664525u + 1013904223u;
                v[i] = (i & 3) == 3 ? 0.f : static_cast<float>(seed_ >> 8) * (1.f / 16777216.f);
            }
        };
        fill(d, total);
        fill(db, total);
        if (ind) fill(*ind, gather_ ? (m_ ? gi : total) : 0);
        if (gpuMs_ > 0) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(gpuMs_));
        gpuSec_ = gpuMs_ / 1000.0;
        return true;
    }
    float* atlas() override { return atlas_.data(); }
    const std::string& error() const override { return err_; }
    const std::string& device() const override { return dev_; }
    const std::vector<std::string>& validation() const override { return val_; }
    double lastGpuSeconds() const override { return gpuSec_; }

private:
    size_t m_;
    bool gather_;
    double gpuMs_;
    std::vector<float> atlas_;
    std::string err_, dev_ = "noise";
    std::vector<std::string> val_;
    double gpuSec_ = 0;
    uint32_t seed_ = 12345;
};


// A LiveBaker::SessionFactory making NoiseSessions (`gpuMs` per run(), `sessionMs` to create one).
inline LiveBaker::SessionFactory noiseFactory(double gpuMs = 0, double sessionMs = 0) {
    return [gpuMs, sessionMs](const TexelSamples& resident, const OccluderMesh&, const LightScene&, const gpu::LiveSessionConfig& sc, const std::vector<float>&,
                              std::string&) -> std::unique_ptr<gpu::LiveSession> {
        if (sessionMs > 0) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(sessionMs));
        const size_t texels = static_cast<size_t>(resident.pageCount) * static_cast<size_t>(resident.atlas) * static_cast<size_t>(resident.atlas);
        return std::make_unique<NoiseSession>(texels, sc.giEntries, sc.gather, gpuMs);
    };
}
}  // namespace testutil
