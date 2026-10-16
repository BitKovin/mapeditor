// Texture groups: ports of shell/src/model/rng.ts, texgroup.ts (reader, parameters, face layout), tileset.ts (simple sets: reader and solver),
// layout.ts (rule solver) and layoutPlan.ts (frame, cell map, layout plan) (texplan.h).
#include "core/texplan.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <variant>

namespace core::texplan {

using ojson = nlohmann::ordered_json;
using nlohmann::json;

// ---- rng.ts -------------------------------------------------------------------------------------------------------------------------------------------
namespace {
// JavaScript strings are UTF-16: charCodeAt reads code units
std::vector<uint16_t> utf16(const std::string& s) {
    std::vector<uint16_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6 && i + 1 < s.size()) { cp = ((c & 0x1fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3fu); n = 2; }
        else if ((c >> 4) == 14 && i + 2 < s.size()) { cp = ((c & 0x0fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3fu) << 6) | (static_cast<unsigned char>(s[i + 2]) & 0x3fu); n = 3; }
        else if ((c >> 3) == 30 && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3fu) << 12) | ((static_cast<unsigned char>(s[i + 2]) & 0x3fu) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3fu);
            n = 4;
        } else { cp = 0xfffd; n = 1; }
        if (cp >= 0x10000) { cp -= 0x10000; out.push_back(static_cast<uint16_t>(0xd800 + (cp >> 10))); out.push_back(static_cast<uint16_t>(0xdc00 + (cp & 0x3ff))); }
        else out.push_back(static_cast<uint16_t>(cp));
        i += static_cast<size_t>(n);
    }
    return out;
}
}  // namespace

uint32_t hash32(const std::string& s) {
    uint32_t h = 0x811c9dc5u;
    for (uint16_t c : utf16(s)) { h ^= c; h *= 0x01000193u; }
    h ^= h >> 16; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16;
    return h;
}
double rand01(uint32_t seed, const std::string& key) { return hash32(std::to_string(seed) + "|" + key) / 4294967296.0; }
uint32_t faceSeed(const std::string& brushId, uint32_t faceId) { return hash32(brushId + "/" + std::to_string(faceId)); }

// ---- texgroup.ts types --------------------------------------------------------------------------------------------------------------------------------
namespace {
constexpr int kMaxDepth = 24, kMaxNodes = 4000, kMaxList = 64, kMaxSlots = 64, kMaxParams = 32, kMaxRules = 64;
constexpr double kInf = std::numeric_limits<double>::infinity();

struct Val { enum class T { Num, Str, Bool, Param, Var } t = T::Num; double n = 0; std::string s; bool b = false; };
struct Cond;
using CondPtr = std::shared_ptr<const Cond>;
struct Cond {
    enum class K { Const, All, Any, Not, Truthy, Cmp } k = K::Const;
    bool value = false;
    std::vector<CondPtr> items;
    std::string v, op;
    std::vector<Val> vals;  // one for plain ops, the list for in / mod
    bool list = false;
};
struct SizeSpec { enum class K { Fixed, Param, Frac, Natural, Flex } k = K::Fixed; double m = 0, mul = 1, f = 0, weight = 1, min = 0, max = kInf; std::string name; };
struct Node;
using NodePtr = std::shared_ptr<const Node>;
struct RepeatSpec { SizeSpec size; std::string fit = "round"; int min = 1, max = 256; std::string as; };
struct Part { SizeSpec size; NodePtr node; CondPtr when; bool optional = false; int priority = 0; std::optional<RepeatSpec> repeat; };
struct Option { CondPtr when; double weight = 1; NodePtr node; };
struct Node {
    enum class K { Tex, Split, Choice, Ref } k = K::Tex;
    // tex
    std::string slot, fit = "tile", origin = "face", alignU = "start", alignV = "start";
    bool flipU = false, flipV = false;
    int rotate = 0;
    // split
    std::string axis, leftover = "stretch";
    std::vector<Part> parts;
    NodePtr fill;
    // choice
    std::string mode = "first";
    std::optional<std::string> by;
    std::vector<Option> options;
    // ref
    std::string name;
};
struct ParamDef { std::string type; double dnum = 0, min = 0, max = 0, step = 1; bool dbool = false; std::string dstr; std::vector<std::string> options; };

// ---- tileset.ts types
struct KitPiece { std::vector<std::vector<std::optional<std::string>>> cells; double weight = 1; std::string place = "any"; std::optional<std::string> match; };
struct KitLane { int rows = 1; std::vector<KitPiece> pieces; };
struct Kit {
    std::string kind;
    std::optional<V2> tile;
    double maxStretch = 0.25;
    bool avoidRepeat = true;
    KitLane top, middle, bottom;
    std::map<std::string, std::optional<std::string>> frame;  // tl t tr l c r bl b br
};
const char* const kFrameSlots[9] = {"tl", "t", "tr", "l", "c", "r", "bl", "b", "br"};
}  // namespace

struct TexGroup {
    std::string name;
    std::vector<std::pair<std::string, std::string>> slots;  // insertion order
    std::optional<std::string> fallback;
    std::map<std::string, NodePtr> rules;
    std::map<std::string, ParamDef> params;
    NodePtr root;
    std::optional<Kit> kit;
    const std::string* slot(const std::string& n) const { for (const auto& [k, v] : slots) if (k == n) return &v; return nullptr; }
};

