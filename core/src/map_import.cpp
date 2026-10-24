#include "core/map_import.h"
#include "core/albedo.h"
#include "core/brush.h"
#include "core/document.h"
#include "core/entity_class.h"
#include "core/textures.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

using nlohmann::json;

namespace core {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------------------------------------------------------------------------

struct Token {
    std::string text;
    bool quoted = false;
    int line = 0;
};

[[noreturn]] void fail(int line, const std::string& what) { throw std::runtime_error("line " + std::to_string(line) + ": " + what); }

std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    int line = 1;
    size_t i = 0;
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) i = 3;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\0') { ++i; continue; }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') { while (i < s.size() && s[i] != '\n') ++i; continue; }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) { if (s[i] == '\n') ++line; ++i; }
            i += 2;
            continue;
        }
        if (c == '"') {
            Token t;
            t.quoted = true;
            t.line = line;
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\n') ++line;
                t.text += s[i++];
            }
            if (i >= s.size()) fail(t.line, "unterminated string");
            ++i;
            out.push_back(std::move(t));
            continue;
        }
        if (c == '{' || c == '}' || c == '(' || c == ')' || c == '[' || c == ']') {
            out.push_back({std::string(1, c), false, line});
            ++i;
            continue;
        }
        Token t;
        t.line = line;
        while (i < s.size()) {
            const char d = s[i];
            if (d == ' ' || d == '\t' || d == '\r' || d == '\n' || d == '\0' || d == '{' || d == '}' || d == '(' || d == ')' || d == '[' || d == ']' || d == '"') break;
            if (d == '/' && i + 1 < s.size() && (s[i + 1] == '/' || s[i + 1] == '*')) break;
            t.text += d;
            ++i;
        }
        out.push_back(std::move(t));
    }
    return out;
}

bool toNumber(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size() && std::isfinite(v);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Parsed map
// ---------------------------------------------------------------------------------------------------------------------------------------------

enum class TexKind { Classic, Valve, Primitive };

struct FaceDef {
    Vec3 p[3];
    std::string texture;
    TexKind kind = TexKind::Classic;
    double shift[2] = {0, 0}, rotate = 0, scale[2] = {1, 1};  // classic / Valve
    Vec3 axis[2];                                             // Valve 220
    double axisOffset[2] = {0, 0};
    double matrix[2][3] = {{1, 0, 0}, {0, 1, 0}};             // brush primitives
    long long contents = 0, surface = 0, value = 0;
    int line = 0;
};

struct PatchDef {
    std::string texture;
    int width = 0, height = 0;
    bool explicitSubdivisions = false;
    std::vector<std::vector<std::array<double, 5>>> grid;  // grid[column][row] = x y z s t (the file's groups are the columns)
    int line = 0;
};

struct Item {
    bool patch = false;
    std::vector<FaceDef> faces;
    PatchDef patchDef;
    int line = 0;
};

struct EntityDef {
    std::vector<std::pair<std::string, std::string>> keys;
    std::vector<Item> items;
    int line = 0;
    int index = 0;

    const std::string* find(const std::string& key) const {
        for (const auto& [k, v] : keys) if (k == key) return &v;
        return nullptr;
    }
    std::string get(const std::string& key, const std::string& dflt = {}) const {
        const std::string* v = find(key);
        return v ? *v : dflt;
    }
};

class Parser {
public:
    explicit Parser(std::vector<Token> t) : t_(std::move(t)) {}

    std::vector<EntityDef> parse() {
        std::vector<EntityDef> out;
        while (pos_ < t_.size()) {
            const Token& open = next();
            if (open.quoted || open.text != "{") fail(open.line, "expected '{' to start an entity, found '" + open.text + "'");
            EntityDef e;
            e.line = open.line;
            e.index = static_cast<int>(out.size());
            entityBody(e);
            out.push_back(std::move(e));
        }
        return out;
    }

private:
    std::vector<Token> t_;
    size_t pos_ = 0;
    int lastLine_ = 1;

    const Token& next() {
        if (pos_ >= t_.size()) fail(lastLine_, "unexpected end of file");
        lastLine_ = t_[pos_].line;
        return t_[pos_++];
    }
    const Token* peek() const { return pos_ < t_.size() ? &t_[pos_] : nullptr; }
    bool peekIs(const char* s) const { const Token* t = peek(); return t && !t->quoted && t->text == s; }
    void expect(const char* s) {
        const Token& t = next();
        if (t.quoted || t.text != s) fail(t.line, std::string("expected '") + s + "', found '" + t.text + "'");
    }
    double number() {
        const Token& t = next();
        double v;
        if (t.quoted || !toNumber(t.text, v)) fail(t.line, "expected a number, found '" + t.text + "'");
        return v;
    }
    Vec3 point() {
        expect("(");
        Vec3 p;
        p.x = number(); p.y = number(); p.z = number();
        expect(")");
        return p;
    }
    bool peekNumber() const {
        const Token* t = peek();
        double v;
        return t && !t->quoted && toNumber(t->text, v);
    }

    void entityBody(EntityDef& e) {
        for (;;) {
            const Token& t = next();
            if (!t.quoted && t.text == "}") return;
            if (!t.quoted && t.text == "{") {
                brushBody(e);
                continue;
            }
            if (!t.quoted) fail(t.line, "expected a key, a brush or '}', found '" + t.text + "'");
            const std::string key = t.text;
            const Token& v = next();
            if (!v.quoted) fail(v.line, "the value of key '" + key + "' must be in quotes");
            e.keys.emplace_back(key, v.text);
        }
    }

    void brushBody(EntityDef& e) {
        Item item;
        item.line = lastLine_;
        const Token* t = peek();
        if (!t) fail(lastLine_, "unexpected end of file");
        if (!t->quoted && t->text == "patchDef2") { next(); patch(item, false); }
        else if (!t->quoted && t->text == "patchDef3") { next(); patch(item, true); }
        else if (!t->quoted && t->text == "brushDef") {
            next();
            expect("{");
            faces(item);  // consumes the inner '}'
            expect("}");
        } else if (!t->quoted && (t->text == "brushDef3" || t->text == "patchDef5" || t->text == "patchDef4" || t->text == "terrainDef"))
            fail(t->line, "'" + t->text + "' is not a Quake 3 brush syntax this importer reads");
        else faces(item);  // consumes the '}' closing the brush
        e.items.push_back(std::move(item));
    }

    // Face lines up to and including the closing '}'.
    void faces(Item& item) {
        for (;;) {
            if (peekIs("}")) { next(); return; }
            item.faces.push_back(face());
        }
    }

    FaceDef face() {
        FaceDef f;
        f.line = peek() ? peek()->line : lastLine_;
        for (int i = 0; i < 3; ++i) f.p[i] = point();
        if (peekIs("(")) {  // brush primitive: ( ( a b c ) ( d e f ) ) texture [contents surface value]
            f.kind = TexKind::Primitive;
            expect("(");
            for (int r = 0; r < 2; ++r) {
                expect("(");
                for (int c = 0; c < 3; ++c) f.matrix[r][c] = number();
                expect(")");
            }
            expect(")");
            f.texture = next().text;
            std::vector<double> nums = numbers();
            if (nums.size() >= 3) { f.contents = std::llround(nums[0]); f.surface = std::llround(nums[1]); f.value = std::llround(nums[2]); }
            return f;
        }
        const Token& tex = next();
        if (tex.text == "(" || tex.text == ")" || tex.text == "{" || tex.text == "}") fail(tex.line, "expected a texture name, found '" + tex.text + "'");
        f.texture = tex.text;
        if (peekIs("[")) {  // Valve 220
            f.kind = TexKind::Valve;
            for (int a = 0; a < 2; ++a) {
                expect("[");
                f.axis[a].x = number(); f.axis[a].y = number(); f.axis[a].z = number();
                f.axisOffset[a] = number();
                expect("]");
            }
            const std::vector<double> nums = numbers();
            if (nums.size() >= 3) { f.rotate = nums[0]; f.scale[0] = nums[1]; f.scale[1] = nums[2]; }
            if (nums.size() >= 6) { f.contents = std::llround(nums[3]); f.surface = std::llround(nums[4]); f.value = std::llround(nums[5]); }
            return f;
        }
        const std::vector<double> nums = numbers();  // xoff yoff rot xscale yscale [contents surface value]
        if (nums.size() < 5) fail(tex.line, "a face needs 'xoff yoff rotation xscale yscale' after the texture name");
        f.shift[0] = nums[0]; f.shift[1] = nums[1]; f.rotate = nums[2]; f.scale[0] = nums[3]; f.scale[1] = nums[4];
        if (nums.size() >= 8) { f.contents = std::llround(nums[5]); f.surface = std::llround(nums[6]); f.value = std::llround(nums[7]); }
        return f;
    }

    std::vector<double> numbers() {
        std::vector<double> v;
        while (peekNumber()) v.push_back(number());
        return v;
    }

    void patch(Item& item, bool v3) {
        item.patch = true;
        PatchDef& p = item.patchDef;
        p.line = lastLine_;
        p.explicitSubdivisions = v3;
        expect("{");
        p.texture = next().text;
        expect("(");
        const std::vector<double> head = numbers();
        expect(")");
        if (head.size() < 2) fail(lastLine_, "a patch header is '( width height ... )'");
        p.width = static_cast<int>(head[0]);
        p.height = static_cast<int>(head[1]);
        expect("(");
        while (!peekIs(")")) {
            expect("(");
            std::vector<std::array<double, 5>> column;
            while (!peekIs(")")) {
                expect("(");
                std::array<double, 5> pt{};
                for (double& c : pt) c = number();
                expect(")");
                column.push_back(pt);
            }
            expect(")");
            p.grid.push_back(std::move(column));
        }
        expect(")");
        expect("}");
        expect("}");
    }
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Small math
// ---------------------------------------------------------------------------------------------------------------------------------------------

double comp(const Vec3& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }
double& compRef(Vec3& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }

// Quake (x east, y north, z up, units) -> editor (x east, y up, z south, metres): editor = (qx, qz, -qy) / 32 (DESIGN §4). A pure rotation.
struct Frame {
    double unitsPerMeter;
    Vec3 pos(const Vec3& q) const { return {q.x / unitsPerMeter, q.z / unitsPerMeter, -q.y / unitsPerMeter}; }
    static Vec3 dir(const Vec3& q) { return {q.x, q.z, -q.y}; }
};

using Mat3 = std::array<std::array<double, 3>, 3>;
Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r[i][j] += a[i][k] * b[k][j];
    return r;
}
Mat3 rotX(double a) { const double c = std::cos(a), s = std::sin(a); return {{{1, 0, 0}, {0, c, -s}, {0, s, c}}}; }
Mat3 rotY(double a) { const double c = std::cos(a), s = std::sin(a); return {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}}; }
Mat3 rotZ(double a) { const double c = std::cos(a), s = std::sin(a); return {{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}}; }

