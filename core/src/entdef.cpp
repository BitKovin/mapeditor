#include "core/entdef.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_set>

using nlohmann::json;
namespace fs = std::filesystem;

namespace core {

namespace {

constexpr size_t kMaxFileBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxClasses = 20000;
constexpr size_t kMaxText = 4096;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string decodeEntities(const std::string& s) {
    if (s.find('&') == std::string::npos) return s;
    static const std::pair<const char*, char> kTable[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}};
    std::string out;
    for (size_t i = 0; i < s.size();) {
        bool done = false;
        if (s[i] == '&') {
            for (const auto& [name, ch] : kTable) {
                const size_t n = std::char_traits<char>::length(name);
                if (s.compare(i, n, name) == 0) { out += ch; i += n; done = true; break; }
            }
        }
        if (!done) out += s[i++];
    }
    return out;
}

// "1 0.5 0" / "1, 0.5, 0" -> numbers. Returns how many were read (up to `max`).
size_t numbers(const std::string& s, float* out, size_t max) {
    size_t n = 0;
    const char* p = s.c_str();
    while (n < max) {
        while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ',')) ++p;
        if (!*p) break;
        char* end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p) break;
        out[n++] = static_cast<float>(v);
        p = end;
    }
    return n;
}

struct Tag {
    enum class Type { Open, Close, SelfClose } type = Type::Open;
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string attr(const char* key) const {
        for (const auto& [k, v] : attrs) if (k == key) return v;
        return {};
    }
    bool has(const char* key) const {
        for (const auto& [k, v] : attrs) if (k == key) return true;
        return false;
    }
};

// Scans text into tags and the text between them.
class Scanner {
public:
    explicit Scanner(std::string_view t) : t_(t) {}

    struct Token {
        bool isTag = false;
        Tag tag;
        std::string text;
    };

    bool next(Token& tok) {
        tok = Token{};
        while (i_ < t_.size()) {
            if (t_[i_] != '<') {
                const size_t j = t_.find('<', i_);
                const size_t end = j == std::string_view::npos ? t_.size() : j;
                tok.text = decodeEntities(std::string(t_.substr(i_, end - i_)));
                i_ = end;
                return true;
            }
            if (t_.compare(i_, 4, "<!--") == 0) { skipPast("-->", 4); continue; }
            if (t_.compare(i_, 2, "<?") == 0) { skipPast("?>", 2); continue; }
            if (t_.compare(i_, 9, "<![CDATA[") == 0) {
                const size_t s = i_ + 9, e = t_.find("]]>", s);
                tok.text = std::string(t_.substr(s, (e == std::string_view::npos ? t_.size() : e) - s));
                i_ = e == std::string_view::npos ? t_.size() : e + 3;
                return true;
            }
            if (t_.compare(i_, 2, "<!") == 0) { skipPast(">", 1); continue; }
            if (parseTag(tok.tag)) { tok.isTag = true; return true; }
            // a lone '<' that isn't a tag: treat it as text
            tok.text = "<";
            ++i_;
            return true;
        }
        return false;
    }

private:
    void skipPast(const char* marker, size_t skip) {
        const size_t e = t_.find(marker, i_ + skip);
        i_ = e == std::string_view::npos ? t_.size() : e + std::char_traits<char>::length(marker);
    }
    bool isNameChar(char c) const { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ':' || c == '.'; }

    bool parseTag(Tag& tag) {
        size_t p = i_ + 1;
        tag = Tag{};
        if (p < t_.size() && t_[p] == '/') { tag.type = Tag::Type::Close; ++p; }
        const size_t nameStart = p;
        while (p < t_.size() && isNameChar(t_[p])) ++p;
        if (p == nameStart) return false;
        tag.name = lower(std::string(t_.substr(nameStart, p - nameStart)));
        while (p < t_.size()) {
            while (p < t_.size() && std::isspace(static_cast<unsigned char>(t_[p]))) ++p;
            if (p >= t_.size()) break;
            if (t_[p] == '>') { ++p; i_ = p; return true; }
            if (t_[p] == '/') {
                if (tag.type == Tag::Type::Open) tag.type = Tag::Type::SelfClose;
                ++p;
                continue;
            }
            const size_t ks = p;
            while (p < t_.size() && isNameChar(t_[p])) ++p;
            if (p == ks) { ++p; continue; }  // garbage character inside a tag
            std::string key = lower(std::string(t_.substr(ks, p - ks)));
            while (p < t_.size() && std::isspace(static_cast<unsigned char>(t_[p]))) ++p;
            std::string value;
            if (p < t_.size() && t_[p] == '=') {
                ++p;
                while (p < t_.size() && std::isspace(static_cast<unsigned char>(t_[p]))) ++p;
                if (p < t_.size() && (t_[p] == '"' || t_[p] == '\'')) {
                    const char q = t_[p++];
                    const size_t vs = p;
                    while (p < t_.size() && t_[p] != q) ++p;
                    value = std::string(t_.substr(vs, p - vs));
                    if (p < t_.size()) ++p;
                } else {
                    const size_t vs = p;
                    while (p < t_.size() && !std::isspace(static_cast<unsigned char>(t_[p])) && t_[p] != '>' && t_[p] != '/') ++p;
                    value = std::string(t_.substr(vs, p - vs));
                }
            }
            tag.attrs.emplace_back(std::move(key), decodeEntities(value));
        }
        i_ = t_.size();  // unterminated tag at end of file
        return true;
    }