namespace {
bool fin(const ojson& v) { return v.is_number() && std::isfinite(v.get<double>()); }
double clampd(double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }
double jsRound(double v) { return std::floor(v + 0.5); }  // Math.round: halves go up
bool isName(const ojson& j) { static const std::regex re("^[A-Za-z_][A-Za-z0-9_]{0,47}$"); return j.is_string() && std::regex_match(j.get<std::string>(), re); }
bool isNameS(const std::string& s) { static const std::regex re("^[A-Za-z_][A-Za-z0-9_]{0,47}$"); return std::regex_match(s, re); }
bool isVarName(const ojson& j) { static const std::regex re("^[A-Za-z_][A-Za-z0-9_]{0,47}(\\.[A-Za-z_][A-Za-z0-9_]{0,47})?$"); return j.is_string() && std::regex_match(j.get<std::string>(), re); }
std::string str(const ojson& j) { return j.is_string() ? j.get<std::string>() : std::string(); }
const ojson& at(const ojson& o, const char* k) { static const ojson none; return o.is_object() && o.contains(k) ? o[k] : none; }
bool has(const ojson& o, const char* k) { return o.is_object() && o.contains(k); }

struct Reader { int nodes = 0; std::vector<std::string> warnings; void warn(const std::string& where, const std::string& what) { if (warnings.size() < 200) warnings.push_back(where + ": " + what); } };

std::optional<Val> parseVal(const ojson& j) {
    Val v;
    if (fin(j)) { v.t = Val::T::Num; v.n = j.get<double>(); return v; }
    if (j.is_boolean()) { v.t = Val::T::Bool; v.b = j.get<bool>(); return v; }
    if (j.is_string()) { v.t = Val::T::Str; v.s = j.get<std::string>(); return v; }
    if (j.is_object() && isName(at(j, "param"))) { v.t = Val::T::Param; v.s = str(j["param"]); return v; }
    if (j.is_object() && isVarName(at(j, "var"))) { v.t = Val::T::Var; v.s = str(j["var"]); return v; }
    return std::nullopt;
}

CondPtr parseCond(const ojson& j, Reader& r, const std::string& where, int depth = 0) {
    if (depth > kMaxDepth) { r.warn(where, "condition nested too deep"); return nullptr; }
    auto c = std::make_shared<Cond>();
    if (j.is_boolean()) { c->k = Cond::K::Const; c->value = j.get<bool>(); return c; }
    if (j.is_string()) { if (isVarName(j)) { c->k = Cond::K::Truthy; c->v = str(j); return c; } r.warn(where, "'" + str(j) + "' is not a variable name"); return nullptr; }
    if (!j.is_object()) { r.warn(where, "a condition must be true / false, a variable name or an object"); return nullptr; }
    if (at(j, "all").is_array() || at(j, "any").is_array()) {
        const std::string key = at(j, "all").is_array() ? "all" : "any";
        c->k = key == "all" ? Cond::K::All : Cond::K::Any;
        int i = 0;
        for (const auto& x : j[key]) {
            if (i >= kMaxList) break;
            auto item = parseCond(x, r, where + "." + key + "[" + std::to_string(i) + "]", depth + 1);
            if (!item) return nullptr;
            c->items.push_back(item);
            ++i;
        }
        return c;
    }
    if (has(j, "not")) { auto it = parseCond(j["not"], r, where + ".not", depth + 1); if (!it) return nullptr; c->k = Cond::K::Not; c->items.push_back(it); return c; }
    if (isVarName(at(j, "var"))) {
        static const char* ops[] = {"==", "!=", "<", "<=", ">", ">=", "in", "mod"};
        std::vector<std::string> found;
        for (const char* o : ops) if (j.contains(o)) found.push_back(o);
        c->v = str(j["var"]);
        if (found.empty()) { c->k = Cond::K::Truthy; return c; }
        if (found.size() > 1) {
            std::string l;
            for (size_t i = 0; i < found.size(); ++i) l += (i ? ", " : "") + found[i];
            r.warn(where, "one comparison per condition (found " + l + ")");
            return nullptr;
        }
        const std::string op = found[0];
        const ojson& raw = j[op];
        c->k = Cond::K::Cmp;
        c->op = op;
        if (op == "in" || op == "mod") {
            if (!raw.is_array() || raw.empty() || raw.size() > static_cast<size_t>(kMaxList)) { r.warn(where, "'" + op + "' needs a list"); return nullptr; }
            for (const auto& x : raw) { auto v = parseVal(x); if (!v) { r.warn(where, "'" + op + "' list has an unusable value"); return nullptr; } c->vals.push_back(*v); }
            if (op == "mod" && c->vals.size() != 2) { r.warn(where, "'mod' needs [m, r]"); return nullptr; }
            c->list = true;
            return c;
        }
        auto v = parseVal(raw);
        if (!v) { r.warn(where, "'" + op + "' has an unusable value"); return nullptr; }
        c->vals.push_back(*v);
        return c;
    }
    r.warn(where, "unknown condition (use { var, op: value }, all, any, not, a variable name or true / false)");
    return nullptr;
}

double jsNumber(const std::string& s) {  // Number() of a string matched by the size regexes ("2", ".5", "3.")
    if (s.empty() || s == ".") return std::numeric_limits<double>::quiet_NaN();
    return std::stod(s[0] == '.' ? "0" + s : s);
}

SizeSpec parseSize(const ojson& j, Reader& r, const std::string& where, const SizeSpec& dflt) {
    if (j.is_null()) return dflt;
    SizeSpec s;
    if (fin(j)) { if (j.get<double>() > 0) { s.k = SizeSpec::K::Fixed; s.m = std::min(j.get<double>(), 10000.0); return s; } r.warn(where, "a size must be positive"); return dflt; }
    if (j.is_string()) {
        std::string t = j.get<std::string>();
        t.erase(0, t.find_first_not_of(" \t\n\r"));
        t.erase(t.find_last_not_of(" \t\n\r") + 1);
        static const std::regex flex("^(\\d*\\.?\\d*)\\*$"), pct("^(\\d*\\.?\\d+)%$");
        std::smatch m;
        if (std::regex_match(t, m, flex)) { s.k = SizeSpec::K::Flex; s.weight = m[1].str().empty() ? 1 : std::max(1e-6, jsNumber(m[1].str())); return s; }
        if (std::regex_match(t, m, pct)) { s.k = SizeSpec::K::Frac; s.f = clampd(jsNumber(m[1].str()) / 100, 0, 1); return s; }
        r.warn(where, "size '" + j.get<std::string>() + "' not understood (use 3, \"*\", \"2*\" or \"25%\")");
        return dflt;
    }
    if (j.is_object()) {
        if (fin(at(j, "flex")) || j.contains("min") || j.contains("max")) {
            s.k = SizeSpec::K::Flex;
            s.weight = fin(at(j, "flex")) && j["flex"].get<double>() > 0 ? j["flex"].get<double>() : 1;
            s.min = fin(at(j, "min")) && j["min"].get<double>() > 0 ? j["min"].get<double>() : 0;
            s.max = fin(at(j, "max")) && j["max"].get<double>() > 0 ? std::max(j["max"].get<double>(), s.min) : kInf;
            return s;
        }
        if (fin(at(j, "frac"))) { s.k = SizeSpec::K::Frac; s.f = clampd(j["frac"].get<double>(), 0, 1); return s; }
        if (isName(at(j, "param"))) { s.k = SizeSpec::K::Param; s.name = str(j["param"]); s.mul = fin(at(j, "mul")) ? j["mul"].get<double>() : 1; return s; }
        if (isName(at(j, "natural"))) { s.k = SizeSpec::K::Natural; s.name = str(j["natural"]); return s; }
    }
    r.warn(where, "size not understood");
    return dflt;
}

SizeSpec flex1() { SizeSpec s; s.k = SizeSpec::K::Flex; return s; }

NodePtr parseTex(const ojson& o, Reader& r, const std::string& where) {
    if (!isName(at(o, "tex"))) { r.warn(where, "a texture part needs a slot name in \"tex\""); return nullptr; }
    auto n = std::make_shared<Node>();
    n->k = Node::K::Tex;
    n->slot = str(o["tex"]);
    const std::string fit = str(at(o, "fit"));
    n->fit = fit == "stretch" || fit == "width" || fit == "height" ? fit : "tile";
    if (has(o, "fit") && !o["fit"].is_null() && fit != n->fit && fit != "tile") r.warn(where, "unknown fit '" + (o["fit"].is_string() ? fit : o["fit"].dump()) + "', using tile");
    auto al = [&](const char* k) { const std::string v = str(at(o, k)); return v == "center" || v == "end" ? v : std::string("start"); };
    n->alignU = al("alignU");
    n->alignV = al("alignV");
    n->flipU = at(o, "flipU").is_boolean() && o["flipU"].get<bool>();
    n->flipV = at(o, "flipV").is_boolean() && o["flipV"].get<bool>();
    const ojson& rot = at(o, "rotate");
    n->rotate = rot.is_number() && (rot.get<double>() == 90 || rot.get<double>() == 180 || rot.get<double>() == 270) ? static_cast<int>(rot.get<double>()) : 0;
    const std::string origin = str(at(o, "origin"));
    n->origin = origin == "cell" ? "cell" : origin == "face" ? "face" : n->fit == "tile" ? "face" : "cell";
    return n;
}

NodePtr parseNode(const ojson& j, Reader& r, const std::string& where, int depth);

std::optional<Part> parseSplitPart(const ojson& jj, Reader& r, const std::string& where, int depth) {
    ojson o = jj.is_string() ? ojson{{"tex", jj}} : jj;
    if (!o.is_object()) { r.warn(where, "a part must be an object"); return std::nullopt; }
    NodePtr node = parseNode(has(o, "node") ? o["node"] : o, r, where, depth);
    if (!node) return std::nullopt;
    Part p;
    p.node = node;
    const ojson rep = at(o, "repeat");
    if (!rep.is_null() && !(rep.is_boolean() && !rep.get<bool>())) {
        const ojson rr = rep.is_boolean() ? ojson::object() : rep;
        if (!rr.is_object()) r.warn(where, "repeat must be an object");
        else {
            SizeSpec one; one.k = SizeSpec::K::Fixed; one.m = 1;
            RepeatSpec rs;
            const SizeSpec size = parseSize(at(rr, "size"), r, where + ".repeat.size", one);
            if (size.k == SizeSpec::K::Flex || size.k == SizeSpec::K::Frac) r.warn(where, "a repeat size must be metres, a parameter or a natural size; using 1 m");
            rs.size = size.k == SizeSpec::K::Flex || size.k == SizeSpec::K::Frac ? one : size;
            const std::string fit = str(at(rr, "fit"));
            rs.fit = fit == "floor" || fit == "ceil" ? fit : "round";
            rs.min = fin(at(rr, "min")) ? static_cast<int>(clampd(std::floor(rr["min"].get<double>()), 0, 4096)) : 1;
            rs.max = fin(at(rr, "max")) ? static_cast<int>(clampd(std::floor(rr["max"].get<double>()), 1, 4096)) : 256;
            rs.as = isName(at(rr, "as")) ? str(rr["as"]) : "";
            if (rs.max < rs.min) rs.max = rs.min;
            p.repeat = rs;
        }
    }
    if (has(o, "when")) { p.when = parseCond(o["when"], r, where + ".when"); if (!p.when) return std::nullopt; }
    p.size = parseSize(at(o, "size"), r, where + ".size", flex1());
    p.optional = at(o, "optional").is_boolean() && o["optional"].get<bool>();
    p.priority = fin(at(o, "priority")) ? static_cast<int>(clampd(jsRound(o["priority"].get<double>()), -1000, 1000)) : 0;
    return p;
}

std::optional<Option> parseOption(const ojson& jj, Reader& r, const std::string& where, int depth) {
    ojson o = jj.is_string() ? ojson{{"tex", jj}} : jj;
    if (!o.is_object()) { r.warn(where, "an option must be an object"); return std::nullopt; }
    NodePtr node = parseNode(has(o, "node") ? o["node"] : o, r, where, depth);
    if (!node) return std::nullopt;
    Option op;
    op.node = node;
    if (has(o, "when")) { op.when = parseCond(o["when"], r, where + ".when"); if (!op.when) return std::nullopt; }
    op.weight = fin(at(o, "weight")) && o["weight"].get<double>() > 0 ? std::min(o["weight"].get<double>(), 1e6) : 1;
    return op;
}

NodePtr parseNode(const ojson& j, Reader& r, const std::string& where, int depth) {
    if (depth > kMaxDepth) { r.warn(where, "nested deeper than " + std::to_string(kMaxDepth) + " levels"); return nullptr; }
    if (++r.nodes > kMaxNodes) { if (r.nodes == kMaxNodes + 1) r.warn(where, "more than " + std::to_string(kMaxNodes) + " nodes: the rest is ignored"); return nullptr; }
    if (j.is_string()) { if (isName(j)) return parseTex(ojson{{"tex", j}}, r, where); r.warn(where, "'" + str(j) + "' is not a slot name"); return nullptr; }
    if (!j.is_object()) { r.warn(where, "a node must be a slot name or an object"); return nullptr; }
    if (has(j, "node") && !has(j, "tex") && !has(j, "split") && !has(j, "choice") && !has(j, "ref")) return parseNode(j["node"], r, where, depth + 1);
    if (has(j, "tex")) return parseTex(j, r, where);
    if (has(j, "ref")) { if (isName(j["ref"])) { auto n = std::make_shared<Node>(); n->k = Node::K::Ref; n->name = str(j["ref"]); return n; } r.warn(where, "ref needs a rule name"); return nullptr; }
    if (has(j, "split")) {
        const std::string ax = str(j["split"]);
        if (ax != "u" && ax != "v") { r.warn(where, "split must be 'u' or 'v'"); return nullptr; }
        if (!at(j, "parts").is_array()) { r.warn(where, "a split needs a list of parts"); return nullptr; }
        auto n = std::make_shared<Node>();
        n->k = Node::K::Split;
        n->axis = ax;
        int i = 0;
        for (const auto& p : j["parts"]) {
            if (i >= kMaxList) break;
            if (auto pt = parseSplitPart(p, r, where + ".parts[" + std::to_string(i) + "]", depth + 1)) n->parts.push_back(*pt);
            ++i;
        }
        if (j["parts"].size() > static_cast<size_t>(kMaxList)) r.warn(where, "more than " + std::to_string(kMaxList) + " parts: the rest is ignored");
        if (n->parts.empty()) { r.warn(where, "a split without usable parts"); return nullptr; }
        const std::string lo = str(at(j, "leftover"));
        n->leftover = lo == "start" || lo == "center" || lo == "end" ? lo : "stretch";
        if (has(j, "fill")) n->fill = parseNode(j["fill"], r, where + ".fill", depth + 1);
        return n;
    }
    if (has(j, "choice")) {
        if (!j["choice"].is_array()) { r.warn(where, "choice needs a list of options"); return nullptr; }
        auto n = std::make_shared<Node>();
        n->k = Node::K::Choice;
        int i = 0;
        for (const auto& o : j["choice"]) {
            if (i >= kMaxList) break;
            if (auto op = parseOption(o, r, where + ".choice[" + std::to_string(i) + "]", depth + 1)) n->options.push_back(*op);
            ++i;
        }
        if (n->options.empty()) { r.warn(where, "a choice without usable options"); return nullptr; }
        const std::string mode = str(at(j, "mode"));
        n->mode = mode == "random" || mode == "cycle" ? mode : "first";
        if (isVarName(at(j, "by"))) n->by = str(j["by"]);
        return n;
    }
    r.warn(where, "node has none of tex / split / choice / ref");
    return nullptr;
}

std::map<std::string, ParamDef> parseParams(const ojson& j, Reader& r) {
    std::map<std::string, ParamDef> out;
    if (j.is_null()) return out;
    if (!j.is_object()) { r.warn("params", "must be an object"); return out; }
    int count = 0;
    for (auto it = j.begin(); it != j.end() && count < kMaxParams; ++it, ++count) {
        const std::string name = it.key();
        const ojson& d = it.value();
        const std::string where = "params." + name;
        if (!isNameS(name) || !d.is_object()) { r.warn(where, "unusable parameter"); continue; }
        ParamDef p;
        const std::string type = str(at(d, "type"));
        if (type == "bool") { p.type = "bool"; p.dbool = at(d, "default").is_boolean() && d["default"].get<bool>(); out[name] = p; continue; }
        if (type == "choice") {
            if (at(d, "options").is_array())
                for (const auto& o : d["options"]) if (o.is_string() && !o.get<std::string>().empty() && o.get<std::string>().size() <= 64 && p.options.size() < static_cast<size_t>(kMaxList)) p.options.push_back(o.get<std::string>());
            if (p.options.empty()) { r.warn(where, "a choice parameter needs options"); continue; }
            p.type = "choice";
            const std::string dv = str(at(d, "default"));
            p.dstr = at(d, "default").is_string() && std::find(p.options.begin(), p.options.end(), dv) != p.options.end() ? dv : p.options[0];
            out[name] = p;
            continue;
        }
        if (type == "number" || type == "int") {
            p.type = type;
            p.min = fin(at(d, "min")) ? d["min"].get<double>() : 0;
            p.max = fin(at(d, "max")) && d["max"].get<double>() >= p.min ? d["max"].get<double>() : std::max(p.min, 1000.0);
            p.step = fin(at(d, "step")) && d["step"].get<double>() > 0 ? d["step"].get<double>() : type == "int" ? 1 : 0.1;
            const double dflt = clampd(fin(at(d, "default")) ? d["default"].get<double>() : p.min, p.min, p.max);
            p.dnum = type == "int" ? jsRound(dflt) : dflt;
            out[name] = p;
            continue;
        }
        r.warn(where, "type must be number, int, bool or choice");
    }
    return out;
}

// ---- tileset.ts reader
std::optional<std::string> safePath(const ojson& t) { if (!t.is_string() || !safeTexturePath(t.get<std::string>())) return std::nullopt; return t.get<std::string>(); }
int pieceCols(const KitPiece& p) { return p.cells.empty() ? 0 : static_cast<int>(p.cells[0].size()); }
int pieceRows(const KitPiece& p) { return static_cast<int>(p.cells.size()); }

std::optional<KitPiece> parsePiece(const ojson& jj, const std::function<void(const std::string&)>& warn, const std::string& where) {
    ojson j = jj.is_string() ? ojson{{"tex", jj}} : jj;
    if (!j.is_object()) { warn(where + ": a piece must be a texture or an object"); return std::nullopt; }
    ojson grid = has(j, "cells") ? j["cells"] : ojson();
    if (grid.is_null() && at(j, "tex").is_string()) grid = ojson::array({ojson::array({j["tex"]})});
    if (grid.is_null() && at(j, "tex").is_array()) grid = j["tex"];
    if (!grid.is_array() || grid.empty() || grid.size() > 6) { warn(where + ": a piece needs \"tex\" or \"cells\" (at most 6 rows)"); return std::nullopt; }
    KitPiece p;
    for (const auto& row : grid) {
        const ojson list = row.is_array() ? row : ojson::array({row});
        if (list.empty() || list.size() > 6) { warn(where + ": a row of a piece needs 1 to 6 textures"); return std::nullopt; }
        std::vector<std::optional<std::string>> r;
        for (const auto& t : list) {
            if (t.is_null()) { r.push_back(std::nullopt); continue; }
            auto s = safePath(t);
            if (!s) warn(where + ": '" + (t.is_string() ? t.get<std::string>() : t.dump()) + "' is not a valid texture path (the piece is kept without it)");
            r.push_back(s);
        }
        p.cells.push_back(r);
    }
    for (const auto& r : p.cells) if (r.size() != p.cells[0].size()) { warn(where + ": all rows of a piece must have the same number of textures"); return std::nullopt; }
    const std::string place = str(at(j, "place"));
    p.place = place == "any" || place == "left" || place == "right" || place == "middle" || place == "once" ? place : "any";
    if (at(j, "match").is_string()) {
        std::string m = j["match"].get<std::string>();
        m.erase(0, m.find_first_not_of(" \t\n\r"));
        m.erase(m.find_last_not_of(" \t\n\r") + 1);
        static const std::regex re("^[A-Za-z0-9 _\\-]{1,32}$");
        if (std::regex_match(m, re)) p.match = m;
    }
    if (has(j, "match") && !j["match"].is_null() && !p.match) warn(where + ": 'match' must be a short name (letters, digits, space, _ -)");
    p.weight = fin(at(j, "weight")) && j["weight"].get<double>() > 0 ? std::min(j["weight"].get<double>(), 1000.0) : 1;
    return p;
}

KitLane parseLane(const ojson& j, const std::function<void(const std::string&)>& warn, const std::string& where) {
    KitLane lane;
    if (!j.is_object()) return lane;
    if (at(j, "pieces").is_array()) {
        int i = 0;
        for (const auto& p : j["pieces"]) {
            if (i >= 48) break;
            if (auto pc = parsePiece(p, warn, where + ".pieces[" + std::to_string(i) + "]")) lane.pieces.push_back(*pc);
            ++i;
        }
    }
    int rows = 1;
    if (!lane.pieces.empty()) { rows = 0; for (const auto& p : lane.pieces) rows = std::max(rows, pieceRows(p)); }
    lane.rows = fin(at(j, "rows")) ? std::max(1, std::min(6, static_cast<int>(jsRound(j["rows"].get<double>())))) : rows;
    const size_t before = lane.pieces.size();
    lane.pieces.erase(std::remove_if(lane.pieces.begin(), lane.pieces.end(), [&](const KitPiece& p) { return pieceRows(p) != lane.rows; }), lane.pieces.end());
    if (lane.pieces.size() < before) warn(where + ": pieces with another height than the lane (" + std::to_string(lane.rows) + " row" + (lane.rows == 1 ? "" : "s") + ") were left out");
    return lane;
}

double parseStretch(const ojson& j) { return j.is_number() && std::isfinite(j.get<double>()) && j.get<double>() >= 0 ? std::min(j.get<double>(), 500.0) / 100 : 0.25; }
std::optional<V2> parseTile(const ojson& j) {
    if (j.is_array() && j.size() == 2 && fin(j[0]) && fin(j[1]) && j[0].get<double>() > 0.05 && j[1].get<double>() > 0.05)
        return V2{std::min(j[0].get<double>(), 1000.0), std::min(j[1].get<double>(), 1000.0)};
    return std::nullopt;
}

std::optional<Kit> parseKit(const ojson& j, const std::function<void(const std::string&)>& warn) {
    if (!j.is_object()) { warn("kit: must be an object"); return std::nullopt; }
    Kit k;
    if (at(j, "kind") == "frame") {
        k.kind = "frame";
        k.tile = parseTile(at(j, "tile"));
        k.maxStretch = parseStretch(at(j, "maxStretch"));
        for (const char* s : kFrameSlots) {
            const ojson& v = at(j, s);
            k.frame[s] = v.is_null() ? std::nullopt : safePath(v);
            if (!v.is_null() && !k.frame[s]) warn(std::string("kit.") + s + ": not a valid texture path");
        }
        return k;
    }
    if (at(j, "kind") != "wall") { warn("kit: unknown kind '" + (at(j, "kind").is_string() ? str(j["kind"]) : at(j, "kind").is_null() ? std::string("undefined") : at(j, "kind").dump()) + "' (wall or frame)"); return std::nullopt; }
    k.kind = "wall";
    k.tile = parseTile(at(j, "tile"));
    k.maxStretch = parseStretch(at(j, "maxStretch"));
    k.avoidRepeat = !(at(j, "avoidRepeat").is_boolean() && !j["avoidRepeat"].get<bool>());
    k.top = parseLane(at(j, "top"), warn, "kit.top");
    k.middle = parseLane(at(j, "middle"), warn, "kit.middle");
    k.bottom = parseLane(at(j, "bottom"), warn, "kit.bottom");
    return k;
}

std::vector<std::string> kitTextures(const Kit& k) {
    std::vector<std::string> out;
    auto add = [&](const std::optional<std::string>& t) { if (t && std::find(out.begin(), out.end(), *t) == out.end()) out.push_back(*t); };
    if (k.kind == "frame") { for (const char* s : kFrameSlots) { auto it = k.frame.find(s); if (it != k.frame.end()) add(it->second); } }
    else for (const KitLane* l : {&k.top, &k.middle, &k.bottom}) for (const auto& p : l->pieces) for (const auto& r : p.cells) for (const auto& t : r) add(t);
    return out;
}
}  // namespace

