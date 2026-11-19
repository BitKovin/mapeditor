// T1.1: jobs, hash, log, timing, trace.
#include <doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "base/hash.h"
#include "base/jobs.h"
#include "base/log.h"
#include "base/timing.h"
#include "base/trace.h"

using namespace native::base;

namespace {
std::filesystem::path scratchFile(const char* name) {
    return std::filesystem::temp_directory_path() / (std::string("native_tests_") + name);
}
} // namespace

// ---- hash -------------------------------------------------------------------------------------------------------------

TEST_CASE("hashBytes matches the xxHash64 reference values") {
    CHECK(hashBytes("", 0) == 0xEF46DB3751D8E999ULL);
    CHECK(hashString("abc") == 0x44BC2CF5AD770999ULL);
}

TEST_CASE("hashBytes depends on every byte, the length and the seed") {
    std::vector<unsigned char> buf(100);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<unsigned char>(i * 7 + 3);
    const uint64_t base = hashBytes(buf.data(), buf.size());
    for (size_t i = 0; i < buf.size(); i += 9) {          // covers the 32-byte, 8-byte, 4-byte and 1-byte paths
        auto copy = buf;
        copy[i] ^= 1;
        CHECK(hashBytes(copy.data(), copy.size()) != base);
    }
    CHECK(hashBytes(buf.data(), buf.size() - 1) != base);
    CHECK(hashBytes(buf.data(), buf.size(), 1) != base);
    CHECK(hashBytes(buf.data(), buf.size()) == base);     // deterministic
}

TEST_CASE("hashCombine is order dependent") {
    CHECK(hashCombine(1, 2) != hashCombine(2, 1));
}

TEST_CASE("hashJson ignores object key insertion order") {
    nlohmann::ordered_json a, b;
    a["x"] = 1; a["y"] = "two"; a["z"] = {{"p", 1.5}, {"q", {1, 2, 3}}};
    b["z"] = {{"q", {1, 2, 3}}, {"p", 1.5}}; b["y"] = "two"; b["x"] = 1;
    REQUIRE(a.dump() != b.dump());                        // really different insertion order
    CHECK(hashJson(a) == hashJson(b));

    nlohmann::json c = nlohmann::json::parse(a.dump()), d = nlohmann::json::parse(b.dump());
    CHECK(hashJson(c) == hashJson(d));
    CHECK(hashJson(c) == hashJson(a));                    // json and ordered_json agree
}

TEST_CASE("hashJson separates different values and treats 1 and 1.0 as equal") {
    using nlohmann::json;
    CHECK(hashJson(json{{"a", 1}}) != hashJson(json{{"a", 2}}));
    CHECK(hashJson(json{{"a", 1}}) != hashJson(json{{"b", 1}}));
    CHECK(hashJson(json::array({1, 2})) != hashJson(json::array({2, 1})));
    CHECK(hashJson(json("1")) != hashJson(json(1)));
    CHECK(hashJson(json(1)) == hashJson(json(1.0)));
    CHECK(hashJson(json(0.0)) == hashJson(json(-0.0)));
    CHECK(hashJson(json(0.1)) != hashJson(json(0.2)));
    CHECK(hashJson(json()) != hashJson(json(false)));
    CHECK(hashJson(json::object()) != hashJson(json::array()));
}

// ---- jobs -------------------------------------------------------------------------------------------------------------

TEST_CASE("jobs run in priority order, equal priorities in submission order") {
    JobPool pool(1);
    std::mutex gateMutex;
    std::condition_variable gateCv;
    bool open = false;
    pool.submit([&] {                                    // occupies the only worker until all others are queued
        std::unique_lock<std::mutex> lock(gateMutex);
        gateCv.wait(lock, [&] { return open; });
    }, 100);
    while (pool.pending() != 0) std::this_thread::yield();   // the blocker has been picked up

    std::vector<int> order;
    std::mutex orderMutex;
    auto add = [&](int id, int prio) {
        pool.submit([&, id] { std::lock_guard<std::mutex> l(orderMutex); order.push_back(id); }, prio);
    };
    add(1, 0); add(2, 5); add(3, 0); add(4, 10); add(5, 5);
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        open = true;
    }
    gateCv.notify_all();
    pool.waitIdle();
    CHECK(order == std::vector<int>{4, 2, 5, 1, 3});
}

TEST_CASE("cancelled jobs do not run their body") {
    JobPool pool(1);
    VersionCounter version;
    std::mutex gateMutex;
    std::condition_variable gateCv;
    bool open = false;
    pool.submit([&] {
        std::unique_lock<std::mutex> lock(gateMutex);
        gateCv.wait(lock, [&] { return open; });
    }, 100);
    while (pool.pending() != 0) std::this_thread::yield();

    std::atomic<int> ranOld{0}, ranNew{0}, ranNoToken{0};
    for (int i = 0; i < 5; ++i) pool.submit([&] { ++ranOld; }, 0, version.token());
    version.bump();                                      // the work those five were made for is superseded
    for (int i = 0; i < 3; ++i) pool.submit([&] { ++ranNew; }, 0, version.token());
    pool.submit([&] { ++ranNoToken; });
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        open = true;
    }
    gateCv.notify_all();
    pool.waitIdle();
    CHECK(ranOld == 0);
    CHECK(ranNew == 3);
    CHECK(ranNoToken == 1);
    CHECK(pool.skippedCount() == 5);
}