// Quake's `angles` "pitch yaw roll" (AnglesToAxis: R = Rz(yaw) Ry(pitch) Rx(roll), pitch down is positive) as the editor's `angles`
// (Ry(yaw) Rx(pitch) Rz(roll), the same turn seen in the editor's axes).
Vec3 quakeAnglesToEditor(double pitch, double yaw, double roll) {
    const Mat3 q = mul(rotZ(yaw * kDeg), mul(rotY(pitch * kDeg), rotX(roll * kDeg)));
    const Mat3 c = {{{1, 0, 0}, {0, 0, 1}, {0, -1, 0}}};
    const Mat3 ct = {{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}};
    const Mat3 m = mul(c, mul(q, ct));
    double P, Y, R;
    const double sp = std::clamp(-m[1][2], -1.0, 1.0);
    P = std::asin(sp);
    if (std::fabs(sp) < 0.999999) { Y = std::atan2(m[0][2], m[2][2]); R = std::atan2(m[1][0], m[1][1]); }
    else { Y = std::atan2(-m[2][0], m[0][0]); R = 0; }
    auto clean = [](double deg) { deg = std::round(deg * 1e6) / 1e6; return deg == 0 ? 0.0 : deg; };
    return {clean(P / kDeg), clean(Y / kDeg), clean(R / kDeg)};
}

// Euler angles whose forward axis (R * (0,0,1), lights.h) is the unit vector d.
Vec3 anglesLookingAlong(const Vec3& d) {
    const double P = std::asin(std::clamp(-d.y, -1.0, 1.0));
    const double Y = std::fabs(d.y) > 0.999999 ? 0.0 : std::atan2(d.x, d.z);
    auto clean = [](double deg) { deg = std::round(deg * 1e6) / 1e6; return deg == 0 ? 0.0 : deg; };
    return {clean(P / kDeg), clean(Y / kDeg), 0.0};
}

std::string fmt(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

json vecJson(const Vec3& v) { return json::array({v.x, v.y, v.z}); }

std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Texture coordinates: pixels / world units of the file -> repeats (the editor's uvs), as an affine map of the point in Quake units
// ---------------------------------------------------------------------------------------------------------------------------------------------

struct UvAffine {
    Vec3 gu, gv;       // gradient, repeats per Quake unit
    double cu = 0, cv = 0;
    Vec2 at(const Vec3& p) const { return {gu.dot(p) + cu, gv.dot(p) + cv}; }
};

// q3map2 TextureAxisFromPlane: normal, s axis, t axis of the six axial directions (the earlier one wins a tie).
const Vec3 kBaseAxes[6][3] = {
    {{0, 0, 1}, {1, 0, 0}, {0, -1, 0}},   {{0, 0, -1}, {1, 0, 0}, {0, -1, 0}}, {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}},
    {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}},
};

