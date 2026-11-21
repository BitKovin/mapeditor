// tools/texturing/tileset_edit -- see tileset_edit.h.
#include "tools/texturing/tileset_edit.h"

#include <core/texplan.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <set>

namespace native::tools::tex {

using ojson = nlohmann::ordered_json;
using json = nlohmann::json;

const char* placeName(Place p) {
    switch (p) {
        case Place::Left: return "left";
        case Place::Right: return "right";
        case Place::Middle: return "middle";
        case Place::Once: return "once";
        default: return "any";
    }
}
Place placeFromName(const std::string& s) {
    if (s == "left") return Place::Left;
    if (s == "right") return Place::Right;
    if (s == "middle") return Place::Middle;
    if (s == "once") return Place::Once;
    return Place::Any;
}
const char* laneName(Lane l) { return l == Lane::Top ? "top" : l == Lane::Middle ? "middle" : "bottom"; }

Kit emptyWallKit() { return Kit{}; }
Kit emptyFrameKit() {
    Kit k;
    k.kind = KitKind::Frame;
    return k;
}

std::vector<std::string> kitTextures(const Kit& k) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    const auto add = [&](const std::optional<std::string>& t) {
        if (t && seen.insert(*t).second) out.push_back(*t);
    };
    if (k.kind == KitKind::Frame) {
        for (const auto& t : k.frame) add(t);
    } else {
        for (Lane l : kLanes)
            for (const KitPiece& p : k.lane(l).pieces)
                for (const auto& row : p.cells)
                    for (const auto& t : row) add(t);
    }
    return out;
}

