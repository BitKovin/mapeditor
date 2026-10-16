#include "core/profile.h"
#include <fstream>

using nlohmann::json;
namespace fs = std::filesystem;

namespace core {

json GameProfile::toJson() const {
    json lightStylesJson = json::object();
    for (const auto& [id, pattern] : lightStyles) lightStylesJson[std::to_string(id)] = pattern;
    return json{{"name", name}, {"textureRoot", textureRoot}, {"textureExtensions", textureExtensions},
                {"modelExtensions", modelExtensions}, {"entityDefinitions", entityDefinitions},
                {"mapRoot", mapRoot}, {"prefabRoot", prefabRoot}, {"texGroupRoot", texGroupRoot}, {"pixelsPerMeter", pixelsPerMeter},
                {"lighting", json{{"blockLight", blockLight}, {"receiveBakedLight", receiveBakedLight}, {"lightStyles", lightStylesJson}}},
                {"bsp", bsp.is_object() ? bsp : json::object()}};
}

static std::vector<std::string> extensions(const json& j, const char* key) {
    std::vector<std::string> out;
    if (!j.is_array()) throw std::runtime_error(std::string("game_profile.json: ") + key + " must be an array");
    for (const auto& e : j) {
        if (!e.is_string() || e.get<std::string>().empty() || e.get<std::string>()[0] != '.')
            throw std::runtime_error(std::string("game_profile.json: ") + key + " entries look like \".tga\"");
        out.push_back(e.get<std::string>());
    }
    return out;
}

static std::vector<std::string> classList(const json& j, const char* key) {
    std::vector<std::string> out;
    if (!j.is_array()) throw std::runtime_error(std::string("game_profile.json: lighting.") + key + " must be an array of classnames");
    for (const auto& e : j) {
        if (!e.is_string() || e.get<std::string>().empty())
            throw std::runtime_error(std::string("game_profile.json: lighting.") + key + " must contain non-empty strings");
        out.push_back(e.get<std::string>());
    }
    return out;
}

GameProfile loadProfile(const fs::path& assetsRoot) {
    GameProfile p;
    std::ifstream in(assetsRoot / "game_profile.json", std::ios::binary);
    if (!in) return p;
    json j;
    try {
        j = json::parse(in);
    } catch (const json::parse_error& e) {
        throw std::runtime_error(std::string("game_profile.json is not valid JSON: ") + e.what());
    }
    if (!j.is_object()) throw std::runtime_error("game_profile.json must be an object");
    if (j.contains("name")) p.name = j["name"].get<std::string>();
    // Path-valued fields must stay inside the assets folder.
    auto insidePath = [&](const char* key, std::string& target) {
        if (!j.contains(key)) return;
        if (!j[key].is_string()) throw std::runtime_error(std::string("game_profile.json: ") + key + " must be a string");
        target = j[key].get<std::string>();
        const fs::path r(target);
        if (r.is_absolute() || r.has_root_name() || r.has_root_directory() || r.lexically_normal().generic_string().rfind("..", 0) == 0)
            throw std::runtime_error(std::string("game_profile.json: ") + key + " must stay inside the assets folder");
    };
    insidePath("textureRoot", p.textureRoot);
    insidePath("entityDefinitions", p.entityDefinitions);
    insidePath("mapRoot", p.mapRoot);
    insidePath("prefabRoot", p.prefabRoot);
    insidePath("texGroupRoot", p.texGroupRoot);
    if (j.contains("pixelsPerMeter")) {
        if (!j["pixelsPerMeter"].is_number() || !(j["pixelsPerMeter"].get<double>() >= 1 && j["pixelsPerMeter"].get<double>() <= 4096))
            throw std::runtime_error("game_profile.json: pixelsPerMeter must be a number from 1 to 4096");
        p.pixelsPerMeter = j["pixelsPerMeter"].get<double>();
    }
    if (j.contains("textureExtensions")) p.textureExtensions = extensions(j["textureExtensions"], "textureExtensions");
    if (j.contains("modelExtensions")) p.modelExtensions = extensions(j["modelExtensions"], "modelExtensions");
    if (j.contains("lighting")) {
        if (!j["lighting"].is_object()) throw std::runtime_error("game_profile.json: lighting must be an object");
        const json& l = j["lighting"];
        if (l.contains("blockLight")) p.blockLight = classList(l["blockLight"], "blockLight");
        if (l.contains("receiveBakedLight")) p.receiveBakedLight = classList(l["receiveBakedLight"], "receiveBakedLight");
        if (l.contains("lightStyles")) {
            if (!l["lightStyles"].is_object()) throw std::runtime_error("game_profile.json: lighting.lightStyles must be an object (id -> pattern)");
            for (const auto& [k, v] : l["lightStyles"].items()) {
                int id = -1;
                try { id = std::stoi(k); } catch (...) {}
                if (id < 1 || id > 63 || !v.is_string()) throw std::runtime_error("game_profile.json: lightStyles keys are 1..63 and values letter strings (a..z)");
                const std::string pat = v.get<std::string>();
                for (char c : pat) if (c < 'a' || c > 'z') throw std::runtime_error("game_profile.json: light style " + k + " may only contain the letters a..z");
                p.lightStyles[id] = pat;
            }
        }
    }
    if (j.contains("bsp")) {
        if (!j["bsp"].is_object()) throw std::runtime_error("game_profile.json: bsp must be an object");
        p.bsp = j["bsp"];
    }
    return p;
}

}  // namespace core
