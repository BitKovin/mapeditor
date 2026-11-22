// Command line parsing (02_BUILD_AND_TOOLING.md §6).
#include <doctest.h>

#include <string>

#include "app/app_options.h"

using native::app::parseArgs;

TEST_CASE("empty command line gives the defaults") {
    const auto r = parseArgs({});
    CHECK(r.error.empty());
    CHECK(r.options.assetsRoot.empty());
    CHECK(r.options.automationPort == 0);
    CHECK_FALSE(r.options.noLighting);
    CHECK_FALSE(r.options.validation);
    CHECK_FALSE(r.options.noStreamline);
    CHECK_FALSE(r.options.gpuCrashDumps);
    CHECK(r.options.layoutViewports == 0);
}

TEST_CASE("all options are read") {
    const auto r = parseArgs({"--assets", "D:/a", "--map", "maps/x.map.json", "--automation-port", "8123", "--no-lighting",
                              "--validation", "--log", "l.txt", "--trace", "t.json", "--screenshot", "s.png", "--layout", "4",
                              "--no-streamline", "--gpu-crash-dumps", "--dlss", "dlaa"});
    REQUIRE(r.error.empty());
    CHECK(r.options.dlss == "dlaa");
    CHECK(r.options.noStreamline);
    CHECK(r.options.gpuCrashDumps);
    CHECK(r.options.assetsRoot == "D:/a");
    CHECK(r.options.map == "maps/x.map.json");
    CHECK(r.options.automationPort == 8123);
    CHECK(r.options.noLighting);
    CHECK(r.options.validation);
    CHECK(r.options.logFile == "l.txt");
    CHECK(r.options.traceFile == "t.json");
    CHECK(r.options.screenshotPath == "s.png");
    CHECK(r.options.layoutViewports == 4);
}

TEST_CASE("bad command lines report an error") {
    CHECK_FALSE(parseArgs({"--bogus"}).error.empty());
    CHECK_FALSE(parseArgs({"--assets"}).error.empty());
    CHECK_FALSE(parseArgs({"--automation-port", "abc"}).error.empty());
    CHECK_FALSE(parseArgs({"--automation-port", "70000"}).error.empty());
    CHECK_FALSE(parseArgs({"--layout", "3"}).error.empty());
    CHECK_FALSE(parseArgs({"--layout"}).error.empty());
    CHECK_FALSE(parseArgs({"--dlss"}).error.empty());
    CHECK(parseArgs({"--dlss", "balanced"}).error.empty());
    CHECK(parseArgs({"--dlss", "ultraPerformance"}).error.empty());
    CHECK_FALSE(parseArgs({"--dlss", "turbo"}).error.empty());
    CHECK(parseArgs({}).options.rtBounces);
    CHECK_FALSE(parseArgs({"--rt-bounces", "0"}).options.rtBounces);
    CHECK_FALSE(parseArgs({"--rt-bounces", "2"}).error.empty());
    CHECK(parseArgs({"--dlss", "quality"}).error.empty());
    CHECK(parseArgs({}).options.dlss.empty());
    CHECK(parseArgs({"--help"}).options.help);
}

TEST_CASE("the ray traced Lit path: on by default on a capable GPU, --no-rt-lit and --rt-lit are read") {
    CHECK_FALSE(parseArgs({}).options.noRtLit);
    CHECK_FALSE(parseArgs({}).options.rtLit);
    CHECK(parseArgs({"--no-rt-lit"}).options.noRtLit);
    CHECK(parseArgs({"--rt-lit"}).options.rtLit);
}

TEST_CASE("--upscaler and --no-fsr (T15.1)") {
    CHECK(parseArgs({}).options.upscaler.empty());
    CHECK_FALSE(parseArgs({}).options.noFsr);
    CHECK(parseArgs({"--no-fsr"}).options.noFsr);
    for (const char* v : {"auto", "dlss", "fsr", "off", "FSR"}) {
        const auto r = parseArgs({"--upscaler", v});
        CHECK(r.error.empty());
        CHECK(r.options.upscaler == v);
    }
    CHECK_FALSE(parseArgs({"--upscaler", "xess"}).error.empty());
    CHECK_FALSE(parseArgs({"--upscaler"}).error.empty());
    CHECK(std::string(native::app::usageText()).find("--upscaler") != std::string::npos);
}
