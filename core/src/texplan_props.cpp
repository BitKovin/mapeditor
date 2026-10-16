// Texture properties, reliefs, face repetition overrides and the seamless-edge analysis: ports of shell/src/model/texprops.ts, relief.ts, seamless.ts and
// state/textures.ts analyseSeams (texplan.h).
#include "core/texplan.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace core::texplan {

using nlohmann::json;

namespace {
bool fin(const json& v) { return v.is_number() && std::isfinite(v.get<double>()); }
double clampd(double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }
constexpr double kSplitMin = 0.02, kSplitMax = 0.98, kPpmMin = 1, kPpmMax = 4096;
constexpr int kReliefMaxVertices = 4096, kReliefMaxFaces = 4096, kReliefMaxLoop = 64;
constexpr double kReliefMaxDepth = 8;

AxisRepeat defaultAxis(AnchorSide a) { AxisRepeat r; r.anchor = a; return r; }

AxisRepeat parseAxis(const json& j, AnchorSide anchor, const std::string& self) {
    AxisRepeat d = defaultAxis(anchor);
    if (!j.is_object()) return d;
    const std::string mode = j.value("mode", json()).is_string() ? j["mode"].get<std::string>() : "";
    d.mode = mode == "mirror" ? AxisMode::Mirror : mode == "anchor" ? AxisMode::Anchor : AxisMode::Repeat;
    const std::string side = j.contains("anchor") && j["anchor"].is_string() ? j["anchor"].get<std::string>() : "";
    if (side == "start") d.anchor = AnchorSide::Start;
    else if (side == "end") d.anchor = AnchorSide::End;
    if (j.contains("split") && fin(j["split"])) d.split = clampd(j["split"].get<double>(), kSplitMin, kSplitMax);
    d.fill = j.contains("fill") && j["fill"] == "repeat" ? Fill::Repeat : Fill::Mirror;
    if (j.contains("alt") && j["alt"].is_object()) {
        const json& a = j["alt"];
        const std::string t = a.contains("texture") && a["texture"].is_string() ? a["texture"].get<std::string>() : "";
        V2 off{0, 0};
        if (a.contains("offset") && a["offset"].is_array() && a["offset"].size() == 2 && fin(a["offset"][0]) && fin(a["offset"][1]))
            off = {a["offset"][0].get<double>(), a["offset"][1].get<double>()};
        if (safeTexturePath(t) && t != self) d.alt = AltTexture{t, off};
    }
    const std::string edge = j.contains("edge") && j["edge"].is_string() ? j["edge"].get<std::string>() : "";
    d.edge = edge == "wrap" ? EdgeMode::Wrap : edge == "clamp" ? EdgeMode::Clamp : EdgeMode::Auto;
    return d;
}
}  // namespace