// ---- reading ------------------------------------------------------------------------------------------------------------------------------
namespace {

bool fin(const json& j) { return j.is_number() && std::isfinite(j.get<double>()); }
std::optional<std::string> safeTex(const json& j) {
    if (!j.is_string() || !core::texplan::safeTexturePath(j.get<std::string>())) return std::nullopt;
    return j.get<std::string>();
}
bool matchOk(const std::string& s) {
    static const std::regex re("^[A-Za-z0-9 _\\-]{1,32}$");
    return std::regex_match(s, re);
}
std::string trimmed(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::optional<KitPiece> parsePiece(json j, std::vector<std::string>& warn, const std::string& at) {
    if (j.is_string()) j = json{{"tex", j}};
    if (!j.is_object()) {
        warn.push_back(at + ": a piece must be a texture or an object");
        return std::nullopt;
    }
    json grid = j.contains("cells") ? j["cells"] : json();
    if (grid.is_null() && j.contains("tex") && j["tex"].is_string()) grid = json::array({json::array({j["tex"]})});
    if (grid.is_null() && j.contains("tex") && j["tex"].is_array()) grid = j["tex"];
    if (!grid.is_array() || grid.empty() || grid.size() > static_cast<size_t>(kMaxPiece)) {
        warn.push_back(at + ": a piece needs \"tex\" or \"cells\" (at most " + std::to_string(kMaxPiece) + " rows)");
        return std::nullopt;
    }
    KitPiece p;
    for (const json& row : grid) {
        const json list = row.is_array() ? row : json::array({row});
        if (list.empty() || list.size() > static_cast<size_t>(kMaxPiece)) {
            warn.push_back(at + ": a row of a piece needs 1 to " + std::to_string(kMaxPiece) + " textures");
            return std::nullopt;
        }
        std::vector<std::optional<std::string>> r;
        for (const json& t : list) {
            std::optional<std::string> safe = t.is_null() ? std::nullopt : safeTex(t);
            if (!t.is_null() && !safe) warn.push_back(at + ": '" + (t.is_string() ? t.get<std::string>() : t.dump()) + "' is not a valid texture path (the piece is kept without it)");
            r.push_back(safe);
        }
        p.cells.push_back(std::move(r));
    }
    for (const auto& r : p.cells)
        if (r.size() != p.cells[0].size()) {
            warn.push_back(at + ": all rows of a piece must have the same number of textures");
            return std::nullopt;
        }
    p.place = j.contains("place") && j["place"].is_string() ? placeFromName(j["place"].get<std::string>()) : Place::Any;
    if (j.contains("match") && !j["match"].is_null()) {
        const std::string m = j["match"].is_string() ? trimmed(j["match"].get<std::string>()) : std::string();
        if (matchOk(m)) p.match = m;
        else warn.push_back(at + ": 'match' must be a short name (letters, digits, space, _ -)");
    }
    p.weight = j.contains("weight") && fin(j["weight"]) && j["weight"].get<double>() > 0 ? std::min(j["weight"].get<double>(), 1000.0) : 1;
    return p;
}

KitLane parseLane(const json& j, std::vector<std::string>& warn, const std::string& at) {
    KitLane lane;
    if (!j.is_object()) return lane;
    if (j.contains("pieces") && j["pieces"].is_array()) {
        int i = 0;
        for (const json& p : j["pieces"]) {
            if (i >= kMaxPieces) break;
            if (auto piece = parsePiece(p, warn, at + ".pieces[" + std::to_string(i) + "]")) lane.pieces.push_back(std::move(*piece));
            ++i;
        }
    }
    int rows = 1;
    for (const KitPiece& p : lane.pieces) rows = std::max(rows, p.rows());
    lane.rows = j.contains("rows") && fin(j["rows"]) ? std::clamp(static_cast<int>(std::lround(j["rows"].get<double>())), 1, kMaxPiece) : rows;
    const size_t before = lane.pieces.size();
    lane.pieces.erase(std::remove_if(lane.pieces.begin(), lane.pieces.end(), [&](const KitPiece& p) { return p.rows() != lane.rows; }), lane.pieces.end());
    if (lane.pieces.size() < before) warn.push_back(at + ": pieces with another height than the lane (" + std::to_string(lane.rows) + " row" + (lane.rows == 1 ? "" : "s") + ") were left out");
    return lane;
}

} // namespace

std::optional<Kit> kitFromJson(const json& j, std::vector<std::string>& warnings) {
    if (!j.is_object()) {
        warnings.push_back("kit: must be an object");
        return std::nullopt;
    }
    Kit k;
    const std::string kind = j.contains("kind") && j["kind"].is_string() ? j["kind"].get<std::string>() : std::string();
    if (kind != "wall" && kind != "frame") {
        warnings.push_back("kit: unknown kind '" + (j.contains("kind") && j["kind"].is_string() ? kind : std::string("undefined")) + "' (wall or frame)");
        return std::nullopt;
    }
    if (j.contains("tile") && j["tile"].is_array() && j["tile"].size() == 2 && fin(j["tile"][0]) && fin(j["tile"][1]) && j["tile"][0].get<double>() > 0.05 && j["tile"][1].get<double>() > 0.05)
        k.tile = std::array<double, 2>{std::min(j["tile"][0].get<double>(), 1000.0), std::min(j["tile"][1].get<double>(), 1000.0)};
    k.maxStretch = j.contains("maxStretch") && fin(j["maxStretch"]) && j["maxStretch"].get<double>() >= 0 ? std::min(j["maxStretch"].get<double>(), 500.0) / 100 : kDefaultMaxStretch;
    if (kind == "frame") {
        k.kind = KitKind::Frame;
        for (int i = 0; i < 9; ++i) {
            const char* s = kFrameSlots[i];
            if (!j.contains(s) || j[s].is_null()) continue;
            k.frame[static_cast<size_t>(i)] = safeTex(j[s]);
            if (!k.frame[static_cast<size_t>(i)]) warnings.push_back(std::string("kit.") + s + ": not a valid texture path");
        }
        return k;
    }
    k.kind = KitKind::Wall;
    k.avoidRepeat = !(j.contains("avoidRepeat") && j["avoidRepeat"].is_boolean() && !j["avoidRepeat"].get<bool>());
    for (Lane l : kLanes) k.lane(l) = parseLane(j.contains(laneName(l)) ? j[laneName(l)] : json(), warnings, std::string("kit.") + laneName(l));
    return k;
}

ojson kitToJson(const Kit& k) {
    ojson out = ojson::object();
    out["kind"] = k.kind == KitKind::Frame ? "frame" : "wall";
    if (k.tile) out["tile"] = ojson::array({(*k.tile)[0], (*k.tile)[1]});
    if (std::abs(k.maxStretch - kDefaultMaxStretch) > 1e-9) out["maxStretch"] = std::round(k.maxStretch * 1000) / 10;
    if (k.kind == KitKind::Frame) {
        for (int i = 0; i < 9; ++i) out[kFrameSlots[i]] = k.frame[static_cast<size_t>(i)] ? ojson(*k.frame[static_cast<size_t>(i)]) : ojson(nullptr);
        return out;
    }
    if (!k.avoidRepeat) out["avoidRepeat"] = false;
    for (Lane l : kLanes) {
        const KitLane& lane = k.lane(l);
        ojson lj = ojson::object();
        if (lane.rows > 1) lj["rows"] = lane.rows;
        ojson pieces = ojson::array();
        for (const KitPiece& p : lane.pieces) {
            ojson pj = ojson::object();
            const auto cell = [](const std::optional<std::string>& t) { return t ? ojson(*t) : ojson(nullptr); };
            if (p.rows() == 1 && p.cols() == 1) {
                pj["tex"] = cell(p.cells[0][0]);
            } else {
                ojson rows = ojson::array();
                for (const auto& r : p.cells) {
                    ojson row = ojson::array();
                    for (const auto& t : r) row.push_back(cell(t));
                    rows.push_back(std::move(row));
                }
                pj["cells"] = std::move(rows);
            }
            if (p.weight != 1) pj["weight"] = p.weight;
            if (p.place != Place::Any) pj["place"] = placeName(p.place);
            if (p.match) pj["match"] = *p.match;
            pieces.push_back(std::move(pj));
        }
        lj["pieces"] = std::move(pieces);
        out[laneName(l)] = std::move(lj);
    }
    return out;
}

ojson tileSetFile(const Kit& k, const std::string& title, const std::string& description) {
    ojson out = ojson::object();
    out["format"] = 1;
    if (!title.empty()) out["name"] = title;
    if (!description.empty()) out["description"] = description;
    out["kit"] = kitToJson(k);
    return out;
}

// ---- first guess from file names -----------------------------------------------------------------------------------------------------------
namespace {

std::vector<std::string> tokens(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
    std::transform(leaf.begin(), leaf.end(), leaf.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::vector<std::string> out;
    std::string cur;
    for (const char c : leaf) {
        if (std::isalnum(static_cast<unsigned char>(c))) cur += c;
        else if (!cur.empty()) out.push_back(std::move(cur)), cur.clear();
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}
bool has(const std::vector<std::string>& t, std::initializer_list<const char*> words) {
    for (const std::string& x : t)
        for (const char* w : words)
            if (x == w) return true;
    return false;
}

} // namespace

Kit suggestWallKit(const std::vector<std::string>& paths) {
    Kit k = emptyWallKit();
    for (const std::string& path : paths) {
        const auto t = tokens(path);
        const bool door = has(t, {"door", "gate", "entrance"}), win = has(t, {"window", "win"});
        const Lane lane = has(t, {"roof", "rooftop", "cap", "cornice", "parapet", "crown", "eave"}) ? Lane::Top : (has(t, {"bottom", "base", "ground", "plinth", "basement"}) || door) ? Lane::Bottom : Lane::Middle;
        // 'window_2' / 'door_1': the number is the style; the same style lines up between rows
        const size_t slash = path.find_last_of('/');
        std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
        std::transform(leaf.begin(), leaf.end(), leaf.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        static const std::regex style("(window|win|door)[_ \\-]?(\\d+)$");
        std::smatch m;
        KitPiece p;
        p.cells = {{path}};
        p.weight = door || win ? 1 : 3;
        p.place = door ? Place::Once : Place::Any;
        if (std::regex_search(leaf, m, style)) p.match = (m[1].str() == "door" ? std::string("door ") : std::string("window ")) + m[2].str();
        k.lane(lane).pieces.push_back(std::move(p));
    }
    // a lane that only has doors / windows still needs something plain to fill with: the middle lane's plain walls also fill the bottom lane
    const auto plainOf = [](const KitLane& l) {
        std::vector<KitPiece> out;
        for (const KitPiece& p : l.pieces)
            if (p.place == Place::Any && p.weight == 3) out.push_back(p);
        return out;
    };
    if (!k.lane(Lane::Bottom).pieces.empty() && plainOf(k.lane(Lane::Bottom)).empty())
        for (const KitPiece& p : plainOf(k.lane(Lane::Middle))) k.lane(Lane::Bottom).pieces.push_back(p);
    return k;
}

// ---- changes --------------------------------------------------------------------------------------------------------------------------------
void fitPieceRows(KitPiece& p, int rows) {
    rows = std::clamp(rows, 1, kMaxPiece);
    while (static_cast<int>(p.cells.size()) < rows) p.cells.push_back(p.cells.back());
    p.cells.resize(static_cast<size_t>(rows));
}

void fitPieceCols(KitPiece& p, int cols) {
    cols = std::clamp(cols, 1, kMaxPiece);
    for (auto& r : p.cells) {
        while (static_cast<int>(r.size()) < cols) r.push_back(r.empty() ? std::optional<std::string>() : r.back());
        r.resize(static_cast<size_t>(cols));
    }
}

void setLaneRows(Kit& k, Lane lane, int rows) {
    KitLane& l = k.lane(lane);
    l.rows = std::clamp(rows, 1, kMaxPiece);
    for (KitPiece& p : l.pieces) fitPieceRows(p, l.rows);
}

int addPiece(Kit& k, Lane lane, const std::optional<std::string>& tex, Place place, double weight) {
    KitLane& l = k.lane(lane);
    if (static_cast<int>(l.pieces.size()) >= kMaxPieces) return -1;
    KitPiece p;
    p.cells = {{tex}};
    p.weight = weight;
    p.place = place;
    fitPieceRows(p, l.rows);
    l.pieces.push_back(std::move(p));
    return static_cast<int>(l.pieces.size()) - 1;
}

void removePiece(Kit& k, Lane lane, int index) {
    auto& v = k.lane(lane).pieces;
    if (index >= 0 && index < static_cast<int>(v.size())) v.erase(v.begin() + index);
}

int duplicatePiece(Kit& k, Lane lane, int index) {
    auto& v = k.lane(lane).pieces;
    if (index < 0 || index >= static_cast<int>(v.size()) || static_cast<int>(v.size()) >= kMaxPieces) return -1;
    const KitPiece copy = v[static_cast<size_t>(index)];
    v.insert(v.begin() + index + 1, copy);
    return index + 1;
}

int movePiece(Kit& k, Lane from, int index, Lane to) {
    auto& src = k.lane(from).pieces;
    if (index < 0 || index >= static_cast<int>(src.size())) return -1;
    if (from == to) return index;
    KitPiece p = src[static_cast<size_t>(index)];
    src.erase(src.begin() + index);
    KitLane& dst = k.lane(to);
    if (dst.pieces.empty()) dst.rows = p.rows();
    else fitPieceRows(p, dst.rows);
    dst.pieces.push_back(std::move(p));
    return static_cast<int>(dst.pieces.size()) - 1;
}

int addTextures(Kit& k, const std::vector<std::string>& paths) {
    Kit s = suggestWallKit(paths);
    int n = 0;
    for (Lane l : kLanes)
        for (KitPiece& p : s.lane(l).pieces) {
            KitLane& dst = k.lane(l);
            if (static_cast<int>(dst.pieces.size()) >= kMaxPieces) continue;
            fitPieceRows(p, dst.pieces.empty() ? 1 : dst.rows);
            if (dst.pieces.empty()) dst.rows = 1;
            dst.pieces.push_back(std::move(p));
            ++n;
        }
    return n;
}

bool setPieceCell(Kit& k, Lane lane, int index, int row, int col, const std::optional<std::string>& tex) {
    auto& v = k.lane(lane).pieces;
    if (index < 0 || index >= static_cast<int>(v.size())) return false;
    KitPiece& p = v[static_cast<size_t>(index)];
    if (row < 0 || row >= p.rows() || col < 0 || col >= p.cols()) return false;
    p.cells[static_cast<size_t>(row)][static_cast<size_t>(col)] = tex;
    return true;
}

} // namespace native::tools::tex