GroupParse parseGroup(const ojson& j) {
    GroupParse out;
    Reader r;
    if (!j.is_object()) { out.errors.push_back("the file is not a JSON object"); return out; }
    if (has(j, "format") && !(j["format"].is_number() && j["format"].get<double>() == 1)) {
        out.errors.push_back("unsupported format " + (j["format"].is_string() ? str(j["format"]) : j["format"].dump()) + " (this editor reads format 1)");
        return out;
    }
    auto g = std::make_shared<TexGroup>();
    g->name = at(j, "name").is_string() ? str(j["name"]).substr(0, 120) : "";
    if (has(j, "kit")) {
        std::vector<std::string> kw;
        auto kit = parseKit(j["kit"], [&](const std::string& m) { kw.push_back(m); });
        if (!kit) { out.errors.push_back("the \"kit\" is not usable"); for (auto& w : kw) out.errors.push_back(w); out.warnings = kw; return out; }
        const auto tex = kitTextures(*kit);
        for (size_t i = 0; i < tex.size(); ++i) g->slots.push_back({"s" + std::to_string(i), tex[i]});
        if (!g->slots.empty()) g->fallback = g->slots[0].first;
        auto root = std::make_shared<Node>();
        root->k = Node::K::Tex;
        root->slot = g->slots.empty() ? "" : g->slots[0].first;
        g->root = root;
        g->kit = kit;
        out.group = g;
        out.warnings = kw;
        return out;
    }
    if (at(j, "slots").is_object()) {
        int count = 0;
        for (auto it = j["slots"].begin(); it != j["slots"].end() && count < kMaxSlots; ++it, ++count) {
            const ojson& v = it.value();
            const ojson path = v.is_string() ? v : v.is_object() ? at(v, "texture") : ojson();
            if (isNameS(it.key()) && path.is_string() && safeTexturePath(path.get<std::string>())) g->slots.push_back({it.key(), path.get<std::string>()});
            else r.warn("slots." + it.key(), "needs a valid texture path");
        }
    } else r.warn("slots", "missing: the rules have no textures to use");
    if (at(j, "rules").is_object()) {
        int count = 0;
        for (auto it = j["rules"].begin(); it != j["rules"].end() && count < kMaxRules; ++it, ++count) {
            NodePtr node = isNameS(it.key()) ? parseNode(it.value(), r, "rules." + it.key(), 1) : nullptr;
            if (node) g->rules[it.key()] = node;
            else if (!isNameS(it.key())) r.warn("rules." + it.key(), "not a valid rule name");
        }
    }
    g->root = has(j, "root") ? parseNode(j["root"], r, "root", 0) : nullptr;
    if (!g->root) {
        out.errors.push_back("no usable \"root\" node");
        for (const auto& w : r.warnings) if (w.rfind("root", 0) == 0) out.errors.push_back(w);
        out.warnings = r.warnings;
        return out;
    }
    g->params = parseParams(at(j, "params"), r);
    if (at(j, "fallback").is_string() && g->slot(str(j["fallback"]))) g->fallback = str(j["fallback"]);
    else if (!g->slots.empty()) g->fallback = g->slots[0].first;
    out.group = g;
    out.warnings = r.warnings;
    return out;
}