bool safeTexturePath(const std::string& p) {
    if (p.empty() || p.size() > 260) return false;
    if (p[0] == '/' || p[0] == '\\') return false;
    for (unsigned char c : p)
        if (c < 0x20 || c == ':' || c == '\\' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
    size_t start = 0;
    for (;;) {
        const size_t slash = p.find('/', start);
        const std::string seg = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (seg.empty() || seg == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

TextureProps defaultProps() {
    TextureProps p;
    p.u = defaultAxis(AnchorSide::Start);
    p.v = defaultAxis(AnchorSide::End);
    return p;
}

TextureProps parseProps(const json& j, const std::string& self) {
    TextureProps d = defaultProps();
    if (!j.is_object()) return d;
    if (j.contains("pixelsPerMeter") && fin(j["pixelsPerMeter"]) && j["pixelsPerMeter"].get<double>() > 0)
        d.pixelsPerMeter = clampd(j["pixelsPerMeter"].get<double>(), kPpmMin, kPpmMax);
    const json rep = j.contains("repeat") && j["repeat"].is_object() ? j["repeat"] : json::object();
    d.u = parseAxis(rep.contains("u") ? rep["u"] : json(), AnchorSide::Start, self);
    d.v = parseAxis(rep.contains("v") ? rep["v"] : json(), AnchorSide::End, self);
    d.relief = parseRelief(j.contains("relief") ? j["relief"] : json());
    return d;
}

std::shared_ptr<const Relief> parseRelief(const json& j) {
    if (!j.is_object() || !j.contains("vertices") || !j.contains("faces") || !j["vertices"].is_array() || !j["faces"].is_array()) return nullptr;
    std::vector<V3> vertices;
    for (const auto& v : j["vertices"]) {
        if (static_cast<int>(vertices.size()) >= kReliefMaxVertices) break;
        if (!v.is_array() || v.size() != 3 || !fin(v[0]) || !fin(v[1]) || !fin(v[2])) { vertices.push_back({0, 0, 0}); continue; }
        vertices.push_back({clampd(v[0].get<double>(), 0, 1), clampd(v[1].get<double>(), 0, 1), clampd(v[2].get<double>(), -kReliefMaxDepth, kReliefMaxDepth)});
    }
    std::vector<std::vector<int>> faces;
    for (const auto& f : j["faces"]) {
        if (static_cast<int>(faces.size()) >= kReliefMaxFaces) break;
        if (!f.is_array() || f.size() < 3 || f.size() > static_cast<size_t>(kReliefMaxLoop)) continue;
        bool ok = true;
        std::vector<int> loop;
        std::set<int> seen;
        for (const auto& i : f) {
            if (!i.is_number_integer() && !(i.is_number() && std::floor(i.get<double>()) == i.get<double>())) { ok = false; break; }
            const double x = i.get<double>();
            if (x < 0 || x >= static_cast<double>(vertices.size())) { ok = false; break; }
            loop.push_back(static_cast<int>(x));
            seen.insert(static_cast<int>(x));
        }
        if (!ok || seen.size() != loop.size()) continue;
        faces.push_back(loop);
    }
    if (faces.empty()) return nullptr;
    // compact: drop unused vertices, renumber in order of first use
    auto r = std::make_shared<Relief>();
    std::map<int, int> map;
    for (auto& f : faces) {
        std::vector<int> nf;
        for (int i : f) {
            auto it = map.find(i);
            if (it == map.end()) { it = map.emplace(i, static_cast<int>(r->vertices.size())).first; r->vertices.push_back(vertices[static_cast<size_t>(i)]); }
            nf.push_back(it->second);
        }
        r->faces.push_back(nf);
    }
    return r;
}

bool isFlatRelief(const Relief& r) {
    for (const auto& v : r.vertices) if (std::fabs(v[2]) >= 1e-6) return false;
    return true;
}

bool isPlainProps(const TextureProps& p) { return p.u.mode == AxisMode::Repeat && p.v.mode == AxisMode::Repeat; }

bool edgeClamp(const TexInfo& i, int axis) {
    const EdgeMode e = (axis == 0 ? i.props.u : i.props.v).edge;
    return e == EdgeMode::Clamp || (e == EdgeMode::Auto && i.seamless && !(*i.seamless)[static_cast<size_t>(axis)]);
}

double lineDifference(const uint8_t* a, const uint8_t* b, size_t n) {
    if (n == 0) return 0;
    double s = 0;
    for (size_t i = 0; i < n; ++i)
        s += std::abs(int(a[4 * i]) - int(b[4 * i])) + std::abs(int(a[4 * i + 1]) - int(b[4 * i + 1])) + std::abs(int(a[4 * i + 2]) - int(b[4 * i + 2]));
    return s / (3.0 * static_cast<double>(n));
}

std::array<bool, 2> analyseSeams(const uint8_t* rgba, int width, int height) {
    // one line of the axis: u = column x (height pixels), v = row y (width pixels), as RGBA bytes
    auto column = [&](int x) { std::vector<uint8_t> o(static_cast<size_t>(height) * 4); for (int y = 0; y < height; ++y) for (int c = 0; c < 4; ++c) o[static_cast<size_t>(y) * 4 + c] = rgba[(static_cast<size_t>(y) * width + x) * 4 + c]; return o; };
    auto row = [&](int y) { return std::vector<uint8_t>(rgba + static_cast<size_t>(y) * width * 4, rgba + (static_cast<size_t>(y) + 1) * width * 4); };
    auto axis = [&](int len, const std::function<std::vector<uint8_t>(int)>& line) -> bool {
        if (len < 8) return true;
        const int q[3] = {len / 4, len / 2, (3 * len) / 4};
        const auto first = line(0), last = line(len - 1);
        double base = 0;
        for (int i : q) { const auto a = line(i), b = line(i + 1); base += lineDifference(a.data(), b.data(), std::min(a.size(), b.size()) / 4); }
        base /= 3.0;
        const double seam = lineDifference(first.data(), last.data(), std::min(first.size(), last.size()) / 4);
        return !(seam > 1.8 * base + 8);
    };
    return {axis(width, column), axis(height, row)};
}

std::optional<FaceRepeat> parseFaceRepeat(const json& j) {
    if (!j.is_object()) return std::nullopt;
    auto axis = [](const json& a) -> std::optional<FaceRepeatAxis> {
        if (!a.is_object()) return std::nullopt;
        FaceRepeatAxis o;
        if (a.contains("mode") && a["mode"] == "repeat") o.mode = AxisMode::Repeat;
        if (a.contains("mode") && a["mode"] == "mirror") o.mode = AxisMode::Mirror;
        if (a.contains("edge") && a["edge"] == "wrap") o.edge = EdgeMode::Wrap;
        if (a.contains("edge") && a["edge"] == "clamp") o.edge = EdgeMode::Clamp;
        if (!o.mode && !o.edge) return std::nullopt;
        return o;
    };
    FaceRepeat r;
    r.u = axis(j.contains("u") ? j["u"] : json());
    r.v = axis(j.contains("v") ? j["v"] : json());
    if (!r.u && !r.v) return std::nullopt;
    return r;
}

TexInfo withFaceRepeat(const TexInfo& info, const std::optional<FaceRepeat>& over) {
    if (!over) return info;
    TexInfo out = info;
    auto apply = [](AxisRepeat& a, const std::optional<FaceRepeatAxis>& o) {
        if (!o) return;
        if (o->mode) { a.mode = *o->mode; a.alt.reset(); }
        if (o->edge) a.edge = *o->edge;
    };
    apply(out.props.u, over->u);
    apply(out.props.v, over->v);
    return out;
}

}  // namespace core::texplan
