// T6.4: the direct light and ambient occlusion passes against core::LiveBaker (bounces off), per light class. [gpu]: skips itself without a ray tracing device.
// After maxPasses passes the native values agree with the Core's within the noise of different random ids: mean relative difference < 1 %, 99th percentile < 5 %
// over the texels above 0.01 (06_LIGHTING.md §1).
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "light_bake_fixture.h"

using namespace native;
using native_test::LightFixture;
namespace fs = std::filesystem;

namespace {

struct Stat {
    double mean = 0, p99 = 0, worst = 0, bias = 0;   // bias: mean of the signed relative difference
    size_t count = 0;
};

// Relative differences of the channels above `floorValue` of the reference.
Stat summarize(std::vector<double>& rel, const std::vector<double>* signedRel = nullptr) {
    Stat s;
    s.count = rel.size();
    if (rel.empty()) return s;
    double sum = 0;
    for (double r : rel) sum += r;
    if (signedRel && !signedRel->empty()) {
        double b = 0;
        for (double v : *signedRel) b += v;
        s.bias = b / static_cast<double>(signedRel->size());
    }
    s.mean = sum / static_cast<double>(rel.size());
    std::sort(rel.begin(), rel.end());
    s.p99 = rel[std::min(rel.size() - 1, static_cast<size_t>(static_cast<double>(rel.size()) * 0.99))];
    s.worst = rel.back();
    return s;
}

struct ParityResult {
    Stat direct, shown, ao;
    size_t samples = 0, matched = 0;
};

// Runs the native bake and the Core's reference on `map` and compares the direct light (ambient + the three classes) and the shown value (times the AO factor).
// `rooms` > 0: the generated rooms map instead of `map`.
ParityResult compareToCore(gpu::Device& device, const std::string& map, int rooms, const std::function<void(core::LightScene&)>& edit, int maxPasses, bool refGpu,
                           const light::DirectParams& params) {
    ParityResult out;
    LightFixture f(device);
    const fs::path root = NATIVE_CORE_ASSETS_DIR;
    if (!(rooms > 0 ? f.loadRooms(root, rooms, edit) : f.load(root, map, edit))) return out;

    f.bake(static_cast<uint32_t>(maxPasses), params);
    const LightFixture::Readback r = f.readResults();
    MESSAGE(f.sampleCount << " samples, " << f.lights.lights.size() << " lights");

    core::LiveSettings s;
    s.bounces = false;
    s.useGpu = refGpu;
    s.denoiseIndirect = false;
    s.useTextures = false;
    s.maxPasses = maxPasses;
    s.areaSamplesPerPass = static_cast<int>(params.areaSamples);
    s.skySamplesPerPass = static_cast<int>(params.skySamples);
    s.directSupersample = static_cast<int>(params.superSamples);
    s.classes = f.classes;
    core::LiveBaker ref(s);
    ref.setGeometry(f.live->layout(), f.surfaces, {}, f.solids);
    ref.setLights(f.lights);
    const core::Vec3 cam{0, 0, 0};
    int guard = 0;
    while (!ref.converged() && guard++ < 10000) ref.step(&cam, 1000000);
    REQUIRE(ref.converged());

    const uint32_t atlas = static_cast<uint32_t>(f.live->layout().settings.atlasSize);
    std::unordered_map<uint32_t, size_t> byTexel;
    for (size_t i = 0; i < ref.sampleCount(); ++i) byTexel[ref.samples().texel[i]] = i;

    std::vector<double> relDirect, relShown, relAo, sgnDirect;
    const core::AoSettings& ao = f.lights.level.ao;
    const uint32_t used = f.store->usedSlots();
    out.samples = used;
    for (uint32_t slot = 0; slot < used; ++slot) {
        const auto* rec = reinterpret_cast<const light::SampleRecord*>(r.samples.data() + static_cast<size_t>(slot) * sizeof(light::SampleRecord));
        const uint32_t texel = (rec->page * atlas + (rec->xy >> 16)) * atlas + (rec->xy & 0xffffu);
        const auto it = byTexel.find(texel);
        if (it == byTexel.end()) continue;
        ++out.matched;
        const size_t i = it->second;
        float mine[3] = {static_cast<float>(f.lights.ambient.x), static_cast<float>(f.lights.ambient.y), static_cast<float>(f.lights.ambient.z)};
        for (int c = 0; c < 3; ++c) {
            const float* d = reinterpret_cast<const float*>(r.direct[c].data()) + static_cast<size_t>(slot) * 4;
            for (int ch = 0; ch < 3; ++ch) mine[ch] += d[ch];
        }
        const core::Vec3 refValue = ref.value(i);
        const double refv[3] = {refValue.x, refValue.y, refValue.z};
        for (int ch = 0; ch < 3; ++ch)
            if (refv[ch] > 0.01) {
                relDirect.push_back(std::fabs(mine[ch] - refv[ch]) / refv[ch]);
                sgnDirect.push_back((mine[ch] - refv[ch]) / refv[ch]);
            }
        const float occ = reinterpret_cast<const float*>(r.occlusion.data())[slot];
        const float k = native_test::aoFactorOf(ao, occ);
        const core::Vec3 shown = ref.shownValue(i);
        const double shownv[3] = {shown.x, shown.y, shown.z};
        for (int ch = 0; ch < 3; ++ch)
            if (shownv[ch] > 0.01) relShown.push_back(std::fabs(std::max(0.f, mine[ch] * k) - shownv[ch]) / shownv[ch]);
        const float refOcc = ref.occlusion(i);
        if (ao.active() && !std::isnan(refOcc) && refOcc > 0.01f) relAo.push_back(std::fabs(occ - refOcc));   // absolute: the occluded fraction is already 0..1
    }
    out.direct = summarize(relDirect, &sgnDirect);
    out.shown = summarize(relShown);
    out.ao = summarize(relAo);
    return out;
}

void report(const char* what, const Stat& s) {
    MESSAGE(what << ": " << s.count << " values, mean " << s.mean * 100 << " %, p99 " << s.p99 * 100 << " %, worst " << s.worst * 100 << " %, bias " << s.bias * 100 << " %");
}

} // namespace