std::vector<std::string> groupTextures(const TexGroup& g) {
    std::vector<std::string> out;
    for (const auto& [k, v] : g.slots) if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    return out;
}
std::optional<std::string> groupFallbackTexture(const TexGroup& g) {
    if (!g.fallback) return std::nullopt;
    const std::string* p = g.slot(*g.fallback);
    return p ? std::optional<std::string>(*p) : std::nullopt;
}

std::optional<FaceLayout> parseFaceLayout(const json& j) {
    if (!j.is_object() || !j.contains("group") || !j["group"].is_string()) return std::nullopt;
    const std::string group = j["group"].get<std::string>();
    if (!safeTexturePath(group)) return std::nullopt;
    FaceLayout l;
    l.group = group;
    if (j.contains("params") && j["params"].is_object()) {
        int count = 0;
        for (auto it = j["params"].begin(); it != j["params"].end() && count < kMaxParams; ++it, ++count) {
            const json& v = it.value();
            if (!isNameS(it.key())) continue;
            ParamValue pv;
            if (v.is_number() && std::isfinite(v.get<double>())) { pv.t = ParamValue::T::Num; pv.n = v.get<double>(); }
            else if (v.is_boolean()) { pv.t = ParamValue::T::Bool; pv.b = v.get<bool>(); }
            else if (v.is_string() && v.get<std::string>().size() <= 64) { pv.t = ParamValue::T::Str; pv.s = v.get<std::string>(); }
            else continue;
            l.params[it.key()] = pv;
        }
    }
    const json& rot = j.contains("rotate") ? j["rotate"] : json();
    l.rotate = rot.is_number() && (rot.get<double>() == 90 || rot.get<double>() == 180 || rot.get<double>() == 270) ? static_cast<int>(rot.get<double>()) : 0;
    l.flipU = j.contains("flipU") && j["flipU"].is_boolean() && j["flipU"].get<bool>();
    if (j.contains("seed") && j["seed"].is_number() && std::isfinite(j["seed"].get<double>())) {
        const double t = std::trunc(j["seed"].get<double>());
        // JavaScript: Math.trunc(x) >>> 0 (ToUint32: modulo 2^32)
        const double m = std::fmod(t, 4294967296.0);
        l.seed = static_cast<uint32_t>(static_cast<long long>(m < 0 ? m + 4294967296.0 : m));
    }
    return l;
}

// ---- layout.ts: the solver -------------------------------------------------------------------------------------------------------------------------
namespace {
constexpr int kMaxLayoutCells = 2048, kMaxLayoutVisits = 40000, kMaxRefDepth = 40;
constexpr double kEps = 1e-9;

struct Value { enum class T { Undef, Num, Str, Bool } t = T::Undef; double n = 0; std::string s; bool b = false; };
Value vNum(double n) { Value v; v.t = Value::T::Num; v.n = n; return v; }
Value vStr(const std::string& s) { Value v; v.t = Value::T::Str; v.s = s; return v; }
Value vBool(bool b) { Value v; v.t = Value::T::Bool; v.b = b; return v; }
Value fromParam(const ParamValue& p) { return p.t == ParamValue::T::Num ? vNum(p.n) : p.t == ParamValue::T::Bool ? vBool(p.b) : vStr(p.s); }
std::optional<double> num(const Value& v) {
    if (v.t == Value::T::Num && std::isfinite(v.n)) return v.n;
    if (v.t == Value::T::Bool) return v.b ? 1.0 : 0.0;
    return std::nullopt;
}
bool same(const Value& a, const Value& b) {
    if (a.t == Value::T::Str || b.t == Value::T::Str) return a.t == b.t && a.s == b.s;
    const auto x = num(a), y = num(b);
    return x && y && std::fabs(*x - *y) < 1e-9;
}
std::string fmt2(double v) {  // String(Math.round(v * 100) / 100)
    const double r = std::floor(v * 100 + 0.5) / 100;
    std::ostringstream o;
    o.precision(15);
    o << r;
    return o.str();
}

std::map<std::string, ParamValue> resolveParams(const TexGroup& g, const std::map<std::string, ParamValue>& ov) {
    std::map<std::string, ParamValue> out;
    for (const auto& [name, d] : g.params) {
        const auto it = ov.find(name);
        const ParamValue* v = it == ov.end() ? nullptr : &it->second;
        ParamValue p;
        if (d.type == "bool") { p.t = ParamValue::T::Bool; p.b = v && v->t == ParamValue::T::Bool ? v->b : d.dbool; }
        else if (d.type == "choice") { p.t = ParamValue::T::Str; p.s = v && v->t == ParamValue::T::Str && std::find(d.options.begin(), d.options.end(), v->s) != d.options.end() ? v->s : d.dstr; }
        else { p.t = ParamValue::T::Num; const double n = v && v->t == ParamValue::T::Num ? clampd(v->n, d.min, d.max) : d.dnum; p.n = d.type == "int" ? jsRound(n) : n; }
        out[name] = p;
    }
    return out;
}

struct Rect { double x, y, w, h; };
struct Rep { std::string name; int index, count; };
struct Ctx { std::string path; std::vector<Rep> reps; };

class Solver {
public:
    std::vector<Cell> cells;
    std::vector<std::string> warnings;
    bool truncated = false;
    explicit Solver(const LayoutInput& i) : i_(i) {
        params_ = resolveParams(*i.group, i.params);
        const auto fb = groupFallbackTexture(*i.group);
        fallbackPath_ = fb ? *fb : i.hasFallbackTexture ? i.fallbackTexture : "dev/grey";
    }
    void run() {
        if (!(i_.w > kEps && i_.h > kEps)) return;
        node(i_.group->root, {0, 0, i_.w, i_.h}, {"r", {}}, 0);
    }

private:
    const LayoutInput& i_;
    std::map<std::string, ParamValue> params_;
    std::string fallbackPath_;
    std::set<std::string> warned_;
    int visits_ = 0;

    void warn(const std::string& m) { if (!warned_.count(m) && warnings.size() < 100) { warned_.insert(m); warnings.push_back(m); } }
    void stop(const std::string& why) { if (!truncated) { truncated = true; warn(why); } }

    Value param(const std::string& n) { const auto it = params_.find(n); return it == params_.end() ? Value{} : fromParam(it->second); }

    Value variable(const std::string& name, const Rect& r, const Ctx& c) {
        if (name == "w") return vNum(r.w);
        if (name == "h") return vNum(r.h);
        if (name == "x" || name == "fromLeft") return vNum(r.x);
        if (name == "y" || name == "fromBottom") return vNum(r.y);
        if (name == "fromTop") return vNum(i_.h - (r.y + r.h));
        if (name == "fromRight") return vNum(i_.w - (r.x + r.w));
        if (name == "worldY") return vNum(i_.worldY ? i_.worldY(r.x, r.y) : r.y);
        if (name == "faceW") return vNum(i_.w);
        if (name == "faceH") return vNum(i_.h);
        if (name == "facing") return vStr(i_.facing);
        if (name == "tilt") return vNum(i_.tilt);
        if (name == "rand") return vNum(rand01(i_.seed, c.path));
        const size_t dot = name.find('.');
        const std::string head = dot == std::string::npos ? name : name.substr(0, dot), tail = dot == std::string::npos ? "" : name.substr(dot + 1);
        if (head == "param" && !tail.empty()) return param(tail);
        const Rep* rep = nullptr;
        if (dot == std::string::npos) { if (!c.reps.empty()) rep = &c.reps.back(); }
        else for (auto it = c.reps.rbegin(); it != c.reps.rend(); ++it) if (it->name == head) { rep = &*it; break; }
        const std::string field = dot == std::string::npos ? head : tail;
        if (!rep) {
            if (dot != std::string::npos) return Value{};
            if (field == "index" || field == "fromEnd") return vNum(0);
            if (field == "count") return vNum(1);
            if (field == "first" || field == "last") return vBool(true);
            return Value{};
        }
        if (field == "index") return vNum(rep->index);
        if (field == "count") return vNum(rep->count);
        if (field == "first") return vBool(rep->index == 0);
        if (field == "last") return vBool(rep->index == rep->count - 1);
        if (field == "fromEnd") return vNum(rep->count - 1 - rep->index);
        return Value{};
    }

    Value val(const Val& v, const Rect& r, const Ctx& c) {
        switch (v.t) {
            case Val::T::Param: return param(v.s);
            case Val::T::Var: return variable(v.s, r, c);
            case Val::T::Num: return vNum(v.n);
            case Val::T::Bool: return vBool(v.b);
            case Val::T::Str: return vStr(v.s);
        }
        return Value{};
    }

    bool cond(const Cond& k, const Rect& r, const Ctx& c) {
        switch (k.k) {
            case Cond::K::Const: return k.value;
            case Cond::K::All: for (const auto& x : k.items) if (!cond(*x, r, c)) return false; return true;
            case Cond::K::Any: for (const auto& x : k.items) if (cond(*x, r, c)) return true; return false;
            case Cond::K::Not: return !cond(*k.items[0], r, c);
            case Cond::K::Truthy: {
                const Value v = variable(k.v, r, c);
                if (v.t == Value::T::Undef) warn("unknown variable '" + k.v + "'");
                return (v.t == Value::T::Bool && v.b) || (v.t == Value::T::Num && v.n != 0) || (v.t == Value::T::Str && !v.s.empty());
            }
            case Cond::K::Cmp: {
                const Value a = variable(k.v, r, c);
                if (a.t == Value::T::Undef) { warn("unknown variable '" + k.v + "'"); return false; }
                std::vector<Value> list;
                for (const auto& x : k.vals) list.push_back(val(x, r, c));
                if (k.op == "in") { for (const auto& b : list) if (same(a, b)) return true; return false; }
                if (k.op == "mod") {
                    const auto m = num(list[0]), rem = num(list[1]);
                    if (!(m && *m > 0 && rem)) return false;
                    const auto x = num(a);
                    return x && std::fabs(std::fmod(std::fmod(*x, *m) + *m, *m) - *rem) < 1e-6;
                }
                const Value& b = list[0];
                if (k.op == "==") return same(a, b);
                if (k.op == "!=") return !same(a, b);
                const auto x = num(a), y = num(b);
                if (!x || !y) return false;
                if (k.op == "<") return *x < *y - 1e-9;
                if (k.op == "<=") return *x <= *y + 1e-9;
                if (k.op == ">") return *x > *y + 1e-9;
                return *x >= *y - 1e-9;
            }
        }
        return false;
    }

