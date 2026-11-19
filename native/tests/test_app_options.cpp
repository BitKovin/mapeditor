// Command line parsing (02_BUILD_AND_TOOLING.md §6).
#include <doctest.h>

#include "app/app_options.h"

using native::app::parseArgs;

TEST_CASE("empty command line gives the defaults") {
    const auto r = parseArgs({});
    CHECK(r.error.empty());
    CHECK(r.options.assetsRoot.empty());
    CHECK(r.options.automationPort == 0);
    CHECK_FALSE(r.options.noLighting);
    CHECK_FALSE(r.options.validation);
    CHECK(r.options.layoutViewports == 0);
}

TEST_CASE("all options are read") {
    const auto r = parseArgs({"--assets", "D:/a", "--map", "maps/x.map.json", "--automation-port", "8123", "--no-lighting",
                              "--validation", "--log", "l.txt", "--trace", "t.json", "--screenshot", "s.png", "--layout", "4"});
    REQUIRE(r.error.empty());
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
    CHECK(parseArgs({"--help"}).options.help);
}