TEST_CASE("a running job sees its own cancellation") {
    JobPool pool(2);
    VersionCounter version;
    std::atomic<bool> started{false}, sawCancel{false};
    VersionToken token = version.token();
    pool.submit([&] {
        started = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!token.cancelled() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        sawCancel = token.cancelled();
    });
    while (!started) std::this_thread::yield();
    version.bump();
    pool.waitIdle();
    CHECK(sawCancel);
}

TEST_CASE("a throwing job is counted and does not kill the worker") {
    setSink([](LogLevel, const std::string&) {});        // keep the test output clean
    JobPool pool(1);
    std::atomic<int> after{0};
    pool.submit([] { throw std::runtime_error("boom"); });
    pool.submit([&] { ++after; });
    pool.waitIdle();
    setSink({});
    CHECK(pool.failedCount() == 1);
    CHECK(after == 1);
}

TEST_CASE("many jobs on many workers all run exactly once") {
    JobPool pool(0);
    CHECK(pool.threadCount() >= 1);
    std::atomic<int> sum{0};
    for (int i = 1; i <= 1000; ++i) pool.submit([&sum, i] { sum += i; }, i % 3);
    pool.waitIdle();
    CHECK(sum == 1000 * 1001 / 2);
}

// ---- log --------------------------------------------------------------------------------------------------------------

TEST_CASE("log lines carry a timestamp and level, and warnings / errors are counted") {
    std::vector<std::string> lines;
    setSink([&](LogLevel, const std::string& line) { lines.push_back(line); });
    resetLogCounts();
    logInfo("hello");
    logWarning("careful");
    logError("broken");
    const LogCounts c = logCounts();
    setSink({});
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].find(" I hello") != std::string::npos);
    CHECK(lines[1].find(" W careful") != std::string::npos);
    CHECK(lines[2].find(" E broken") != std::string::npos);
    CHECK(lines[0][0] == '[');
    CHECK(c.warnings == 1);
    CHECK(c.errors == 1);
}

TEST_CASE("log writes to the chosen file") {
    const auto path = scratchFile("log.txt");
    std::filesystem::remove(path);
    REQUIRE(setLogFile(path.string()));
    logInfo("to the file");
    std::string text;
    {
        std::ifstream in(path);
        std::stringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }
    CHECK(text.find("to the file") != std::string::npos);
    setLogFile((std::filesystem::temp_directory_path() / "native_tests_log_after.txt").string());   // release the first file
    std::filesystem::remove(path);
}

// ---- timing -----------------------------------------------------------------------------------------------------------

TEST_CASE("stopwatch measures elapsed time and nowMicros is monotonic") {
    const int64_t a = nowMicros();
    Stopwatch sw;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(sw.elapsedMs() >= 15.0);
    CHECK(sw.elapsedMs() < 1000.0);
    CHECK(nowMicros() > a);
}

// ---- trace ------------------------------------------------------------------------------------------------------------

TEST_CASE("trace writes valid Chrome trace JSON with events from several threads") {
    const auto path = scratchFile("trace.json");
    std::filesystem::remove(path);
    CHECK_FALSE(traceRunning());
    { TraceScope ignored("not recorded"); }              // no trace running: nothing happens
    REQUIRE(traceStart(path.string()));
    CHECK(traceRunning());
    traceSetThreadName("main");
    {
        TraceScope s("outer", "test");
        std::thread t([] {
            traceSetThreadName("helper");
            TraceScope inner("inner", "test");
            traceCounter("pending", 7);
        });
        t.join();
    }
    traceStop();
    CHECK_FALSE(traceRunning());

    nlohmann::json j;
    {
        std::ifstream in(path);
        j = nlohmann::json::parse(in);                    // throws (fails the test) when the file is not valid JSON
    }
    REQUIRE(j.contains("traceEvents"));
    int complete = 0, counters = 0, meta = 0;
    unsigned tidOuter = 0, tidInner = 0;
    for (const auto& e : j["traceEvents"]) {
        const std::string ph = e["ph"];
        if (ph == "X") {
            ++complete;
            CHECK(e["dur"].get<long long>() >= 0);
            if (e["name"] == "outer") tidOuter = e["tid"];
            if (e["name"] == "inner") tidInner = e["tid"];
        }
        if (ph == "C") ++counters;
        if (ph == "M") ++meta;
    }
    CHECK(complete == 2);
    CHECK(counters == 1);
    CHECK(meta == 2);
    CHECK(tidOuter != tidInner);
    std::filesystem::remove(path);
}