    double metres(const SizeSpec& s, double extent, const std::string& axis) {
        switch (s.k) {
            case SizeSpec::K::Fixed: return s.m;
            case SizeSpec::K::Frac: return s.f * extent;
            case SizeSpec::K::Param: {
                const auto it = params_.find(s.name);
                if (it == params_.end() || it->second.t != ParamValue::T::Num) { warn("size uses parameter '" + s.name + "' which is not a number"); return 0; }
                return it->second.n * s.mul;
            }
            case SizeSpec::K::Natural: {
                const std::string* path = i_.group->slot(s.name);
                const TexInfo* info = path && i_.info ? i_.info(*path) : nullptr;
                if (!info) { warn("natural size of slot '" + s.name + "' is not known (texture not loaded?): 1 m used"); return 1; }
                return naturalSize(*info)[axis == "u" ? 0 : 1];
            }
            case SizeSpec::K::Flex: return 0;
        }
        return 0;
    }

    void fallbackCell(const Rect& r, const Ctx& c, const std::string& why) {
        warn(why);
        Node n;
        n.k = Node::K::Tex;
        n.slot = "";
        add(r, c, n, fallbackPath_);
    }

    void add(const Rect& r, const Ctx& c, const Node& n, const std::string& texture) {
        if (static_cast<int>(cells.size()) >= kMaxLayoutCells) { stop("more than " + std::to_string(kMaxLayoutCells) + " cells: the face is not laid out (use bigger parts or fewer repeats)"); return; }
        Cell cell;
        cell.x0 = r.x; cell.y0 = r.y; cell.x1 = r.x + r.w; cell.y1 = r.y + r.h;
        cell.texture = texture; cell.slot = n.slot; cell.fit = n.fit; cell.origin = n.origin; cell.alignU = n.alignU; cell.alignV = n.alignV;
        cell.flipU = n.flipU; cell.flipV = n.flipV; cell.rotate = n.rotate; cell.path = c.path;
        cells.push_back(cell);
    }

    void node(const NodePtr& np, const Rect& r, const Ctx& c, int depth) {
        if (truncated) return;
        if (++visits_ > kMaxLayoutVisits) { stop("the rules need too many steps (a rule may call itself)"); return; }
        if (!(r.w > 1e-6 && r.h > 1e-6)) return;
        if (depth > kMaxRefDepth) { fallbackCell(r, c, "rules nested too deep (does a rule call itself?)"); return; }
        const Node& n = *np;
        switch (n.k) {
            case Node::K::Tex: {
                const std::string* path = i_.group->slot(n.slot);
                if (!path) { fallbackCell(r, c, "unknown slot '" + n.slot + "'"); return; }
                add(r, c, n, *path);
                return;
            }
            case Node::K::Ref: {
                const auto it = i_.group->rules.find(n.name);
                if (it == i_.group->rules.end()) { fallbackCell(r, c, "unknown rule '" + n.name + "'"); return; }
                node(it->second, r, {c.path + ">" + n.name, c.reps}, depth + 1);
                return;
            }
            case Node::K::Choice: return choice(n, r, c, depth);
            case Node::K::Split: return split(n, r, c, depth);
        }
    }

    void choice(const Node& n, const Rect& r, const Ctx& c, int depth) {
        std::vector<std::pair<const Option*, int>> matching;
        for (size_t i = 0; i < n.options.size(); ++i) if (!n.options[i].when || cond(*n.options[i].when, r, c)) matching.push_back({&n.options[i], static_cast<int>(i)});
        if (matching.empty()) { fallbackCell(r, c, "no option of a choice matched at " + c.path); return; }
        auto pick = matching[0];
        if (n.mode == "random") {
            double total = 0;
            for (const auto& m : matching) total += m.first->weight;
            double t = rand01(i_.seed, c.path + "?") * total;
            pick = matching.back();
            for (const auto& m : matching) { if (t < m.first->weight) { pick = m; break; } t -= m.first->weight; }
        } else if (n.mode == "cycle") {
            const auto by = n.by ? num(variable(*n.by, r, c)) : num(variable("index", r, c));
            const long long k = static_cast<long long>(std::floor(by ? *by : 0));
            const long long m = static_cast<long long>(matching.size());
            pick = matching[static_cast<size_t>(((k % m) + m) % m)];
        }
        node(pick.first->node, r, {c.path + "?" + std::to_string(pick.second), c.reps}, depth + 1);
    }

    struct Item { const Part* part; int idx; std::optional<double> fixed; double weight, min, max, size; bool dropped; };

    void split(const Node& n, const Rect& r, const Ctx& c, int depth) {
        const bool horizontal = n.axis == "u";
        const double E = horizontal ? r.w : r.h;
        std::vector<Item> items;
        for (size_t idx = 0; idx < n.parts.size(); ++idx) {
            const Part& part = n.parts[idx];
            if (part.when && !cond(*part.when, r, c)) continue;
            const bool flex = part.size.k == SizeSpec::K::Flex;
            items.push_back({&part, static_cast<int>(idx), flex ? std::nullopt : std::optional<double>(std::max(0.0, metres(part.size, E, n.axis))),
                             flex ? part.size.weight : 0, flex ? part.size.min : 0, flex ? part.size.max : kInf, 0, false});
        }
        if (items.empty()) return;
        auto need = [&]() { double a = 0; for (const auto& it : items) a += it.dropped ? 0 : (it.fixed ? *it.fixed : it.min); return a; };
        if (need() > E + 1e-6) {
            std::vector<Item*> order;
            for (auto& it : items) if (it.part->optional) order.push_back(&it);
            std::stable_sort(order.begin(), order.end(), [](const Item* a, const Item* b) {
                if (a->part->priority != b->part->priority) return a->part->priority < b->part->priority;
                return b->idx < a->idx;
            });
            for (Item* it : order) { if (need() <= E + 1e-6) break; it->dropped = true; warn("part " + std::to_string(it->idx) + " of " + c.path + " left out: not enough room (" + fmt2(E) + " m)"); }
        }
        std::vector<Item*> active;
        for (auto& it : items) if (!it.dropped) active.push_back(&it);
        if (active.empty()) return;
        double total = need();
        if (total > E + 1e-6) {
            warn("parts of " + c.path + " need " + fmt2(total) + " m but only " + fmt2(E) + " m are there: shrunk to fit");
            const double f = E / total;
            for (Item* it : active) it->size = (it->fixed ? *it->fixed : it->min) * f;
        } else {
            std::vector<Item*> flexes;
            for (Item* it : active) if (!it->fixed) flexes.push_back(it);
            for (Item* it : active) if (it->fixed) it->size = *it->fixed;
            double remaining = E;
            for (Item* it : active) remaining -= it->fixed ? *it->fixed : 0;
            std::vector<Item*> open = flexes;
            for (size_t pass = 0; pass <= flexes.size() && !open.empty(); ++pass) {
                double wsum = 0;
                for (Item* it : open) wsum += it->weight;
                std::vector<double> shares;
                for (Item* it : open) shares.push_back((remaining * it->weight) / wsum);
                std::vector<Item*> low, high;
                for (size_t k = 0; k < open.size(); ++k) {
                    if (shares[k] < open[k]->min - 1e-9) low.push_back(open[k]);
                    if (shares[k] > open[k]->max + 1e-9) high.push_back(open[k]);
                }
                const std::vector<Item*>& pin = !low.empty() ? low : high;
                if (pin.empty()) { for (size_t k = 0; k < open.size(); ++k) open[k]->size = shares[k]; open.clear(); break; }
                for (Item* it : pin) { it->size = !low.empty() ? it->min : it->max; remaining -= it->size; }
                std::vector<Item*> rest;
                for (Item* it : open) if (std::find(pin.begin(), pin.end(), it) == pin.end()) rest.push_back(it);
                open = rest;
            }
        }
        double used = 0;
        for (Item* it : active) used += it->size;
        double cursor = 0;
        const double gap = E - used;
        if (gap > 1e-6) {
            if (n.leftover == "stretch" && used > kEps) { const double f = E / used; for (Item* it : active) it->size *= f; }
            else if (n.leftover == "stretch") active[0]->size += gap;
            else {
                const double before = n.leftover == "end" ? gap : n.leftover == "center" ? gap / 2 : 0;
                const double after = gap - before;
                if (before > 1e-6) gapFill(n, r, c, depth, horizontal, 0, before);
                cursor = before;
                emit(n, r, c, depth, active, horizontal, cursor);
                if (after > 1e-6) gapFill(n, r, c, depth, horizontal, E - after, after);
                return;
            }
        }
        emit(n, r, c, depth, active, horizontal, cursor);
    }

    void gapFill(const Node& n, const Rect& r, const Ctx& c, int depth, bool horizontal, double at, double size) {
        const Rect rect = horizontal ? Rect{r.x + at, r.y, size, r.h} : Rect{r.x, r.y + at, r.w, size};
        const Ctx gc{c.path + ".gap" + (at > 0 ? "e" : "s"), c.reps};
        if (n.fill) node(n.fill, rect, gc, depth + 1);
        else fallbackCell(rect, gc, "leftover space of " + c.path + " filled with the fallback texture");
    }

    void emit(const Node& n, const Rect& r, const Ctx& c, int depth, const std::vector<Item*>& active, bool horizontal, double start) {
        double at = start;
        for (const Item* it : active) {
            const double size = it->size;
            if (size > 1e-6) {
                auto rect = [&](double a, double s) { return horizontal ? Rect{r.x + a, r.y, s, r.h} : Rect{r.x, r.y + a, r.w, s}; };
                const auto& rep = it->part->repeat;
                if (!rep) node(it->part->node, rect(at, size), {c.path + "/" + std::to_string(it->idx), c.reps}, depth + 1);
                else {
                    const int count = repeatCount(*rep, size, n.axis, c.path + "/" + std::to_string(it->idx));
                    for (int k = 0; k < count && !truncated; ++k) {
                        std::vector<Rep> reps = c.reps;
                        reps.push_back({rep->as, k, count});
                        node(it->part->node, rect(at + (size * k) / count, size / count), {c.path + "/" + std::to_string(it->idx) + "." + std::to_string(k), reps}, depth + 1);
                    }
                }
            }
            at += size;
        }
    }