    std::string_view t_;
    size_t i_ = 0;
};

// Description text: drop the old ".def"-style section banners ("-------- KEYS --------" and everything
// after them) and cap the size.
std::string cleanDescription(const std::string& raw) {
    std::istringstream in(raw);
    std::string line, out;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.rfind("--------", 0) == 0) break;
        out += t;
        out += '\n';
    }
    out = trim(out);
    if (out.size() > kMaxText) out.resize(kMaxText);
    return out;
}

std::array<float, 3> parseColor(const std::string& s, bool& ok) {
    float v[3];
    ok = numbers(s, v, 3) == 3;
    std::array<float, 3> c{0.6f, 0.6f, 0.6f};
    if (ok) for (int i = 0; i < 3; ++i) c[i] = std::clamp(v[i], 0.0f, 1.0f);
    return c;
}

// Position of the lowest set bit of a power-of-two value, or -1.
int bitOfValue(long v) {
    if (v <= 0 || (v & (v - 1)) != 0) return -1;
    int b = 0;
    while (!(v & 1)) { v >>= 1; ++b; }
    return b;
}

}  // namespace

const EntClass* EntityDefinitions::find(const std::string& classname) const {
    for (const auto& c : classes) if (c.name == classname) return &c;
    return nullptr;
}

bool EntityDefinitions::isBrushClass(const std::string& classname) const {
    const EntClass* c = find(classname);
    return c && c->kind == EntClass::Kind::Group;
}

json EntityDefinitions::toJson() const {
    json arr = json::array();
    for (const auto& c : classes) {
        json keys = json::array();
        for (const auto& k : c.keys) {
            json kj{{"key", k.key}, {"type", k.type}, {"name", k.name}, {"description", k.description}};
            if (k.defaultValue) kj["default"] = *k.defaultValue;
            if (k.min) kj["min"] = *k.min;
            if (k.max) kj["max"] = *k.max;
            if (k.step) kj["step"] = *k.step;
            if (!k.items.empty()) {
                json items = json::array();
                for (const auto& [label, value] : k.items) items.push_back(json{{"name", label}, {"value", value}});
                kj["items"] = items;
            }
            keys.push_back(std::move(kj));
        }
        json flags = json::array();
        for (const auto& f : c.flags)
            flags.push_back(json{{"bit", f.bit}, {"key", f.key}, {"name", f.name}, {"description", f.description}});
        arr.push_back(json{{"name", c.name},
                           {"kind", c.kind == EntClass::Kind::Point ? "point" : "group"},
                           {"color", c.color},
                           {"box", c.box ? json(*c.box) : json(nullptr)},
                           {"description", c.description},
                           {"keys", keys},
                           {"flags", flags}});
    }
    return json{{"type", "entity_defs"}, {"classes", arr}, {"warnings", warnings}};
}