TEST_CASE("direct light passes match core::LiveBaker after maxPasses (demo_lit, bounces off)" * doctest::test_suite("gpu")) {
    gpu::DeviceOptions o;
    o.validation = true;
    std::string err;
    auto device = gpu::Device::create(o, &err);
    if (!device || !device->caps().lit()) {
        MESSAGE("no ray tracing device (" << err << "): GPU test skipped");
        return;
    }
    const ParityResult r = compareToCore(*device, "maps/demo_lit.map.json", 0, {}, 32, true, light::DirectParams{});
    if (r.samples == 0) return;
    MESSAGE(r.matched << " of " << r.samples << " samples matched");
    CHECK(r.matched == r.samples);
    report("direct", r.direct);
    CHECK(r.direct.count > 1000);
    CHECK(r.direct.mean < 0.01);
    CHECK(r.direct.p99 < 0.05);
    CHECK(device->validationErrors() == 0);
}

TEST_CASE("ambient occlusion pass matches core::LiveBaker (demo_lit with _ao_radius)" * doctest::test_suite("gpu")) {
    gpu::DeviceOptions o;
    o.validation = true;
    std::string err;
    auto device = gpu::Device::create(o, &err);
    if (!device || !device->caps().lit()) {
        MESSAGE("no ray tracing device (" << err << "): GPU test skipped");
        return;
    }
    const auto edit = [](core::LightScene& l) {
        l.level.ao.radius = 2.5;
        l.level.ao.rays = 128;   // the turn of the stratified directions comes from the random id, which differs: many rays keep that noise small
        l.level.ao.intensity = 0.8;
        l.level.ao.power = 1.5;
    };
    const ParityResult r = compareToCore(*device, "maps/demo_lit.map.json", 0, edit, 32, true, light::DirectParams{});
    if (r.samples == 0) return;
    report("ao", r.ao);
    report("shown", r.shown);
    CHECK(r.ao.count > 1000);
    CHECK(r.ao.mean < 0.01);   // absolute difference of the occluded fraction: same weights, a different turn of the stratified directions
    CHECK(r.ao.p99 < 0.05);
    CHECK(r.shown.mean < 0.01);
    CHECK(r.shown.p99 < 0.05);
    CHECK(device->validationErrors() == 0);
}

TEST_CASE("direct light and occlusion match core::LiveBaker on a generated room set (6 x 6 rooms, point lights and a sun)" * doctest::test_suite("gpu")) {
    gpu::DeviceOptions o;
    o.validation = true;
    std::string err;
    auto device = gpu::Device::create(o, &err);
    if (!device || !device->caps().lit()) {
        MESSAGE("no ray tracing device (" << err << "): GPU test skipped");
        return;
    }
    const auto edit = [](core::LightScene& l) {
        l.ambient = core::Vec3{0.02, 0.02, 0.03};
        l.level.ao.radius = 1.5;
        l.level.ao.rays = 128;
    };
    const ParityResult r = compareToCore(*device, "", 6, edit, 32, true, light::DirectParams{});
    REQUIRE(r.samples > 0);
    MESSAGE(r.matched << " of " << r.samples << " samples matched");
    CHECK(r.matched == r.samples);
    report("direct", r.direct);
    report("ao", r.ao);
    report("shown", r.shown);
    CHECK(r.direct.mean < 0.01);
    CHECK(r.direct.p99 < 0.05);
    CHECK(r.ao.mean < 0.01);
    CHECK(r.shown.mean < 0.01);
    CHECK(r.shown.p99 < 0.05);
    CHECK(device->validationErrors() == 0);
}