    int repeatCount(const RepeatSpec& rep, double extent, const std::string& axis, const std::string& where) {
        double s = metres(rep.size, extent, axis);
        if (!(s >= 0.01)) { warn("repeat at " + where + " has a size below 1 cm: 1 cm used"); s = 0.01; }
        const double q = extent / s;
        long long n = rep.fit == "floor" ? static_cast<long long>(std::floor(q + 1e-9)) : rep.fit == "ceil" ? static_cast<long long>(std::ceil(q - 1e-9)) : static_cast<long long>(std::floor(q + 0.5));
        if (n < rep.min) n = rep.min;
        if (n > rep.max) { warn("repeat at " + where + " limited to " + std::to_string(rep.max) + " copies"); n = rep.max; }
        return static_cast<int>(std::max(0LL, n));
    }
};

// ---- tileset.ts solver
double bestCount(double q) {
    if (!(q > 1)) return 1;
    const double f = std::floor(q);
    return q < std::sqrt(f * (f + 1)) ? f : f + 1;
}
double stretchOf(double scale) { return std::max(scale, 1 / scale) - 1; }
struct KSeg { double a0, a1, crop; };
std::pair<std::vector<KSeg>, bool> axisTiles(double len, double tile, double limit, bool natural = false) {
    auto even = [&](int n) { std::vector<KSeg> s; for (int k = 0; k < n; ++k) s.push_back({len * k / n, len * (k + 1) / n, 1}); return s; };
    const double q = len / tile;
    const int n = static_cast<int>(bestCount(q));
    if (!natural && (limit <= 0 || stretchOf(len / (n * tile)) <= limit + 1e-9)) return {even(n), false};
    const int full = static_cast<int>(std::floor(q + 1e-9));
    const double rem = len - full * tile;
    if (full >= 1 && rem <= 0.03 * tile) return {even(full), false};
    std::vector<KSeg> segs;
    for (int k = 0; k < full; ++k) segs.push_back({k * tile, (k + 1) * tile, 1});
    if (rem > 1e-9) segs.push_back({full * tile, len, rem / tile});
    return {segs, rem > 1e-9};
}
std::string pct(double v) { return std::to_string(static_cast<long long>(std::floor(v * 100 + 0.5))); }

Cell kcell(double x0, double y0, double x1, double y1, const std::string& texture, const std::string& path, double cropU = 1, double cropV = 1) {
    Cell c;
    c.x0 = x0; c.y0 = y0; c.x1 = x1; c.y1 = y1; c.texture = texture; c.slot = texture; c.fit = "stretch"; c.origin = "cell"; c.path = path;
    if (cropU < 1 - 1e-9) c.cropU = cropU;
    if (cropV < 1 - 1e-9) c.cropV = cropV;
    return c;
}

struct Placement { const KitPiece* piece; int col; };
struct RowOut { std::vector<Placement> placements; std::vector<int> gaps; std::vector<std::optional<std::string>> tags; };

RowOut placeRow(const std::vector<KitPiece>& pieces, int nC, uint32_t seed, const std::string& key, bool avoid, const std::vector<std::optional<std::string>>& below,
                const std::function<void(const std::string&)>& warn) {
    int n = 0;
    auto rnd = [&]() { return rand01(seed, key + "#" + std::to_string(n++)); };
    auto pick = [&](const std::vector<const KitPiece*>& list) {
        double total = 0;
        for (const auto* p : list) total += p->weight;
        double t = rnd() * total;
        for (const auto* p : list) { if (t < p->weight) return p; t -= p->weight; }
        return list.back();
    };
    std::vector<bool> taken(static_cast<size_t>(nC), false);
    RowOut out;
    auto freeAt = [&](int c, int w) { if (c < 0 || c + w > nC) return false; for (int i = c; i < c + w; ++i) if (taken[static_cast<size_t>(i)]) return false; return true; };
    auto matches = [&](const KitPiece* p, int c) {
        if (!p->match) return true;
        for (int i = 0; i < pieceCols(*p); ++i) { const auto& b = below[static_cast<size_t>(c + i)]; if (b && *b != *p->match) return false; }
        return true;
    };
    bool relaxed = false;
    auto put = [&](const KitPiece* piece, int col) { for (int i = 0; i < pieceCols(*piece); ++i) taken[static_cast<size_t>(col + i)] = true; out.placements.push_back({piece, col}); };
    for (const char* place : {"left", "right", "middle"}) {
        auto atOf = [&](const KitPiece* p) { const std::string pl = place; return pl == "left" ? 0 : pl == "right" ? nC - pieceCols(*p) : (nC - pieceCols(*p)) / 2 - ((nC - pieceCols(*p)) < 0 && (nC - pieceCols(*p)) % 2 ? 1 : 0); };
        std::vector<const KitPiece*> fit, ok;
        for (const auto& p : pieces) if (p.place == place && pieceCols(p) <= nC) fit.push_back(&p);
        for (const auto* p : fit) if (matches(p, atOf(p))) ok.push_back(p);
        if (!fit.empty() && ok.empty()) relaxed = true;
        if (!ok.empty()) fit = ok;
        if (fit.empty()) continue;
        const KitPiece* p = pick(fit);
        if (freeAt(atOf(p), pieceCols(*p))) put(p, atOf(p));
    }
    for (const auto& pp : pieces) {
        if (pp.place != "once") continue;
        const KitPiece* p = &pp;
        const int w = pieceCols(*p);
        std::vector<int> spots;
        for (int c = 0; c + w <= nC; ++c) if (freeAt(c, w) && matches(p, c)) spots.push_back(c);
        if (spots.empty()) { for (int c = 0; c + w <= nC; ++c) if (freeAt(c, w)) spots.push_back(c); if (!spots.empty()) relaxed = true; }
        if (!spots.empty()) put(p, spots[static_cast<size_t>(std::min(static_cast<int>(spots.size()) - 1, static_cast<int>(std::floor(rnd() * spots.size()))))]);
    }
    std::vector<const KitPiece*> ordinary;
    for (const auto& p : pieces) if (p.place == "any") ordinary.push_back(&p);
    const KitPiece* prev = nullptr;
    for (int c = 0; c < nC;) {
        if (taken[static_cast<size_t>(c)]) { ++c; prev = nullptr; continue; }
        int run = 0;
        while (c + run < nC && !taken[static_cast<size_t>(c + run)]) ++run;
        std::vector<const KitPiece*> candidates, matching;
        for (const auto* p : ordinary) if (pieceCols(*p) <= run) candidates.push_back(p);
        for (const auto* p : candidates) if (matches(p, c)) matching.push_back(p);
        if (!candidates.empty() && matching.empty()) relaxed = true;
        if (!matching.empty()) candidates = matching;
        if (candidates.empty()) for (const auto& p : pieces) if ((p.place == "any" || p.place == "once") && pieceCols(p) <= run && matches(&p, c)) candidates.push_back(&p);
        if (candidates.empty()) for (const auto& p : pieces) if ((p.place == "any" || p.place == "once") && pieceCols(p) <= run) candidates.push_back(&p);
        if (candidates.empty()) {
            warn("no piece is narrow enough for a gap of " + std::to_string(run) + " tile" + (run == 1 ? "" : "s") + ": add a one tile wide piece");
            for (int i = 0; i < run; ++i) { taken[static_cast<size_t>(c + i)] = true; out.gaps.push_back(c + i); }
            c += run;
            prev = nullptr;
            continue;
        }
        if (avoid && prev && candidates.size() > 1) {
            std::vector<const KitPiece*> others;
            for (const auto* p : candidates) if (p != prev) others.push_back(p);
            if (!others.empty()) candidates = others;
        }
        const KitPiece* p = pick(candidates);
        put(p, c);
        c += pieceCols(*p);
        prev = p;
    }
    if (relaxed) warn(key + ": no piece with a matching Match name fits somewhere: a piece was put on a different one");
    out.tags.assign(static_cast<size_t>(nC), std::nullopt);
    for (const auto& pl : out.placements) for (int i = 0; i < pieceCols(*pl.piece); ++i) out.tags[static_cast<size_t>(pl.col + i)] = pl.piece->match;
    std::stable_sort(out.placements.begin(), out.placements.end(), [](const Placement& a, const Placement& b) { return a.col < b.col; });
    return out;
}

LayoutResult solveKit(const Kit& k, const LayoutInput& in) {
    LayoutResult res;
    auto warn = [&](const std::string& m) { if (std::find(res.warnings.begin(), res.warnings.end(), m) == res.warnings.end() && res.warnings.size() < 50) res.warnings.push_back(m); };
    V2 tile{1, 1};
    if (k.tile) tile = *k.tile;
    else { const auto t = kitTextures(k); const TexInfo* ti = !t.empty() && in.info ? in.info(t[0]) : nullptr; if (ti) tile = naturalSize(*ti); }
    const double tw = tile[0], th = tile[1];
    auto& cells = res.cells;
    if (!(in.w > 1e-6 && in.h > 1e-6)) return res;
    const auto kt = kitTextures(k);
    const std::string fallback = in.hasFallbackTexture ? in.fallbackTexture : !kt.empty() ? kt[0] : "dev/grey";
    auto fill = [&](double x0, double y0, double x1, double y1, const std::string& why) { warn(why); cells.push_back(kcell(x0, y0, x1, y1, fallback, "fallback")); };
    const double lim = k.maxStretch;
    auto tooMany = [&](long long n) { if (n > 2048) { warn("more than 2048 tiles: the face is not laid out (use a bigger tile size)"); res.cells.clear(); res.truncated = true; return true; } return false; };

    if (k.kind == "frame") {
        const double cw = std::min(tw, in.w / 2), ch = std::min(th, in.h / 2);
        const double iw = in.w - 2 * cw, ih = in.h - 2 * ch;
        const auto xs = iw > 1e-6 ? axisTiles(iw, tw, lim) : std::pair<std::vector<KSeg>, bool>{{}, false};
        const auto ys = ih > 1e-6 ? axisTiles(ih, th, lim) : std::pair<std::vector<KSeg>, bool>{{}, false};
        if (tooMany(static_cast<long long>(xs.first.size() + 2) * static_cast<long long>(ys.first.size() + 2))) return res;
        if (xs.second || ys.second)
            warn("the last edge and middle tiles are cut off (tiles keep their size because stretching them more than " + pct(lim) + " % is not allowed): the size " + fmt2(in.w) + " x " + fmt2(in.h) + " m is not a whole number of tiles");
        auto add = [&](const char* s, double x0, double y0, double x1, double y1, double cu = 1, double cv = 1) {
            const auto it = k.frame.find(s);
            if (it != k.frame.end() && it->second) cells.push_back(kcell(x0, y0, x1, y1, *it->second, s, cu, cv));
            else fill(x0, y0, x1, y1, std::string("frame slot '") + s + "' has no texture");
        };
        add("bl", 0, 0, cw, ch); add("br", in.w - cw, 0, in.w, ch); add("tl", 0, in.h - ch, cw, in.h); add("tr", in.w - cw, in.h - ch, in.w, in.h);
        for (const auto& x : xs.first) { add("b", cw + x.a0, 0, cw + x.a1, ch, x.crop); add("t", cw + x.a0, in.h - ch, cw + x.a1, in.h, x.crop); }
        for (const auto& y : ys.first) { add("l", 0, ch + y.a0, cw, ch + y.a1, 1, y.crop); add("r", in.w - cw, ch + y.a0, in.w, ch + y.a1, 1, y.crop); }
        for (const auto& x : xs.first) for (const auto& y : ys.first) add("c", cw + x.a0, ch + y.a0, cw + x.a1, ch + y.a1, x.crop, y.crop);
        return res;
    }

    auto usable = [](const KitPiece& p) {
        if (p.cells.empty()) return false;
        for (const auto& r : p.cells) { if (r.size() != p.cells[0].size()) return false; for (const auto& t : r) if (!t) return false; }
        return true;
    };
    std::map<std::string, KitLane> lanes;
    const std::pair<const char*, const KitLane*> laneList[3] = {{"top", &k.top}, {"middle", &k.middle}, {"bottom", &k.bottom}};
    for (const auto& [name, l] : laneList) {
        KitLane u;
        u.rows = l->rows;
        for (const auto& p : l->pieces) if (usable(p)) u.pieces.push_back(p);
        if (l->pieces.size() > u.pieces.size()) warn(std::string(name) + ": a piece without all its textures is not used");
        lanes[name] = u;
    }
    const bool hasB = !lanes["bottom"].pieces.empty(), hasT = !lanes["top"].pieces.empty(), hasM = !lanes["middle"].pieces.empty();
    const int unit = hasM ? lanes["middle"].rows : 0;
    if (!hasB && !hasM && !hasT) { fill(0, 0, in.w, in.h, "the set has no usable piece yet"); return res; }

    struct Band { double y0, y1, crop; };
    auto bandsOf = [](double y0, int count, double h) { std::vector<Band> b; for (int r = 0; r < count; ++r) b.push_back({y0 + r * h, y0 + (r + 1) * h, 1}); return b; };
    struct Unit { std::string lane, key; std::vector<Band> bands; };
    std::vector<Unit> units;
    struct Cfg { bool b, m, t; int bRows, nM, tRows, R; double stretch; };
    auto make = [&](bool b, bool m, bool t) -> std::optional<Cfg> {
        const int bRows = b ? lanes["bottom"].rows : 0, tRows = t ? lanes["top"].rows : 0;
        int nM = 0;
        if (m) { const double fr = in.h - (bRows + tRows) * th; nM = fr <= 0 ? 1 : static_cast<int>(bestCount(fr / (unit * th))); }
        const int R = bRows + nM * unit + tRows;
        if (R <= 0) return std::nullopt;
        return Cfg{b, m, t, bRows, nM, tRows, R, stretchOf(in.h / (R * th))};
    };
    const bool orderList[7][3] = {{true, true, true}, {true, true, false}, {false, true, true}, {false, true, false}, {true, false, true}, {true, false, false}, {false, false, true}};
    std::vector<std::array<bool, 3>> usableCfgs;
    for (const auto& o : orderList)
        if ((!o[0] || hasB) && (!o[1] || hasM) && (!o[2] || hasT) && (o[0] || o[1] || o[2])) usableCfgs.push_back({o[0], o[1], o[2]});
    auto left = [&](bool b, bool m, bool t) {
        std::vector<std::string> l;
        if (hasT && !t) l.push_back("rooftop");
        if (hasB && !b) l.push_back("ground");
        if (hasM && !m) l.push_back("middle");
        if (l.empty()) return;
        std::string joined;
        for (size_t i = 0; i < l.size(); ++i) joined += (i ? " and " : "") + l[i];
        warn("the " + joined + " row" + (l.size() > 1 ? "s are" : " is") + " left out: this wall is too " + (in.h < th * 3 ? "low" : "short") +
             " for it without stretching the tiles more than " + pct(lim) + " %");
    };
    auto scaled = [&](const Cfg& c) {
        const double rowH = in.h / c.R;
        if (c.b) units.push_back({"bottom", "bottom", bandsOf(0, c.bRows, rowH)});
        for (int m = 0; m < c.nM; ++m) units.push_back({"middle", "middle" + std::to_string(m), bandsOf((c.bRows + m * unit) * rowH, unit, rowH)});
        if (c.t) units.push_back({"top", "top", bandsOf((c.bRows + c.nM * unit) * rowH, c.tRows, rowH)});
    };
    if (lim <= 0) {
        const int bRows = hasB ? lanes["bottom"].rows : 0, tRows = hasT ? lanes["top"].rows : 0;
        const double fr = in.h - (bRows + tRows) * th;
        const int nM = unit ? (fr >= unit * th * 0.5 ? std::max(1, static_cast<int>(std::floor(fr / (unit * th) + 0.5))) : 0) : 0;
        const int R = bRows + nM * unit + tRows;
        scaled({hasB, nM > 0, hasT, bRows, nM, tRows, R, stretchOf(in.h / (R * th))});
    } else {
        std::vector<Cfg> cfgs;
        for (const auto& c : usableCfgs) if (auto x = make(c[0], c[1], c[2])) cfgs.push_back(*x);
        const Cfg* ok = nullptr;
        for (const auto& c : cfgs) if (c.stretch <= lim + 1e-9) { ok = &c; break; }
        if (ok) { scaled(*ok); left(ok->b, ok->m, ok->t); }
        else if (hasM) {
            std::array<bool, 3> c{false, true, false};
            for (const auto& u : usableCfgs)
                if (u[1] && ((u[0] ? lanes["bottom"].rows : 0) + (u[2] ? lanes["top"].rows : 0)) * th <= in.h + 1e-9) { c = u; break; }
            const bool b = c[0], t = c[2];
            const int bRows = b ? lanes["bottom"].rows : 0, tRows = t ? lanes["top"].rows : 0;
            if (b) units.push_back({"bottom", "bottom", bandsOf(0, bRows, th)});
            const double base = bRows * th, fr = in.h - (bRows + tRows) * th;
            const auto mid = axisTiles(fr, unit * th, lim, true);
            for (size_t m = 0; m < mid.first.size(); ++m) {
                const KSeg& seg = mid.first[m];
                std::vector<Band> bands;
                for (int r = 0; r < unit; ++r) {
                    const double y0 = base + seg.a0 + r * th, y1 = std::min(base + seg.a1, y0 + th);
                    if (y1 - y0 > 1e-9) bands.push_back({y0, y1, (y1 - y0) / th});
                }
                units.push_back({"middle", "middle" + std::to_string(m), bands});
            }
            if (t) units.push_back({"top", "top", bandsOf(in.h - tRows * th, tRows, th)});
            left(b, true, t);
            if (mid.second) warn("the top middle row is cut off at " + pct(mid.first.back().crop) + " %: tiles keep their size (stretching them more than " + pct(lim) + " % is not allowed)");
        } else {
            const Cfg* best = nullptr;
            for (const auto& c : cfgs) if (!best || c.stretch < best->stretch - 1e-9) best = &c;
            if (best) {
                scaled(*best);
                left(best->b, best->m, best->t);
                warn("the tiles are scaled by " + pct(best->stretch) + " % in height (limit " + pct(lim) + " %): add a Middle row to the set so a wall of this height can be cut instead of stretched");
            }
        }
    }
    const auto colsOf = axisTiles(in.w, tw, lim);
    const auto& cols = colsOf.first;
    if (colsOf.second) warn("the last tile column is cut off at " + pct(cols.back().crop) + " %: tiles keep their size (stretching them more than " + pct(lim) + " % is not allowed)");
    long long bandCount = 0;
    for (const auto& u : units) bandCount += static_cast<long long>(u.bands.size());
    if (tooMany(static_cast<long long>(cols.size()) * bandCount)) return res;
    std::vector<std::optional<std::string>> below(cols.size());
    for (const auto& u : units) {
        const KitLane& lane = lanes[u.lane];
        const RowOut row = placeRow(lane.pieces, static_cast<int>(cols.size()), in.seed, u.key, k.avoidRepeat, below, warn);
        below = row.tags;
        for (const auto& pl : row.placements) {
            const auto& pc = pl.piece->cells;
            for (size_t r = 0; r < pc.size(); ++r)
                for (size_t c = 0; c < pc[r].size(); ++c) {
                    const long long bi = static_cast<long long>(pc.size()) - 1 - static_cast<long long>(r);
                    const size_t ci = static_cast<size_t>(pl.col) + c;
                    if (bi < 0 || bi >= static_cast<long long>(u.bands.size()) || ci >= cols.size()) continue;
                    const Band& band = u.bands[static_cast<size_t>(bi)];
                    const KSeg& seg = cols[ci];
                    cells.push_back(kcell(seg.a0, band.y0, seg.a1, band.y1, *pc[r][c], u.key, seg.crop, band.crop));
                }
        }
        for (int c : row.gaps) for (const auto& band : u.bands) cells.push_back(kcell(cols[static_cast<size_t>(c)].a0, band.y0, cols[static_cast<size_t>(c)].a1, band.y1, fallback, "gap", cols[static_cast<size_t>(c)].crop, band.crop));
    }
    return res;
}
}  // namespace