EntityDefinitions parseEntityDefinitions(std::string_view text) {
    EntityDefinitions defs;
    auto warn = [&](const std::string& m) { if (defs.warnings.size() < 50) defs.warnings.push_back(m); };

    Scanner scan(text);
    Scanner::Token tok;
    std::unordered_set<std::string> seen;

    std::optional<EntClass> cur;   // class being read
    std::string curText;           // free text directly inside the class
    std::optional<EntKey> key;     // key element being read (its text is the description)
    std::optional<EntFlag> flag;
    std::string keyText;
    bool skipClass = false;        // a class we decided to drop; still consume it up to its close tag

    auto finishKey = [&] {
        if (key && cur) { key->description = cleanDescription(keyText); cur->keys.push_back(std::move(*key)); }
        key.reset(); keyText.clear();
    };
    auto finishFlag = [&] {
        if (flag && cur) { flag->description = cleanDescription(keyText); cur->flags.push_back(std::move(*flag)); }
        flag.reset(); keyText.clear();
    };
    auto finishClass = [&] {
        finishKey(); finishFlag();
        if (cur && !skipClass) {
            cur->description = cleanDescription(curText);
            if (defs.classes.size() >= kMaxClasses) warn("too many classes; the rest are ignored");
            else defs.classes.push_back(std::move(*cur));
        }
        cur.reset(); curText.clear(); skipClass = false;
    };

    while (scan.next(tok)) {
        if (!tok.isTag) {
            if (key || flag) keyText += tok.text;
            else if (cur) curText += tok.text;
            continue;
        }
        const Tag& t = tok.tag;
        const bool isClassTag = t.name == "point" || t.name == "group";

        if (t.type == Tag::Type::Close) {
            if (isClassTag) finishClass();
            else if (key && t.name == key->type) finishKey();
            else if (flag && t.name == "flag") finishFlag();
            continue;
        }

        if (isClassTag) {
            finishClass();  // a missing close tag on the previous class must not swallow this one
            EntClass c;
            c.name = trim(t.attr("name"));
            c.kind = t.name == "point" ? EntClass::Kind::Point : EntClass::Kind::Group;
            skipClass = false;
            if (c.name.empty()) { warn("a <" + t.name + "> without a name was skipped"); skipClass = true; }
            else if (!seen.insert(c.name).second) { warn("duplicate class '" + c.name + "' ignored"); skipClass = true; }
            if (t.has("color")) {
                bool ok = false;
                c.color = parseColor(t.attr("color"), ok);
                if (!ok) warn("class '" + c.name + "': bad color '" + t.attr("color") + "'");
            }
            if (t.has("box")) {
                float v[6];
                if (numbers(t.attr("box"), v, 6) == 6) {
                    for (int i = 0; i < 3; ++i) if (v[i] > v[i + 3]) std::swap(v[i], v[i + 3]);
                    c.box = std::array<float, 6>{v[0], v[1], v[2], v[3], v[4], v[5]};
                } else warn("class '" + c.name + "': bad box '" + t.attr("box") + "'");
            }
            cur = std::move(c);
            curText.clear();
            if (t.type == Tag::Type::SelfClose) finishClass();
            continue;
        }
        if (!cur) continue;  // <classes> wrapper, unknown top-level elements

        if (t.name == "item" && key) {  // <list> option
            key->items.emplace_back(t.attr("name"), t.has("value") ? t.attr("value") : t.attr("name"));
            continue;
        }
        if (t.name == "flag") {
            finishKey(); finishFlag();
            EntFlag f;
            f.key = t.attr("key");
            f.name = t.has("name") ? t.attr("name") : f.key;
            int bit = -1;
            if (t.has("bit")) bit = std::atoi(t.attr("bit").c_str());
            else if (t.has("value")) bit = bitOfValue(std::atol(t.attr("value").c_str()));
            if (bit < 0 || bit > 31) { warn("class '" + cur->name + "': flag '" + f.key + "' has no usable bit"); }
            else { f.bit = bit; flag = std::move(f); keyText.clear(); }
            if (t.type == Tag::Type::SelfClose) finishFlag();
            continue;
        }
        if (t.has("key")) {  // any element with a key attribute is a key of that type
            finishKey(); finishFlag();
            EntKey k;
            k.key = t.attr("key");
            k.type = t.name;
            k.name = t.has("name") ? t.attr("name") : k.key;
            if (t.has("default")) k.defaultValue = t.attr("default");
            auto number = [&](const char* attr, std::optional<double>& out) {
                if (!t.has(attr)) return;
                char* end = nullptr;
                const std::string v = t.attr(attr);
                const double d = std::strtod(v.c_str(), &end);
                if (end != v.c_str() && *end == '\0') out = d;
                else warn("class '" + (cur ? cur->name : std::string()) + "': key '" + k.key + "': bad " + attr + " '" + v + "'");
            };
            number("min", k.min); number("max", k.max); number("step", k.step);
            key = std::move(k);
            keyText.clear();
            if (t.type == Tag::Type::SelfClose) finishKey();
            continue;
        }
        // anything else inside a class (<newline/>, unknown markup) is ignored
    }
    finishClass();
    return defs;
}

EntityDefinitions loadEntityDefinitions(const fs::path& file) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return {};
    const auto size = fs::file_size(file, ec);
    EntityDefinitions none;
    if (ec) { none.warnings.push_back("could not read " + file.filename().string()); return none; }
    if (size > kMaxFileBytes) { none.warnings.push_back(file.filename().string() + " is too large (max 8 MB)"); return none; }
    std::ifstream in(file, std::ios::binary);
    if (!in) { none.warnings.push_back("could not open " + file.filename().string()); return none; }
    std::stringstream ss;
    ss << in.rdbuf();
    return parseEntityDefinitions(ss.str());
}

}  // namespace core