UvAffine uvOf(const FaceDef& f, const Vec3& n, double texW, double texH) {
    UvAffine a;
    switch (f.kind) {
        case TexKind::Classic: {  // q3map2 QuakeTextureVecs
            int best = 0;
            double bestDot = 0;
            for (int i = 0; i < 6; ++i) {
                const double d = n.dot(kBaseAxes[i][0]);
                if (d > bestDot + 1e-4) { bestDot = d; best = i; }
            }
            Vec3 vecs[2] = {kBaseAxes[best][1], kBaseAxes[best][2]};
            double sinv, cosv;
            const double rot = std::fmod(f.rotate, 360.0);
            if (rot == 0) { sinv = 0; cosv = 1; }
            else if (rot == 90 || rot == -270) { sinv = 1; cosv = 0; }
            else if (rot == 180 || rot == -180) { sinv = 0; cosv = -1; }
            else if (rot == 270 || rot == -90) { sinv = -1; cosv = 0; }
            else { sinv = std::sin(rot * kDeg); cosv = std::cos(rot * kDeg); }
            const int sv = vecs[0].x != 0 ? 0 : vecs[0].y != 0 ? 1 : 2;
            const int tv = vecs[1].x != 0 ? 0 : vecs[1].y != 0 ? 1 : 2;
            for (int i = 0; i < 2; ++i) {
                const double ns = cosv * comp(vecs[i], sv) - sinv * comp(vecs[i], tv);
                const double nt = sinv * comp(vecs[i], sv) + cosv * comp(vecs[i], tv);
                compRef(vecs[i], sv) = ns;
                compRef(vecs[i], tv) = nt;
            }
            const double sx = f.scale[0] != 0 ? f.scale[0] : 1, sy = f.scale[1] != 0 ? f.scale[1] : 1;
            a.gu = vecs[0] * (1.0 / (sx * texW));
            a.gv = vecs[1] * (1.0 / (sy * texH));
            a.cu = f.shift[0] / texW;
            a.cv = f.shift[1] / texH;
            break;
        }
        case TexKind::Valve: {
            const double sx = f.scale[0] != 0 ? f.scale[0] : 1, sy = f.scale[1] != 0 ? f.scale[1] : 1;
            a.gu = f.axis[0] * (1.0 / (sx * texW));
            a.gv = f.axis[1] * (1.0 / (sy * texH));
            a.cu = f.axisOffset[0] / texW;
            a.cv = f.axisOffset[1] / texH;
            break;
        }
        case TexKind::Primitive: {  // Radiant ComputeAxisBase: the matrix works on (p . texS, p . texT), already in repeats
            Vec3 nn = n;
            if (std::fabs(nn.x) < 1e-6) nn.x = 0;
            if (std::fabs(nn.y) < 1e-6) nn.y = 0;
            if (std::fabs(nn.z) < 1e-6) nn.z = 0;
            const double rotY2 = -std::atan2(nn.z, std::sqrt(nn.y * nn.y + nn.x * nn.x));
            const double rotZ2 = std::atan2(nn.y, nn.x);
            const Vec3 texS{-std::sin(rotZ2), std::cos(rotZ2), 0};
            const Vec3 texT{-std::sin(rotY2) * std::cos(rotZ2), -std::sin(rotY2) * std::sin(rotZ2), -std::cos(rotY2)};
            a.gu = texS * f.matrix[0][0] + texT * f.matrix[0][1];
            a.gv = texS * f.matrix[1][0] + texT * f.matrix[1][1];
            a.cu = f.matrix[0][2];
            a.cv = f.matrix[1][2];
            break;
        }
    }
    return a;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Brushes: planes -> a closed convex mesh
// ---------------------------------------------------------------------------------------------------------------------------------------------

struct Plane {
    Vec3 n;
    double d = 0;
};

struct Geometry {
    std::vector<Vec3> verts;  // Quake units
    struct Face {
        int plane = 0;
        std::vector<int> loop;  // counter-clockwise seen from outside
    };
    std::vector<Face> faces;
};

bool buildGeometry(const std::vector<Plane>& planes, double tol, Geometry& g, std::string& why) {
    g = {};
    const int n = static_cast<int>(planes.size());
    if (n < 4) { why = "fewer than 4 distinct planes"; return false; }
    std::vector<Vec3> cand;
    for (int i = 0; i < n; ++i) for (int j = i + 1; j < n; ++j) {
        const Vec3 c23 = planes[i].n.cross(planes[j].n);
        if (c23.length() < 1e-9) continue;
        for (int k = j + 1; k < n; ++k) {
            const Vec3 jk = planes[j].n.cross(planes[k].n);
            const double det = planes[i].n.dot(jk);
            if (std::fabs(det) < 1e-9) continue;
            const Vec3 x = (jk * planes[i].d + planes[k].n.cross(planes[i].n) * planes[j].d + c23 * planes[k].d) * (1.0 / det);
            bool inside = true;
            for (int m = 0; m < n && inside; ++m) inside = planes[m].n.dot(x) - planes[m].d <= tol;
            if (inside) cand.push_back(x);
        }
    }
    for (const Vec3& c : cand) {
        bool known = false;
        for (const Vec3& v : g.verts) if ((v - c).length() <= tol) { known = true; break; }
        if (!known) g.verts.push_back(c);
    }
    if (g.verts.size() < 4) { why = "the planes enclose no volume"; return false; }
    for (int p = 0; p < n; ++p) {
        std::vector<int> on;
        for (size_t v = 0; v < g.verts.size(); ++v)
            if (std::fabs(planes[p].n.dot(g.verts[v]) - planes[p].d) <= tol) on.push_back(static_cast<int>(v));
        if (on.size() < 3) continue;
        Vec3 c;
        for (int v : on) c = c + g.verts[v];
        c = c * (1.0 / on.size());
        const Vec3& nn = planes[p].n;
        const Vec3 u = (std::fabs(nn.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0}).cross(nn).normalized();
        const Vec3 w = nn.cross(u);
        std::sort(on.begin(), on.end(), [&](int a, int b) {
            const Vec3 ra = g.verts[a] - c, rb = g.verts[b] - c;
            return std::atan2(ra.dot(w), ra.dot(u)) < std::atan2(rb.dot(w), rb.dot(u));
        });
        Vec3 area;
        for (size_t i = 0; i < on.size(); ++i) {
            const Vec3& a = g.verts[on[i]];
            const Vec3& b = g.verts[on[(i + 1) % on.size()]];
            area = area + a.cross(b);
        }
        if (area.dot(nn) * 0.5 < 1e-4) continue;  // the plane only touches the brush at an edge or a corner
        g.faces.push_back({p, std::move(on)});
    }
    if (g.faces.size() < 4) { why = "fewer than 4 faces"; return false; }
    return true;
}

struct BuiltBrush {
    Brush brush;
    std::vector<int> facePlane;  // per face of `brush`: index of the FaceDef it came from
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The importer
// ---------------------------------------------------------------------------------------------------------------------------------------------

class Importer {
public:
    Importer(const MapImportOptions& o) : opt_(o), frame_{o.unitsPerMeter} {}

    MapImportResult run(const std::string& text);

private:
    MapImportOptions opt_;
    Frame frame_;
    MapImportResult res_;
    std::set<std::string> warnedTextures_;
    std::map<std::string, std::pair<int, int>> sizeCache_;
    std::unordered_map<std::string, std::vector<Vec3>> originsByName_;  // targetname -> origins (Quake units)

    json entities_ = json::array();
    json detailBrushes_ = json::array();   // CONTENTS_DETAIL brushes of the world and of func_group
    json patchBrushes_ = json::array();    // patches of the world and of func_group, in func_detail
    double worldLightmapScale_ = 1;

    void warn(const std::string& w) { res_.warnings.push_back(w); }
    std::pair<int, int> textureSize(const std::string& tex);
    static std::string textureName(std::string t);
    json convertBrush(const EntityDef& e, const Item& item, int brushIndex, const std::string& classname, bool& detail, bool& isOrigin, Vec3* originOut);
    json convertPatch(const EntityDef& e, const Item& item, int patchIndex, const std::string& classname, double lightmapScale);
    json convertEntity(const EntityDef& e);
    void convertLight(const EntityDef& e, json& out, std::map<std::string, std::string>& kv, Vec3& origin, bool& handled);
    void resolveReferences();
    double lightmapScaleOf(const EntityDef& e, bool structural) const;
};

std::string Importer::textureName(std::string t) {
    std::replace(t.begin(), t.end(), '\\', '/');
    if (lower(t).rfind("textures/", 0) == 0) t = t.substr(9);
    return t;
}

// textures that are shaders without an image of their own: common/caulk, common/clip, ..., skyboxes, editor placeholders
bool shaderOnly(const std::string& tex) {
    const std::string t = lower(tex);
    return t.rfind("common/", 0) == 0 || t == "null" || t == "__tb_empty" || (t.size() >= 4 && t.compare(t.size() - 4, 4, "cube") == 0);
}

std::pair<int, int> Importer::textureSize(const std::string& tex) {
    const auto it = sizeCache_.find(tex);
    if (it != sizeCache_.end()) return it->second;
    int w = 0, h = 0;
    std::pair<int, int> r{opt_.fallbackTextureSize, opt_.fallbackTextureSize};
    if (opt_.textureSize && opt_.textureSize(tex, w, h) && w > 0 && h > 0) r = {w, h};
    else if (shaderOnly(tex)) {}
    else if (warnedTextures_.insert(tex).second)
        warn("texture '" + tex + "' not found or unreadable: its texture coordinates assume " + std::to_string(opt_.fallbackTextureSize) + "x" +
             std::to_string(opt_.fallbackTextureSize) + " pixels");
    sizeCache_[tex] = r;
    return r;
}

double Importer::lightmapScaleOf(const EntityDef& e, bool structural) const {
    double v;
    if (const std::string* s = e.find("_lightmapscale"); s && toNumber(*s, v) && v > 0) return 1.0 / v;
    if (structural) return worldLightmapScale_;
    return 1.0;
}

json Importer::convertBrush(const EntityDef& e, const Item& item, int brushIndex, const std::string& classname, bool& detail, bool& isOrigin, Vec3* originOut) {
    detail = false;
    isOrigin = false;
    const std::string where = "entity " + std::to_string(e.index) + " (" + classname + ") brush " + std::to_string(brushIndex) + " (line " + std::to_string(item.line) + ")";
    std::vector<Plane> planes;
    std::vector<int> planeFace;  // plane -> face index in the file
    for (size_t i = 0; i < item.faces.size(); ++i) {
        const FaceDef& f = item.faces[i];
        const Vec3 n = (f.p[0] - f.p[1]).cross(f.p[2] - f.p[1]).normalized();
        if (n.length() < 0.5) { warn(where + ": face " + std::to_string(i) + " has three points on a line; the face is ignored"); continue; }
        Plane pl{n, n.dot(f.p[0])};
        bool dup = false;
        for (const Plane& o : planes)
            if (o.n.dot(pl.n) > 1.0 - 1e-9 && std::fabs(o.d - pl.d) < 1e-4) { dup = true; break; }
        if (dup) continue;
        planes.push_back(pl);
        planeFace.push_back(static_cast<int>(i));
    }
    std::string why;
    Geometry g;
    Brush brush;
    bool ok = false;
    for (const double tol : {0.002, 0.0005, 0.01, 0.03}) {
        if (!buildGeometry(planes, tol, g, why)) continue;
        // used vertices only, in order of first use
        std::vector<int> remap(g.verts.size(), -1);
        brush = Brush{};
        brush.id = "tmp";
        for (const auto& gf : g.faces) for (int v : gf.loop) if (remap[v] < 0) { remap[v] = static_cast<int>(brush.vertices.size()); brush.vertices.push_back(frame_.pos(g.verts[v])); }
        uint32_t id = 0;
        for (const auto& gf : g.faces) {
            BrushFace bf;
            bf.id = id++;
            for (int v : gf.loop) bf.loop.push_back(static_cast<uint32_t>(remap[v]));
            brush.faces.push_back(std::move(bf));
        }
        // the corners of a unit-conversion that are numerically equal must not repeat inside one face
        const auto problem = brush.checkConvexSolid();  // planes always make a convex solid, whatever the entity's class asks for
        if (!problem) { ok = true; break; }
        why = *problem;
    }
    if (!ok) {
        ++res_.stats.skipped;
        warn(where + ": skipped, " + why);
        return nullptr;
    }

    // contents / surface flags, special brushes
    bool allOrigin = !item.faces.empty();
    for (const FaceDef& f : item.faces) {
        const std::string t = lower(textureName(f.texture));
        allOrigin = allOrigin && (t == "common/origin" || t == "origin");
        if (f.contents & 0x8000000LL) detail = true;
    }
    if (allOrigin) {  // an origin brush marks the pivot of its entity and is not geometry
        isOrigin = true;
        if (originOut) {
            Vec3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
            for (const Vec3& v : g.verts) for (int a = 0; a < 3; ++a) { compRef(lo, a) = std::min(comp(lo, a), comp(v, a)); compRef(hi, a) = std::max(comp(hi, a), comp(v, a)); }
            *originOut = (lo + hi) * 0.5;
        }
        return nullptr;
    }

    json fj = json::array();
    const double lmScale = lightmapScaleOf(e, EntityClassRegistry::isStructural(classname));
    for (size_t fi = 0; fi < brush.faces.size(); ++fi) {
        const BrushFace& bf = brush.faces[fi];
        const int planeIdx = g.faces[fi].plane;
        const FaceDef& def = item.faces[static_cast<size_t>(planeFace[static_cast<size_t>(planeIdx)])];
        const Plane& pl = planes[static_cast<size_t>(planeIdx)];
        const std::string texture = textureName(def.texture);
        double tw = 1, th = 1;
        if (def.kind != TexKind::Primitive) { const auto sz = textureSize(texture); tw = sz.first; th = sz.second; }
        const UvAffine uv = uvOf(def, pl.n, tw, th);
        json face;
        face["id"] = bf.id;
        face["loop"] = bf.loop;
        face["texture"] = texture;
        json uvs = json::array();
        for (int v : g.faces[fi].loop) { const Vec2 t = uv.at(g.verts[v]); uvs.push_back(json::array({t.u, t.v})); }
        face["uvs"] = uvs;

        // the same map as an editable projection ("free": two in-plane axes, metres per repeat, shift in repeats), when the two directions are usable
        const Vec3 nE = Frame::dir(pl.n);
        auto inPlane = [&](const Vec3& gq) {  // gradient in repeats per metre, in the editor's axes, projected into the face plane
            Vec3 ge = Frame::dir(gq) * opt_.unitsPerMeter;
            return ge - nE * ge.dot(nE);
        };
        const Vec3 gu = inPlane(uv.gu), gv = inPlane(uv.gv);
        if (gu.length() > 1e-9 && gv.length() > 1e-9) {
            const Vec3 bu = gu.normalized(), bv = gv.normalized();
            if (std::fabs(bu.dot(bv)) < 0.999) {
                const Vec3 q0 = brush.vertices[bf.loop[0]];
                const Vec2 t0 = uv.at(g.verts[g.faces[fi].loop[0]]);
                json proj;
                proj["kind"] = "free";
                proj["basis"] = json::array({vecJson(bu), vecJson(bv)});
                proj["scale"] = json::array({1.0 / gu.length(), 1.0 / gv.length()});
                proj["shift"] = json::array({t0.u - gu.dot(q0), t0.v - gv.dot(q0)});
                proj["rotate"] = 0;
                face["projection"] = proj;
            }
        }
        if (lmScale != 1.0) face["lightmapScale"] = std::clamp(lmScale, kMinLightmapScale, kMaxLightmapScale);
        if (def.contents || def.surface || def.value) face["q3"] = json{{"contents", def.contents}, {"surface", def.surface}, {"value", def.value}};
        fj.push_back(std::move(face));
        ++res_.stats.faces;
    }
    json b;
    b["op"] = "add";
    b["vertices"] = json::array();
    for (const Vec3& v : brush.vertices) b["vertices"].push_back(vecJson(v));
    b["faces"] = std::move(fj);
    return b;
}

// ---- patches -------------------------------------------------------------------------------------------------------------------------------

struct PatchVertex {
    Vec3 p;
    double s = 0, t = 0;
};

json Importer::convertPatch(const EntityDef& e, const Item& item, int patchIndex, const std::string& classname, double lightmapScale) {
    const PatchDef& pd = item.patchDef;
    const std::string where = "entity " + std::to_string(e.index) + " (" + classname + ") patch " + std::to_string(patchIndex) + " (line " + std::to_string(pd.line) + ")";
    const int W = static_cast<int>(pd.grid.size());
    const int H = W ? static_cast<int>(pd.grid[0].size()) : 0;
    for (const auto& col : pd.grid) if (static_cast<int>(col.size()) != H) { ++res_.stats.skipped; warn(where + ": skipped, the rows of the control grid differ in length"); return nullptr; }
    if (W < 3 || H < 3 || W % 2 == 0 || H % 2 == 0) {
        ++res_.stats.skipped;
        warn(where + ": skipped, a quadratic patch has an odd number (3, 5, ...) of control points in both directions, this one has " + std::to_string(W) + " x " + std::to_string(H));
        return nullptr;
    }
    if (pd.width != W || pd.height != H) warn(where + ": the header says " + std::to_string(pd.width) + " x " + std::to_string(pd.height) + " but the grid is " + std::to_string(W) + " x " + std::to_string(H) + "; the grid is used");

    auto ctl = [&](int i, int j) { const auto& c = pd.grid[static_cast<size_t>(i)][static_cast<size_t>(j)]; return PatchVertex{{c[0], c[1], c[2]}, c[3], c[4]}; };
    // segments per span: the chord error of a quadratic is |c1 - (c0 + c2) / 2| / (2 N^2)
    auto segments = [&](bool alongI) {
        double dev = 0;
        const int spans = ((alongI ? W : H) - 1) / 2, other = alongI ? H : W;
        for (int s = 0; s < spans; ++s) for (int o = 0; o < other; ++o) {
            const PatchVertex a = alongI ? ctl(2 * s, o) : ctl(o, 2 * s), m = alongI ? ctl(2 * s + 1, o) : ctl(o, 2 * s + 1), b = alongI ? ctl(2 * s + 2, o) : ctl(o, 2 * s + 2);
            dev = std::max(dev, (m.p - (a.p + b.p) * 0.5).length());
        }
        return std::clamp(static_cast<int>(std::ceil(std::sqrt(dev / (2.0 * std::max(opt_.patchTolerance, 1e-3))))), 1, std::max(1, opt_.patchMaxSegments));
    };
    const int nI = segments(true), nJ = segments(false);
    const int spansI = (W - 1) / 2, spansJ = (H - 1) / 2;
    const int GI = spansI * nI + 1, GJ = spansJ * nJ + 1;

    auto basis = [](double t, double* b) { b[0] = (1 - t) * (1 - t); b[1] = 2 * t * (1 - t); b[2] = t * t; };
    std::vector<PatchVertex> grid(static_cast<size_t>(GI) * GJ);
    for (int gi = 0; gi < GI; ++gi) for (int gj = 0; gj < GJ; ++gj) {
        const int si = std::min(gi / nI, spansI - 1), sj = std::min(gj / nJ, spansJ - 1);
        const double u = static_cast<double>(gi - si * nI) / nI, v = static_cast<double>(gj - sj * nJ) / nJ;
        double bu[3], bv[3];
        basis(u, bu);
        basis(v, bv);
        PatchVertex r;
        for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) {
            const PatchVertex c = ctl(2 * si + a, 2 * sj + b);
            const double w = bu[a] * bv[b];
            r.p = r.p + c.p * w;
            r.s += c.s * w;
            r.t += c.t * w;
        }
        grid[static_cast<size_t>(gi) * GJ + gj] = r;
    }

    // weld coincident grid points (collapsed edges of cones, spheres, ...)
    const double weldTol = 1e-3;
    std::vector<int> weld(grid.size());
    std::vector<Vec3> pts;
    for (size_t i = 0; i < grid.size(); ++i) {
        int hit = -1;
        for (size_t k = pts.size(); k-- > 0 && hit < 0;) if ((pts[k] - grid[i].p).length() <= weldTol) hit = static_cast<int>(k);
        if (hit < 0) { hit = static_cast<int>(pts.size()); pts.push_back(grid[i].p); }
        weld[i] = hit;
    }
    struct Tri { int g[3]; };
    std::vector<Tri> top;
    auto at = [&](int gi, int gj) { return gi * GJ + gj; };
    for (int gi = 0; gi + 1 < GI; ++gi) for (int gj = 0; gj + 1 < GJ; ++gj) {
        const int a = at(gi, gj), b = at(gi + 1, gj), c = at(gi + 1, gj + 1), d = at(gi, gj + 1);
        for (const Tri& t : {Tri{{a, b, c}}, Tri{{a, c, d}}}) {
            const Vec3& p0 = pts[static_cast<size_t>(weld[static_cast<size_t>(t.g[0])])], &p1 = pts[static_cast<size_t>(weld[static_cast<size_t>(t.g[1])])], &p2 = pts[static_cast<size_t>(weld[static_cast<size_t>(t.g[2])])];
            if ((p1 - p0).cross(p2 - p0).length() * 0.5 < 1e-4) continue;  // collapsed
            top.push_back(t);
        }
    }
    if (top.empty()) { ++res_.stats.skipped; warn(where + ": skipped, the patch has no area"); return nullptr; }

    // vertex normals (area weighted) and the slab behind the front side
    std::vector<Vec3> normal(pts.size());
    for (const Tri& t : top) {
        const int a = weld[static_cast<size_t>(t.g[0])], b = weld[static_cast<size_t>(t.g[1])], c = weld[static_cast<size_t>(t.g[2])];
        const Vec3 n = (pts[static_cast<size_t>(b)] - pts[static_cast<size_t>(a)]).cross(pts[static_cast<size_t>(c)] - pts[static_cast<size_t>(a)]);
        normal[static_cast<size_t>(a)] = normal[static_cast<size_t>(a)] + n;
        normal[static_cast<size_t>(b)] = normal[static_cast<size_t>(b)] + n;
        normal[static_cast<size_t>(c)] = normal[static_cast<size_t>(c)] + n;
    }
    const size_t np = pts.size();
    std::vector<Vec3> all = pts;  // [0, np) top, [np, 2np) bottom
    for (size_t i = 0; i < np; ++i) all.push_back(pts[i] - normal[i].normalized() * opt_.patchThickness);

    struct OutFace { int v[3]; Vec2 uv[3]; bool isTop; };
    std::vector<OutFace> faces;
    auto uvOfGrid = [&](int g) { return Vec2{grid[static_cast<size_t>(g)].s, grid[static_cast<size_t>(g)].t}; };
    std::map<std::pair<int, int>, int> edgeCount;
    for (const Tri& t : top) {
        const int a = weld[static_cast<size_t>(t.g[0])], b = weld[static_cast<size_t>(t.g[1])], c = weld[static_cast<size_t>(t.g[2])];
        faces.push_back({{a, b, c}, {uvOfGrid(t.g[0]), uvOfGrid(t.g[1]), uvOfGrid(t.g[2])}, true});
        ++edgeCount[{a, b}]; ++edgeCount[{b, c}]; ++edgeCount[{c, a}];
    }
    const int np_i = static_cast<int>(np);
    for (const Tri& t : top) {
        const int a = weld[static_cast<size_t>(t.g[0])] + np_i, b = weld[static_cast<size_t>(t.g[1])] + np_i, c = weld[static_cast<size_t>(t.g[2])] + np_i;
        faces.push_back({{a, c, b}, {uvOfGrid(t.g[0]), uvOfGrid(t.g[2]), uvOfGrid(t.g[1])}, false});
    }
    // walls along the open edges of the top surface; an edge whose reverse is also there is interior
    std::map<int, Vec2> cornerUv;
    for (const Tri& t : top) for (int k = 0; k < 3; ++k) cornerUv[weld[static_cast<size_t>(t.g[k])]] = uvOfGrid(t.g[k]);
    for (const auto& [edge, count] : edgeCount) {
        if (count != 1 || edgeCount.count({edge.second, edge.first})) continue;
        const int p = edge.first, q = edge.second;  // top has p -> q; the wall runs q -> p
        const int pb = p + np_i, qb = q + np_i;
        faces.push_back({{q, p, pb}, {cornerUv[q], cornerUv[p], cornerUv[p]}, false});
        faces.push_back({{q, pb, qb}, {cornerUv[q], cornerUv[p], cornerUv[q]}, false});
    }

    Brush check;
    check.id = "tmp";
    for (const Vec3& v : all) check.vertices.push_back(frame_.pos(v));
    uint32_t id = 0;
    for (const OutFace& f : faces) { BrushFace bf; bf.id = id++; bf.loop = {static_cast<uint32_t>(f.v[0]), static_cast<uint32_t>(f.v[1]), static_cast<uint32_t>(f.v[2])}; check.faces.push_back(bf); }
    if (const auto err = check.checkStructure()) { ++res_.stats.skipped; warn(where + ": skipped, " + *err); return nullptr; }
    if (check.signedVolume() < 1e-12) { ++res_.stats.skipped; warn(where + ": skipped, the slab behind the patch has no volume"); return nullptr; }

    const std::string texture = textureName(pd.texture);
    json b;
    b["op"] = "add";
    b["vertices"] = json::array();
    for (const Vec3& v : check.vertices) b["vertices"].push_back(vecJson(v));
    json fj = json::array();
    std::map<std::pair<int, int>, uint32_t> topEdge;  // directed edge -> face id of the top triangle that has it
    for (size_t i = 0; i < faces.size(); ++i) {
        const OutFace& f = faces[i];
        json face;
        face["id"] = static_cast<uint32_t>(i);
        face["loop"] = json::array({f.v[0], f.v[1], f.v[2]});
        face["texture"] = f.isTop || opt_.patchBackTexture.empty() ? texture : opt_.patchBackTexture;
        face["uvs"] = json::array({json::array({f.uv[0].u, f.uv[0].v}), json::array({f.uv[1].u, f.uv[1].v}), json::array({f.uv[2].u, f.uv[2].v})});
        if (lightmapScale != 1.0) face["lightmapScale"] = std::clamp(lightmapScale, kMinLightmapScale, kMaxLightmapScale);
        fj.push_back(std::move(face));
        if (f.isTop) { topEdge[{f.v[0], f.v[1]}] = static_cast<uint32_t>(i); topEdge[{f.v[1], f.v[2]}] = static_cast<uint32_t>(i); topEdge[{f.v[2], f.v[0]}] = static_cast<uint32_t>(i); }
    }
    b["faces"] = std::move(fj);
    // the curved surface shades as one: every edge between two top triangles is smooth
    json smooth = json::array();
    for (const auto& [edge, faceA] : topEdge) {
        const auto other = topEdge.find({edge.second, edge.first});
        if (other != topEdge.end() && faceA < other->second) smooth.push_back(json::array({faceA, other->second}));
    }
    if (!smooth.empty()) b["smooth"] = std::move(smooth);
    res_.stats.faces += static_cast<int>(faces.size());
    return b;
}

// ---- lights --------------------------------------------------------------------------------------------------------------------------------
//
// Quake 3 `light` (q3map2): `light` = intensity (default 300), `_color`, `fade` (linear falloff scale), spawnflags 1 = linear falloff,
// `target` + `_sun` 1 = the sun shining at that entity, `target` alone = a spot whose cone reaches the target with `radius`.
// The editor's lights have a radius in metres and a brightness that is the irradiance 1 m from the light (lights.h), so a Q3 light becomes the
// editor light that matches it at HALF its reach: reach = intensity / fade units; the Q3 value there is (intensity - fade * d) / 255 for linear
// lights, so brightness = value * d^2 / window(0.5) (window = the editor's smooth cutoff). A negative `light` keeps its sign (a dark light that takes light
// away, lights.h): the reach and the brightness come from the absolute value.

void Importer::convertLight(const EntityDef& e, json& out, std::map<std::string, std::string>& kv, Vec3& origin, bool& handled) {
    handled = false;
    const std::string cls = e.get("classname");
    auto number = [&](const char* key, double& v) { const std::string* s = e.find(key); return s && toNumber(*s, v); };
    auto color = [&](const char* key, std::string& rgb) {
        const std::string* s = e.find(key);
        if (!s) return false;
        std::istringstream in(*s);
        double r, g, b;
        if (!(in >> r >> g >> b) || !std::isfinite(r + g + b)) return false;
        if (r > 1 || g > 1 || b > 1) { r /= 255; g /= 255; b /= 255; }  // some editors write 0..255
        rgb = fmt(std::max(0.0, r)) + " " + fmt(std::max(0.0, g)) + " " + fmt(std::max(0.0, b));
        return true;
    };
    std::set<std::string> consumed{"classname", "origin", "light", "_color", "color", "fade", "spawnflags", "_sun", "radius", "_deviance", "_samples", "light_color", "intensity"};

    if (cls == "light_point" && (e.find("light_color") || e.find("intensity"))) {  // the game's own point light: renamed keys, same numbers (radius is in metres already)
        std::string rgb;
        if (color("light_color", rgb)) kv["color"] = rgb;
        double v;
        if (number("intensity", v)) kv["brightness"] = fmt(v);  // a negative one is a dark light
        if (number("radius", v)) kv["radius"] = fmt(std::max(0.1, v));
        out["classname"] = "light_point";
        for (const auto& [k, val] : e.keys) if (!consumed.count(k)) kv[k] = val;
        handled = true;
        ++res_.stats.lights;
        return;
    }
    if (cls != "light") return;
    if (!e.items.empty()) { warn("entity " + std::to_string(e.index) + ": a 'light' with brushes is a surface light; left as it is"); return; }

    double intensity = 300;
    number("light", intensity);
    const double sign = intensity < 0 ? -1.0 : 1.0;  // a negative Q3 light takes light away: a dark editor light (negative brightness, lights.h); its reach follows the size
    intensity = std::fabs(intensity);
    double fade = 1;
    number("fade", fade);
    if (fade <= 0) fade = 1;
    double flagsD = 0;
    number("spawnflags", flagsD);
    const long long flags = std::llround(flagsD);
    const bool linear = (flags & 1) != 0;
    std::string rgb = "1 1 1";
    if (!color("_color", rgb)) color("color", rgb);
    kv["color"] = rgb;
    double v;
    if (number("_deviance", v) && v > 0) kv["source_size"] = fmt(v / opt_.unitsPerMeter);
    double sunFlag = 0;
    const bool sun = number("_sun", sunFlag) && sunFlag != 0;
    const std::string target = e.get("target");
    const Vec3* targetOrigin = nullptr;
    if (!target.empty()) { const auto it = originsByName_.find(target); if (it != originsByName_.end() && !it->second.empty()) targetOrigin = &it->second.front(); }

    auto reach = [&]() { return linear ? intensity / fade : std::sqrt(std::max(intensity, 1.0) * 7500.0); };  // units
    auto pointBrightness = [&](double radiusM) {
        const double dRef = radiusM * 0.5;  // metres
        const double q3 = linear ? (intensity - fade * dRef * opt_.unitsPerMeter) / 255.0 : (intensity * 7500.0 / (dRef * opt_.unitsPerMeter * dRef * opt_.unitsPerMeter)) / 255.0;
        const double window = std::pow(1.0 - std::pow(0.5, 4), 2);
        return sign * std::max(0.0, q3) * dRef * dRef / window;
    };

    if (sun) {
        out["classname"] = "light_directional";
        kv["brightness"] = fmt(sign * intensity / 255.0);
        Vec3 dir{0, -1, 0};
        if (targetOrigin) {
            const Vec3 d = Frame::dir(*targetOrigin - origin);
            if (d.length() > 1e-9) dir = d.normalized();
            else warn("entity " + std::to_string(e.index) + ": the sun and its target '" + target + "' are at the same place; it shines straight down");
        } else warn("entity " + std::to_string(e.index) + ": a sun without a target entity; it shines straight down");
        out["angles"] = vecJson(anglesLookingAlong(dir));
    } else if (!target.empty() && targetOrigin) {  // spot: the cone reaches `radius` (units, default 64) at the target
        double coneRadius = 64;
        number("radius", coneRadius);
        const Vec3 dUnits = *targetOrigin - origin;
        const double dist = std::max(dUnits.length(), 1.0);
        const double outer = std::clamp(std::atan2(std::max(coneRadius, 1.0), dist) / kDeg, 0.1, 89.9);
        out["classname"] = "light_spot";
        const double r = std::clamp(reach() / opt_.unitsPerMeter, 0.1, 1000.0);
        kv["radius"] = fmt(r);
        kv["brightness"] = fmt(pointBrightness(r));
        kv["outer_angle"] = fmt(outer);
        kv["inner_angle"] = fmt(outer * 0.8);
        out["angles"] = vecJson(anglesLookingAlong(Frame::dir(dUnits).normalized()));
    } else {
        if (!target.empty()) warn("entity " + std::to_string(e.index) + ": the light's target '" + target + "' does not exist; imported as a point light");
        out["classname"] = "light_point";
        const double r = std::clamp(reach() / opt_.unitsPerMeter, 0.1, 1000.0);
        kv["radius"] = fmt(r);
        kv["brightness"] = fmt(pointBrightness(r));
    }
    for (const auto& [k, val] : e.keys) if (!consumed.count(k)) kv[k] = val;
    handled = true;
    ++res_.stats.lights;
}

// ---- entities ------------------------------------------------------------------------------------------------------------------------------

json Importer::convertEntity(const EntityDef& e) {
    const std::string classname = e.get("classname", e.index == 0 ? "worldspawn" : "info_null");
    json out;
    out["id"] = "e" + std::to_string(e.index);
    out["classname"] = classname;
    std::map<std::string, std::string> kv;

    // origin (Quake units -> metres)
    Vec3 origin;
    bool hasOrigin = false;
    if (const std::string* s = e.find("origin")) {
        std::istringstream in(*s);
        double x, y, z;
        if (in >> x >> y >> z) { origin = Vec3{x, y, z}; hasOrigin = true; }
        else warn("entity " + std::to_string(e.index) + " (" + classname + "): origin '" + *s + "' is not three numbers; ignored");
    }

    bool lightHandled = false;
    json lightOut = json::object();
    convertLight(e, lightOut, kv, origin, lightHandled);
    if (lightHandled) {
        out["classname"] = lightOut["classname"];
        if (lightOut.contains("angles")) out["angles"] = lightOut["angles"];
        else out["angles"] = json::array({0, 0, 0});
        out["origin"] = vecJson(frame_.pos(origin));
        out["keyvalues"] = kv;
        return out;
    }

    // angles ("pitch yaw roll" in Quake's axes) become the entity's own rotation; `angle` (a yaw the game reads itself) stays a key
    Vec3 angles{0, 0, 0};
    if (const std::string* s = e.find("angles")) {
        std::istringstream in(*s);
        double p, y, r;
        if (in >> p >> y >> r) angles = quakeAnglesToEditor(p, y, r);
        else warn("entity " + std::to_string(e.index) + " (" + classname + "): angles '" + *s + "' is not three numbers; ignored");
    }
    for (const auto& [k, v] : e.keys) {
        if (k == "classname" || k == "origin" || k == "angles") continue;
        kv[k] = v;
    }

    // worldspawn: Q3 `_ambient N` (0..255, tinted by `_color`) is the editor's `_ambient "r g b"`
    if (classname == "worldspawn") {
        double amb;
        if (const std::string* s = e.find("_ambient"); s && toNumber(*s, amb)) {
            double r = 1, g = 1, b = 1;
            if (const std::string* c = e.find("_color")) { std::istringstream in(*c); double cr, cg, cb; if (in >> cr >> cg >> cb) { r = cr; g = cg; b = cb; if (r > 1 || g > 1 || b > 1) { r /= 255; g /= 255; b /= 255; } } }
            const double k = std::max(0.0, amb) / 255.0;
            kv["_ambient"] = fmt(r * k) + " " + fmt(g * k) + " " + fmt(b * k);
        }
    }

    // brushes
    const bool structural = EntityClassRegistry::isStructural(classname);
    const bool convex = EntityClassRegistry::requiresConvexBrushes(classname);
    json brushes = json::array();
    Vec3 originBrushCentre;
    bool hasOriginBrush = false;
    int bi = 0, pi = 0;
    for (const Item& item : e.items) {
        if (item.patch) {
            json b = convertPatch(e, item, pi, classname, lightmapScaleOf(e, structural));
            const std::string bid = "e" + std::to_string(e.index) + "_p" + std::to_string(pi++);
            if (!b.is_null()) {
                b["id"] = bid;
                ++res_.stats.patches;
                // a patch does not seal visibility and is not a convex solid: in the world it is func_detail
                (convex ? patchBrushes_ : brushes).push_back(std::move(b));
            }
            continue;
        }
        bool detail = false, isOrigin = false;
        Vec3 oc;
        json b = convertBrush(e, item, bi, classname, detail, isOrigin, &oc);
        const std::string bid = "e" + std::to_string(e.index) + "_b" + std::to_string(bi++);
        if (isOrigin) { originBrushCentre = oc; hasOriginBrush = true; continue; }
        if (b.is_null()) continue;
        b["id"] = bid;
        ++res_.stats.brushes;
        (detail && convex ? detailBrushes_ : brushes).push_back(std::move(b));
    }

    if (hasOriginBrush) { origin = originBrushCentre; hasOrigin = true; }
    out["angles"] = vecJson(angles);
    if (classname == "worldspawn") {
        out["origin"] = json::array({0, 0, 0});
    } else if (!e.items.empty()) {
        if (hasOrigin) out["origin"] = vecJson(frame_.pos(origin));
        else {  // the editor's default pivot of a brush entity: the centre of its geometry
            Vec3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
            bool any = false;
            auto grow = [&](const json& list) {
                for (const json& b : list) for (const json& v : b["vertices"]) for (int a = 0; a < 3; ++a) {
                    const double c = v[static_cast<size_t>(a)].get<double>();
                    compRef(lo, a) = std::min(comp(lo, a), c); compRef(hi, a) = std::max(comp(hi, a), c); any = true;
                }
            };
            grow(brushes);
            out["origin"] = any ? vecJson((lo + hi) * 0.5) : json::array({0, 0, 0});
        }
    } else out["origin"] = vecJson(frame_.pos(origin));
    out["keyvalues"] = kv;
    if (!e.items.empty() || classname == "worldspawn") out["brushes"] = std::move(brushes);
    return out;
}

// target / killtarget: a name in the file -> a reference to the (one) entity that has it; ambiguous or missing names stay as text
void Importer::resolveReferences() {
    std::unordered_map<std::string, std::vector<size_t>> byName;
    for (size_t i = 0; i < entities_.size(); ++i) {
        const json& kv = entities_[i]["keyvalues"];
        if (kv.contains("targetname") && !kv["targetname"].get<std::string>().empty()) byName[kv["targetname"].get<std::string>()].push_back(i);
    }
    std::set<std::string> reported;
    for (json& ent : entities_) {
        json& kv = ent["keyvalues"];
        for (const char* key : {"target", "killtarget"}) {
            if (!kv.contains(key)) continue;
            const std::string name = kv[key].get<std::string>();
            if (name.empty()) { kv.erase(key); continue; }
            const auto it = byName.find(name);
            if (it == byName.end()) {
                if (reported.insert(name).second) warn("target '" + name + "' (" + ent["id"].get<std::string>() + ") matches no targetname in the map; kept as text");
                continue;
            }
            // every entity with that name is a target (Quake triggers them all)
            if (it->second.size() == 1) ent["refs"][key] = entities_[it->second.front()]["id"];
            else {
                json ids = json::array();
                for (const size_t i : it->second) ids.push_back(entities_[i]["id"]);
                ent["refs"][key] = std::move(ids);
            }
            kv.erase(key);
        }
    }
}

MapImportResult Importer::run(const std::string& text) {
    Parser parser(tokenize(text));
    std::vector<EntityDef> defs = parser.parse();
    if (defs.empty() || defs.front().get("classname") != "worldspawn") {
        EntityDef w;
        w.keys.emplace_back("classname", "worldspawn");
        for (EntityDef& d : defs) ++d.index;
        defs.insert(defs.begin(), std::move(w));
        defs.front().index = 0;
        warn("the first entity is not worldspawn; an empty worldspawn was added");
    }
    // names -> origins (sun and spot targets)
    for (const EntityDef& d : defs) {
        const std::string name = d.get("targetname");
        if (name.empty()) continue;
        Vec3 o;
        if (const std::string* s = d.find("origin")) { std::istringstream in(*s); in >> o.x >> o.y >> o.z; }
        originsByName_[name].push_back(o);
    }
    if (const std::string* s = defs.front().find("_lightmapscale")) { double v; if (toNumber(*s, v) && v > 0) worldLightmapScale_ = 1.0 / v; }

    for (const EntityDef& d : defs) {
        const std::string cls = d.get("classname");
        const std::string tbType = d.get("_tb_type");
        if (cls == "func_group" && (tbType == "_tb_layer" || tbType == "_tb_group") && d.items.empty()) { ++res_.stats.droppedEntities; continue; }
        json ent = convertEntity(d);
        // a func_group / func_detail whose brushes all went elsewhere (detail, patches) or failed is an empty shell
        if (cls != "worldspawn" && !d.items.empty() && ent.contains("brushes") && ent["brushes"].empty() && EntityClassRegistry::isStructural(cls)) { ++res_.stats.droppedEntities; continue; }
        entities_.push_back(std::move(ent));
    }
    resolveReferences();

    auto centre = [](const json& list) {
        Vec3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
        for (const json& b : list) for (const json& v : b["vertices"]) for (int a = 0; a < 3; ++a) {
            const double c = v[static_cast<size_t>(a)].get<double>();
            compRef(lo, a) = std::min(comp(lo, a), c); compRef(hi, a) = std::max(comp(hi, a), c);
        }
        return vecJson((lo + hi) * 0.5);
    };
    if (!detailBrushes_.empty())
        entities_.push_back(json{{"id", "e_detail"}, {"classname", "func_detail"}, {"angles", json::array({0, 0, 0})}, {"origin", centre(detailBrushes_)}, {"keyvalues", json::object()}, {"brushes", detailBrushes_}});
    if (!patchBrushes_.empty())
        entities_.push_back(json{{"id", "e_patches"}, {"classname", "func_detail"}, {"angles", json::array({0, 0, 0})}, {"origin", centre(patchBrushes_)}, {"keyvalues", json::object()}, {"brushes", patchBrushes_}});

    res_.stats.entities = static_cast<int>(entities_.size());
    res_.doc = json{{"schemaVersion", kSchemaVersion}, {"entities", entities_}, {"prefabInstances", json::array()}};
    // what the importer returns must open: the Core rejects a document with one bad brush as a whole
    if (const auto problem = validateDocument(documentFromJson(res_.doc))) throw std::runtime_error("internal error: the imported document is invalid: " + *problem);
    return std::move(res_);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------------------------------------------------------------------------

MapImportResult importQuakeMap(const std::string& text, const MapImportOptions& options) {
    Importer imp(options);
    return imp.run(text);
}

MapImportResult importQuakeMapFile(const std::filesystem::path& mapFile, const MapImportOptions& options) {
    std::ifstream in(mapFile, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + mapFile.string());
    std::ostringstream s;
    s << in.rdbuf();
    return importQuakeMap(s.str(), options);
}

bool imageSize(const std::vector<unsigned char>& b, const std::string& extension, int& w, int& h) {
    const std::string ext = lower(extension);
    auto be32 = [&](size_t o) { return (static_cast<int>(b[o]) << 24) | (static_cast<int>(b[o + 1]) << 16) | (static_cast<int>(b[o + 2]) << 8) | b[o + 3]; };
    if (ext == ".png") {
        if (b.size() < 24 || b[1] != 'P' || b[2] != 'N' || b[3] != 'G') return false;
        w = be32(16);
        h = be32(20);
        return w > 0 && h > 0;
    }
    if (ext == ".tga") {
        if (b.size() < 18) return false;
        w = b[12] | (b[13] << 8);
        h = b[14] | (b[15] << 8);
        return w > 0 && h > 0;
    }
    if (ext == ".jpg" || ext == ".jpeg") {
        size_t i = 2;
        if (b.size() < 4 || b[0] != 0xFF || b[1] != 0xD8) return false;
        while (i + 9 < b.size()) {
            if (b[i] != 0xFF) { ++i; continue; }
            const unsigned char m = b[i + 1];
            if (m == 0xFF) { ++i; continue; }
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
            const size_t len = (static_cast<size_t>(b[i + 2]) << 8) | b[i + 3];
            if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                h = (b[i + 5] << 8) | b[i + 6];
                w = (b[i + 7] << 8) | b[i + 8];
                return w > 0 && h > 0;
            }
            i += 2 + len;
        }
        return false;
    }
    return false;
}

MapImportOptions importOptionsFor(const std::filesystem::path& assetsRoot, const GameProfile& profile) {
    MapImportOptions o;
    o.textureSize = [assetsRoot, profile](const std::string& tex, int& w, int& h) {
        const auto file = readTexture(assetsRoot, profile, tex);
        if (!file) return false;
        if (imageSize(file->bytes, file->ext, w, h)) return true;
        if (const auto img = decodeImage(file->bytes, file->ext)) { w = img->width; h = img->height; return w > 0 && h > 0; }
        return false;
    };
    return o;
}

}  // namespace core
