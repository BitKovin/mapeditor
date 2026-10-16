// Command line baker: opens a map, lays out lightmap UVs, bakes the lights and writes one PNG per page (sRGB, clamped,
// debug view only) plus layout.json. Usage:
//   lightbake_tool <assetsRoot> <map.map.json> <outDir> [--cpu] [--validate] [--tpm N] [--atlas N] [--merge DEG] [--bounces N] [--gi-samples N] [--albedo A] [--no-shadows] [--supersample N] [--gi-scale 1|2|4] [--no-denoise] [--denoise-quality 0|1|2]
#include <algorithm>
#include "core/lightbake.h"
#include "core/scene.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace core;

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: lightbake_tool <assetsRoot> <map.map.json> <outDir> [--cpu] [--validate] [--tpm N] [--atlas N] [--merge DEG] [--bounces N] [--gi-samples N] [--albedo A] [--no-shadows] [--supersample N] [--gi-scale 1|2|4] [--no-denoise] [--denoise-quality 0|1|2]\n";
        return 2;
    }
    LightmapSettings cfg;
    BakeOptions opt;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--cpu") opt.forceCpu = true;
        else if (a == "--validate") opt.gpuValidation = true;
        else if (a == "--tpm" && i + 1 < argc) cfg.texelsPerMeter = std::atof(argv[++i]);
        else if (a == "--atlas" && i + 1 < argc) cfg.atlasSize = std::atoi(argv[++i]);
        else if (a == "--merge" && i + 1 < argc) cfg.mergeAngleDeg = std::atof(argv[++i]);
        else if (a == "--bounces" && i + 1 < argc) opt.bounces = std::atoi(argv[++i]);
        else if (a == "--gi-samples" && i + 1 < argc) opt.giSamples = std::atoi(argv[++i]);
        else if (a == "--albedo" && i + 1 < argc) opt.albedo = std::atof(argv[++i]);
        else if (a == "--no-shadows") opt.shadows = false;
        else if (a == "--supersample" && i + 1 < argc) opt.superSamples = std::clamp(std::atoi(argv[++i]), 1, kMaxSuperSamples);
        else if (a == "--gi-scale" && i + 1 < argc) opt.giScale = std::clamp(std::atoi(argv[++i]), 1, 4);
        else if (a == "--no-denoise") opt.denoiseGi = false;
        else if (a == "--denoise-quality" && i + 1 < argc) opt.denoiseQuality = std::clamp(std::atoi(argv[++i]), 0, 2);
        else { std::cerr << "unknown argument " << a << "\n"; return 2; }
    }
    try {
        Scene scene{argv[1]};
        scene.loadGameProfile();
        scene.openDocument(argv[2]);
        const auto surfaces = scene.lightSurfaces();
        const LightmapLayout layout = buildLightmapLayout(surfaces, scene.lightingClasses(), cfg);
        const LightScene lights = scene.lightScene();
        opt.solids = scene.lightSolids();
        opt.classes = scene.lightingClasses();
        BakeStats st;
        const LightmapPages pages = bakeLightmaps(layout, surfaces, lights, opt, &st);
        std::filesystem::create_directories(argv[3]);
        for (int p = 0; p < pages.pageCount; ++p) writePagePng(pages, p, (std::filesystem::path(argv[3]) / ("page" + std::to_string(p) + ".png")).string());
        std::ofstream(std::filesystem::path(argv[3]) / "layout.json") << layout.toJson().dump(1);
        std::cout << "hidden texels skipped " << st.hiddenTexels << ", device " << st.device << (st.usedGpu ? " (gpu)" : "") << ", faces " << layout.faces.size() << ", charts " << layout.charts.size()
                  << ", pages " << pages.pageCount << ", texels " << st.texels << ", lights " << st.lights << ", " << st.seconds << " s\n";
        std::cout << "gi grid " << st.giScale << "x" << (st.denoised ? ", denoised in " + std::to_string(st.denoiseSeconds) + " s" : std::string(", not denoised")) << "\n";
        for (const auto& w : layout.warnings) std::cout << "layout warning: " << w << "\n";
        for (const auto& w : st.warnings) std::cout << "warning: " << w << "\n";
        for (const auto& m : st.validationMessages) std::cout << "validation: " << m << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