LayoutResult solveLayout(const LayoutInput& in) {
    if (in.group->kit) {
        LayoutResult r = solveKit(*in.group->kit, in);
        return r;
    }
    Solver s(in);
    s.run();
    LayoutResult r;
    r.cells = s.truncated ? std::vector<Cell>{} : s.cells;
    r.warnings = s.warnings;
    r.truncated = s.truncated;
    return r;
}

// ---- layoutPlan.ts ------------------------------------------------------------------------------------------------------------------------------------
namespace {
V3 sub3(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 cross3(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double dot3(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 unit3(const V3& a) { double l = std::sqrt(dot3(a, a)); if (l == 0) l = 1; return {a[0] / l, a[1] / l, a[2] / l}; }
V3 neg3(const V3& a) { return {-a[0], -a[1], -a[2]}; }
double at2(const Aff2& m, double x, double y) { return m[0] * x + m[1] * y + m[2]; }
}  // namespace

std::optional<Frame> layoutFrame(const Brush& b, const BrushFace& f, int rotate, bool flipU) {
    const V3 n = faceNormalOf(b, f);
    if (!(std::sqrt(dot3(n, n)) > 0.5) || f.loop.size() < 3) return std::nullopt;
    const V3 up = std::fabs(n[1]) > 0.999 ? V3{0, 0, -1} : V3{0, 1, 0};
    V3 ex = unit3(cross3(up, n)), ey = unit3(cross3(n, ex));
    for (int k = 0; k < rotate / 90; ++k) { const V3 t = ex; ex = ey; ey = neg3(t); }
    if (flipU) ex = neg3(ex);
    double xmin = 1e300, xmax = -1e300, ymin = 1e300, ymax = -1e300;
    std::vector<V3> pts;
    for (uint32_t i : f.loop) pts.push_back(v3(b.vertices[i]));
    for (const V3& p : pts) { const double x = dot3(ex, p), y = dot3(ey, p); xmin = std::min(xmin, x); xmax = std::max(xmax, x); ymin = std::min(ymin, y); ymax = std::max(ymax, y); }
    Frame fr;
    fr.ex = ex; fr.ey = ey; fr.dx = -xmin; fr.dy = -ymin; fr.w = xmax - xmin; fr.h = ymax - ymin; fr.normal = n; fr.q = pts[0];
    fr.xq = dot3(ex, pts[0]) - xmin; fr.yq = dot3(ey, pts[0]) - ymin;
    return fr;
}

CellMap cellMap(const Cell& c, const V2& tex, double fw, double fh) {
    const double cw = c.x1 - c.x0, ch = c.y1 - c.y0;
    const bool faceOrigin = c.origin == "face" && c.fit == "tile" && c.rotate == 0;
    auto local = [&](double x, double y) -> std::array<double, 2> {
        if (faceOrigin) return {c.flipU ? fw - x : x, c.flipV ? fh - y : y};
        const double px = c.flipU ? c.x1 - x : x - c.x0, py = c.flipV ? c.y1 - y : y - c.y0;
        if (c.rotate == 90) return {py, cw - px};
        if (c.rotate == 180) return {cw - px, ch - py};
        if (c.rotate == 270) return {ch - py, px};
        return {px, py};
    };
    const bool swap = c.rotate == 90 || c.rotate == 270;
    const double A = faceOrigin ? fw : swap ? ch : cw, B = faceOrigin ? fh : swap ? cw : ch;
    const double nw = tex[0], nh = tex[1];
    double ew = nw, eh = nh;
    if (c.fit == "stretch") { ew = A / std::max(c.cropU ? *c.cropU : 1.0, 1e-6); eh = B / std::max(c.cropV ? *c.cropV : 1.0, 1e-6); }
    else if (c.fit == "width") { ew = A; eh = (nh * A) / nw; }
    else if (c.fit == "height") { eh = B; ew = (nw * B) / nh; }
    auto fn = [&](double x, double y) -> std::array<double, 2> {
        const auto ab = local(x, y);
        const double a = ab[0], b = ab[1];
        const double u = c.alignU == "center" ? 0.5 + (a - A / 2) / ew : c.alignU == "end" ? 1 + (a - A) / ew : a / ew;
        const double v = c.alignV == "center" ? 0.5 - (b - B / 2) / eh : c.alignV == "end" ? (B - b) / eh : 1 - b / eh;
        return {u, v};
    };
    const auto o = fn(0, 0), px = fn(1, 0), py = fn(0, 1);
    return {{px[0] - o[0], py[0] - o[0], o[0]}, {px[1] - o[1], py[1] - o[1], o[1]}};
}

namespace {
struct XY { double x, y; V3 w; };
std::vector<XY> clipXY(const std::vector<XY>& poly, int k, double bound, bool ge) {
    std::vector<XY> out;
    auto val = [k](const XY& p) { return k == 0 ? p.x : p.y; };
    auto inside = [&](const XY& p) { return ge ? val(p) >= bound : val(p) <= bound; };
    for (size_t i = 0; i < poly.size(); ++i) {
        const XY& a = poly[i];
        const XY& b = poly[(i + 1) % poly.size()];
        const bool ia = inside(a), ib = inside(b);
        if (ia) out.push_back(a);
        if (ia != ib) {
            const double t = (bound - val(a)) / (val(b) - val(a));
            XY p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, {a.w[0] + (b.w[0] - a.w[0]) * t, a.w[1] + (b.w[1] - a.w[1]) * t, a.w[2] + (b.w[2] - a.w[2]) * t}};
            (k == 0 ? p.x : p.y) = bound;
            out.push_back(p);
        }
    }
    return out;
}
double areaXY(const std::vector<XY>& p) { double s = 0; for (size_t i = 0; i < p.size(); ++i) { const XY& a = p[i]; const XY& b = p[(i + 1) % p.size()]; s += a.x * b.y - b.x * a.y; } return std::fabs(s) / 2; }
V3 lerpTri(const std::array<V3, 3>& t, const V3& w) {
    return {t[0][0] * w[0] + t[1][0] * w[1] + t[2][0] * w[2], t[0][1] * w[0] + t[1][1] * w[1] + t[2][1] * w[2], t[0][2] * w[0] + t[1][2] * w[1] + t[2][2] * w[2]};
}
}  // namespace

PlanPtr buildLayoutPlan(const Frame& frame, const std::vector<Cell>& cells, const std::string& fallback, const InfoFn& info) {
    struct Prepared { Cell cell; TexInfo info; CellMap map; PlanPtr sub; };
    auto prepared = std::make_shared<std::vector<Prepared>>();
    std::vector<std::string> paths{fallback};
    auto addPath = [&](const std::string& p) { if (std::find(paths.begin(), paths.end(), p) == paths.end()) paths.push_back(p); };
    for (const Cell& cell : cells) {
        const TexInfo* ti = info ? info(cell.texture) : nullptr;
        if (!ti || !(ti->width > 0 && ti->height > 0 && ti->ppm > 0)) return nullptr;
        addPath(cell.texture);
        const CellMap map = cellMap(cell, naturalSize(*ti), frame.w, frame.h);
        PlanPtr sub;
        if (!isPlainProps(ti->props) || edgeClamp(*ti, 0) || edgeClamp(*ti, 1)) {
            auto world = [&](const Aff2& m) -> Aff {
                return {m[0] * frame.ex[0] + m[1] * frame.ey[0], m[0] * frame.ex[1] + m[1] * frame.ey[1], m[0] * frame.ex[2] + m[1] * frame.ey[2], m[0] * frame.dx + m[1] * frame.dy + m[2]};
            };
            const UvAffine wm{world(map.u), world(map.v)};
            const bool whole = cell.origin == "face" && cell.fit == "tile" && cell.rotate == 0;
            const double xs[2] = {whole ? 0 : cell.x0, whole ? frame.w : cell.x1}, ys[2] = {whole ? 0 : cell.y0, whole ? frame.h : cell.y1};
            Extent ext;
            for (double x : xs) for (double y : ys) {
                const double u = at2(map.u, x, y), v = at2(map.v, x, y);
                ext.umin = std::min(ext.umin, u); ext.umax = std::max(ext.umax, u); ext.vmin = std::min(ext.vmin, v); ext.vmax = std::max(ext.vmax, v);
            }
            sub = makePlan(*ti, info, wm, ext);
            if (sub) for (const auto& t : sub->textures) addPath(t);
        }
        prepared->push_back({cell, *ti, map, sub});
    }
    if (prepared->empty()) return nullptr;
    auto plan = std::make_shared<FacePlan>();
    plan->texture = fallback;
    plan->textures = paths;
    const Frame fr = frame;
    plan->pieces = [prepared, fr](const std::array<V3, 3>& tri) {
        std::vector<XY> verts;
        for (int i = 0; i < 3; ++i)
            verts.push_back({dot3(fr.ex, tri[static_cast<size_t>(i)]) + fr.dx, dot3(fr.ey, tri[static_cast<size_t>(i)]) + fr.dy, {i == 0 ? 1.0 : 0.0, i == 1 ? 1.0 : 0.0, i == 2 ? 1.0 : 0.0}});
        double xmin = 1e300, xmax = -1e300, ymin = 1e300, ymax = -1e300;
        for (const XY& v : verts) { xmin = std::min(xmin, v.x); xmax = std::max(xmax, v.x); ymin = std::min(ymin, v.y); ymax = std::max(ymax, v.y); }
        std::vector<Piece> out;
        for (const Prepared& pc : *prepared) {
            const Cell& c = pc.cell;
            if (c.x1 < xmin - 1e-9 || c.x0 > xmax + 1e-9 || c.y1 < ymin - 1e-9 || c.y0 > ymax + 1e-9) continue;
            const auto poly = clipXY(clipXY(clipXY(clipXY(verts, 0, c.x0, true), 0, c.x1, false), 1, c.y0, true), 1, c.y1, false);
            if (poly.size() < 3 || areaXY(poly) < 1e-10) continue;
            if (!pc.sub) {
                Piece p{c.texture, {}};
                for (const XY& q : poly) p.verts.push_back({q.w, {at2(pc.map.u, q.x, q.y), at2(pc.map.v, q.x, q.y)}});
                out.push_back(std::move(p));
                continue;
            }
            for (size_t k = 1; k + 1 < poly.size(); ++k) {
                const XY sub[3] = {poly[0], poly[k], poly[k + 1]};
                const std::array<V3, 3> world = {lerpTri(tri, sub[0].w), lerpTri(tri, sub[1].w), lerpTri(tri, sub[2].w)};
                for (const Piece& piece : pc.sub->pieces(world)) {
                    Piece p{piece.texture, {}};
                    for (const PieceVert& v : piece.verts) {
                        V3 w;
                        for (int m = 0; m < 3; ++m) w[static_cast<size_t>(m)] = v.w[0] * sub[0].w[static_cast<size_t>(m)] + v.w[1] * sub[1].w[static_cast<size_t>(m)] + v.w[2] * sub[2].w[static_cast<size_t>(m)];
                        p.verts.push_back({w, v.uv});
                    }
                    out.push_back(std::move(p));
                }
            }
        }
        return out;
    };
    return plan;
}

PlanPtr layoutPlanFor(const Brush& b, const BrushFace& f, const FaceLayout& layout, const InfoFn& info, const GroupFn& groups) {
    const GroupPtr group = groups ? groups(layout.group) : nullptr;
    if (!group) return nullptr;
    const auto frame = layoutFrame(b, f, layout.rotate, layout.flipU);
    if (!frame) return nullptr;
    LayoutInput in;
    in.group = group.get();
    in.params = layout.params;
    in.w = frame->w;
    in.h = frame->h;
    const double ny = frame->normal[1];
    in.facing = ny > 0.5 ? "up" : ny < -0.5 ? "down" : "wall";
    in.tilt = std::acos(std::max(-1.0, std::min(1.0, ny))) * 180 / 3.14159265358979323846;
    in.seed = layout.seed ? *layout.seed : faceSeed(b.id, f.id);
    const Frame fr = *frame;
    in.worldY = [fr](double x, double y) { return fr.q[1] + fr.ex[1] * (x - fr.xq) + fr.ey[1] * (y - fr.yq); };
    in.info = info;
    in.fallbackTexture = f.texture.empty() ? "dev/grey" : f.texture;
    in.hasFallbackTexture = true;
    const LayoutResult result = solveLayout(in);
    if (result.truncated || result.cells.empty()) return nullptr;
    const auto fb = groupFallbackTexture(*group);
    return buildLayoutPlan(*frame, result.cells, fb ? *fb : (f.texture.empty() ? "dev/grey" : f.texture), info);
}

}  // namespace core::texplan
