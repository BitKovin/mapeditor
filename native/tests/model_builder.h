// Test helper: builds the bytes of small glTF files.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace modeltest {

using nlohmann::json;

// Builds the binary buffer of a test glTF: every add* appends a bufferView + accessor and returns the accessor index.
struct Builder {
    std::vector<uint8_t> bin;
    json views = json::array(), accessors = json::array();

    int raw(const void* data, size_t bytes, int target = 0) {
        while (bin.size() % 4) bin.push_back(0);
        json v = {{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", bytes}};
        if (target) v["target"] = target;
        bin.insert(bin.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
        views.push_back(v);
        return static_cast<int>(views.size()) - 1;
    }
    int floats(const std::vector<float>& v, const char* type, bool minmax = false) {
        const int view = raw(v.data(), v.size() * 4);
        const size_t w = std::string(type) == "SCALAR" ? 1 : std::string(type) == "VEC2" ? 2 : std::string(type) == "VEC3" ? 3 : std::string(type) == "VEC4" ? 4 : 16;
        json a = {{"bufferView", view}, {"componentType", 5126}, {"count", v.size() / w}, {"type", type}};
        (void)minmax;
        accessors.push_back(a);
        return static_cast<int>(accessors.size()) - 1;
    }
    int u16(const std::vector<uint16_t>& v, const char* type = "SCALAR") {
        const int view = raw(v.data(), v.size() * 2);
        const size_t w = std::string(type) == "VEC4" ? 4 : 1;
        accessors.push_back({{"bufferView", view}, {"componentType", 5123}, {"count", v.size() / w}, {"type", type}});
        return static_cast<int>(accessors.size()) - 1;
    }
    int u8x4(const std::vector<uint8_t>& v) {   // JOINTS_0 as unsigned bytes
        const int view = raw(v.data(), v.size());
        accessors.push_back({{"bufferView", view}, {"componentType", 5121}, {"count", v.size() / 4}, {"type", "VEC4"}});
        return static_cast<int>(accessors.size()) - 1;
    }
    json document(json extra) const {
        json g = {{"asset", {{"version", "2.0"}}}, {"buffers", json::array({json{{"byteLength", bin.size()}}})}, {"bufferViews", views}, {"accessors", accessors}};
        for (auto it = extra.begin(); it != extra.end(); ++it) g[it.key()] = it.value();
        return g;
    }
    std::vector<uint8_t> glb(const json& g) const {
        std::string js = g.dump();
        while (js.size() % 4) js.push_back(' ');
        std::vector<uint8_t> b = bin;
        while (b.size() % 4) b.push_back(0);
        const auto put = [](std::vector<uint8_t>& o, uint32_t v) {
            for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i)));
        };
        std::vector<uint8_t> out;
        put(out, 0x46546c67);
        put(out, 2);
        put(out, static_cast<uint32_t>(12 + 8 + js.size() + 8 + b.size()));
        put(out, static_cast<uint32_t>(js.size()));
        put(out, 0x4e4f534a);
        out.insert(out.end(), js.begin(), js.end());
        put(out, static_cast<uint32_t>(b.size()));
        put(out, 0x004e4942);
        out.insert(out.end(), b.begin(), b.end());
        return out;
    }
};

} // namespace modeltest
