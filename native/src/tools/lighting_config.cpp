// tools/lighting_config -- see lighting_config.h.
#include "tools/lighting_config.h"

#include <core/lighting_service.h>

#include <fstream>
#include <sstream>

namespace native::tools {

namespace fs = std::filesystem;
using json = nlohmann::json;

const std::vector<LightingGroup>& lightingGroups() {
    static const std::vector<LightingGroup> groups = {
        {"Lightmaps", {{"texelsPerMeter", "Texels per metre", "Lightmap resolution"}, {"atlasSize", "Atlas size (px)", "Size of one lightmap page (a power of two)"}, {"padding", "Padding (texels)", ""},
                       {"mergeAngleDeg", "Smoothing angle (°)", "Faces meeting at less than this angle share their lighting"}}},
        {"Quality", {{"maxPasses", "Passes", "Refinement passes per texel"}, {"directSupersample", "Direct supersampling", ""}, {"areaSamplesPerPass", "Area light samples / pass", ""},
                     {"skySamplesPerPass", "Sky samples / pass", ""}, {"giSamplesPerPass", "Bounce samples / pass", ""}, {"giScale", "Bounce grid scale", "1, 2 or 4: coarser bounce lighting is faster"},
                     {"bounces", "Bounce lighting", ""}, {"albedo", "Default albedo", "Reflectance of surfaces without a texture"}, {"indirectWindow", "Indirect window", ""}}},
        {"Denoising", {{"denoise", "Denoise bounce light", "Needs Intel Open Image Denoise next to the editor"}, {"denoiseQuality", "Denoise quality", ""}}},
        {"Light volumes", {{"lightvol", "Bake light volumes", "Lighting for brush entities and models"}, {"lightvolGridSize", "Grid size (m)", ""}, {"lightvolMaxProbes", "Max probes", "0 = no limit"},
                           {"lightvolNormals", "Directions per probe", ""}, {"lightvolGiSamples", "Bounce samples", ""}, {"lightvolProbesPerStep", "Probes per step", ""}}},
        {"Live view", {{"exposure", "Exposure", ""}, {"defaultSourceSize", "Default light size (m)", "Lights with source_size 0 are shown as spheres of this radius"}, {"defaultSunSize", "Default sun size (°)", ""},
                       {"firstSendFraction", "First result after (fraction)", ""}, {"maxHoldSeconds", "Max hold (s)", ""}, {"flushIntervalMs", "Tile send interval (ms)", ""}}},
        {"Performance", {{"useGpu", "Use the GPU", "Vulkan ray tracing when available"}, {"useTextures", "Coloured bounces from textures", ""}, {"gpuTexelsPerStep", "GPU texels per step", ""},
                         {"gpuTargetStepMs", "GPU step target (ms)", ""}, {"gpuMaxStepScale", "GPU max step scale", ""}, {"texelsPerStep", "CPU texels per step", ""},
                         {"tickBudgetMs", "Tick budget (ms)", ""}, {"chunkTexels", "Chunk size (texels)", ""}}},
    };
    return groups;
}

namespace {

json readObject(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return json::object();
    std::stringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    return j.is_object() ? j : json::object();
}

} // namespace

LightingConfig readLightingConfig(const fs::path& file) {
    LightingConfig c;
    c.file = file.generic_string();
    std::string warning;
    c.values = core::LightingService::optionsJson(core::LightingService::loadOptions(file, &warning));
    // what a key that is not in the file means: the editor's defaults of loadOptions (a file that is not there), not the bare Options{}
    c.defaults = core::LightingService::optionsJson(core::LightingService::loadOptions(fs::path("?no such lighting file?")));
    c.warning = warning;
    return c;
}

std::string writeLightingConfig(const fs::path& file, const json& changes) {
    if (!changes.is_object()) return "the changes must be an object";
    const json known = core::LightingService::optionsJson(core::LightingService::Options{});
    for (const auto& [k, v] : changes.items()) {
        if (!known.contains(k)) return "unknown lighting setting: " + k;
        if (known[k].is_boolean() ? !v.is_boolean() : !v.is_number()) return "wrong kind of value for " + k;
    }
    json merged = readObject(file);
    for (const auto& [k, v] : changes.items()) merged[k] = v;
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = fs::path(file).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return "cannot write " + file.generic_string();
        out << merged.dump(2) << "\n";
        if (!out) return "cannot write " + file.generic_string();
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(file, ec);
        std::error_code ec2;
        fs::rename(tmp, file, ec2);
        if (ec2) return "cannot write " + file.generic_string();
    }
    return {};
}

} // namespace native::tools
