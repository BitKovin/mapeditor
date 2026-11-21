// tools/relief/relief_edit -- see relief_edit.h.
#include "tools/relief/relief_edit.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace native::tools::relief {

namespace tp = core::texplan;
namespace mesh = native::tools::mesh;
using core::Vec3;

namespace {

constexpr double kEps = 1e-6;

double clampd(double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }

// Drops vertices no face uses.
Relief compactRelief(const Relief& r) {
    std::map<int, int> remap;
    Relief out;
    for (const auto& f : r.faces) {
        std::vector<int> loop;
        for (const int i : f) {
            auto it = remap.find(i);
            if (it == remap.end()) {
                it = remap.emplace(i, static_cast<int>(out.vertices.size())).first;
                out.vertices.push_back(r.vertices[static_cast<size_t>(i)]);
            }
            loop.push_back(it->second);
        }
        out.faces.push_back(std::move(loop));
    }
    return out;
}

// confineBrush, also telling whether anything had to move.
core::Brush confine(const core::Brush& b, double width, double height, bool& changed) {
    changed = false;
    bool out = false;
    for (const Vec3& v : b.vertices)
        if (v.x < -kEps || v.x > width + kEps || v.y < -kEps || v.y > height + kEps || std::abs(v.z) > kMaxDepth) out = true;
    if (!out) return b;
    std::vector<uint32_t> all(b.vertices.size());
    for (uint32_t i = 0; i < all.size(); ++i) all[i] = i;
    const mesh::OpResult r = mesh::transformVerts(b, all, [&](const Vec3& p, uint32_t) { return Vec3(clampd(p.x, 0, width), clampd(p.y, 0, height), clampd(p.z, -kMaxDepth, kMaxDepth)); });
    if (!r.ok()) return b;
    changed = true;
    return r.brush;
}

} // namespace

Relief flatRelief() {
    Relief r;
    r.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    r.faces = {{0, 1, 2, 3}};
    return r;
}

nlohmann::json reliefToJson(const Relief& r) {
    const auto rnd = [](double v) { return std::round(v * 1e6) / 1e6; };
    nlohmann::json out;
    out["vertices"] = nlohmann::json::array();
    for (const auto& v : r.vertices) out["vertices"].push_back({rnd(v[0]), rnd(v[1]), rnd(v[2])});
    out["faces"] = nlohmann::json::array();
    for (const auto& f : r.faces) out["faces"].push_back(f);
    return out;
}

bool reliefEqual(const Relief* a, const Relief* b) {
    if (!a || !b) return !a && !b;
    return reliefToJson(*a) == reliefToJson(*b);
}

core::Brush reliefToBrush(const Relief& r, double width, double height) {
    core::Brush b;
    b.id = "relief";
    for (const auto& v : r.vertices) b.vertices.push_back(Vec3(v[0] * width, v[1] * height, v[2]));
    uint32_t id = 0;
    for (const auto& loop : r.faces) {
        core::BrushFace f;
        f.id = id++;
        for (const int i : loop) f.loop.push_back(static_cast<uint32_t>(i));
        b.faces.push_back(std::move(f));
    }
    return b;
}

Relief brushToRelief(const core::Brush& b, double width, double height) {
    const double w = std::max(width, 1e-6), h = std::max(height, 1e-6);
    Relief r;
    for (const Vec3& v : b.vertices) r.vertices.push_back({clampd(v.x / w, 0, 1), clampd(v.y / h, 0, 1), clampd(v.z, -kMaxDepth, kMaxDepth)});
    for (const core::BrushFace& f : b.faces) {
        std::vector<int> loop;
        for (const uint32_t i : f.loop) loop.push_back(static_cast<int>(i));
        r.faces.push_back(std::move(loop));
    }
    return compactRelief(r);
}

core::Brush confineBrush(const core::Brush& b, double width, double height) {
    bool changed = false;
    return confine(b, width, height, changed);
}

core::Brush ensureBrush(const core::Brush& b, double width, double height) { return b.faces.empty() ? reliefToBrush(flatRelief(), width, height) : b; }

// ---- checks ---------------------------------------------------------------------------------------------------------------------------------
ReliefReport reliefReport(const core::Brush& b, double width, double height) {
    ReliefReport rep;
    rep.vertices = static_cast<int>(b.vertices.size());
    rep.faces = static_cast<int>(b.faces.size());
    for (const core::BrushFace& f : b.faces) rep.triangles += std::max(0, static_cast<int>(f.loop.size()) - 2);
    for (const Vec3& v : b.vertices) {
        rep.minZ = std::min(rep.minZ, v.z);
        rep.maxZ = std::max(rep.maxZ, v.z);
    }
    const double tol = 1e-4;
    const auto onBorder = [&](const Vec3& v) { return std::abs(v.x) < tol || std::abs(v.x - width) < tol || std::abs(v.y) < tol || std::abs(v.y - height) < tol; };
    const mesh::Topology t = mesh::buildTopology(b);
    int openInside = 0, nonManifold = 0, raisedBorder = 0;
    for (const mesh::EdgeFaces& e : t.edgeList) {
        if (e.faces.size() > 2) ++nonManifold;
        if (e.faces.size() == 1) {
            const Vec3 &p = b.vertices[e.a], &q = b.vertices[e.b];
            const bool sameSide = (std::abs(p.x) < tol && std::abs(q.x) < tol) || (std::abs(p.x - width) < tol && std::abs(q.x - width) < tol) ||
                                  (std::abs(p.y) < tol && std::abs(q.y) < tol) || (std::abs(p.y - height) < tol && std::abs(q.y - height) < tol);
            if (!sameSide) ++openInside;
        }
    }
    for (const Vec3& v : b.vertices)
        if (onBorder(v) && std::abs(v.z) > tol) ++raisedBorder;
    if (openInside > 0) rep.warnings.push_back(std::to_string(openInside) + " open edge(s) inside the tile: the surface behind would show through");
    if (raisedBorder > 0) rep.warnings.push_back(std::to_string(raisedBorder) + " vertex(es) on the tile border are not at depth 0: neighbouring repeats will not join");
    if (nonManifold > 0) rep.warnings.push_back(std::to_string(nonManifold) + " edge(s) shared by more than two faces");
    int backFacing = 0;
    for (const core::BrushFace& f : b.faces)
        if (b.faceNormal(f).z < -0.5) ++backFacing;
    if (backFacing > 0) rep.warnings.push_back(std::to_string(backFacing) + " face(s) look into the surface (flip them if they should face out)");
    double area = 0;
    for (const core::BrushFace& f : b.faces)
        for (const auto& tri : tp::triangulateFace(b, f)) {
            const Vec3 &a = b.vertices[tri[0]], &c = b.vertices[tri[1]], &d = b.vertices[tri[2]];
            area += std::abs((c.x - a.x) * (d.y - a.y) - (d.x - a.x) * (c.y - a.y)) / 2;
        }
    const double total = width * height;
    if (total > 0 && area < total * 0.999) rep.warnings.push_back("The mesh covers " + std::to_string(static_cast<int>(std::lround(area / total * 100))) + "% of the tile when seen from the front");
    return rep;
}

std::vector<float> reliefTriangles(const core::Brush& b, double width, double height) {
    std::vector<float> out;
    const double w = std::max(width, 1e-6), h = std::max(height, 1e-6);
    for (const core::BrushFace& f : b.faces) {
        const Vec3 n = b.faceNormal(f);
        for (const auto& tri : tp::triangulateFace(b, f))
            for (const uint32_t vi : tri) {
                const Vec3& p = b.vertices[vi];
                for (const double v : {p.x, p.y, p.z, n.x, n.y, n.z, p.x / w, 1 - p.y / h}) out.push_back(static_cast<float>(v));
            }
    }
    return out;
}

std::vector<float> reliefEdgeLines(const core::Brush& b) {
    std::vector<float> out;
    for (const mesh::EdgeFaces& e : mesh::buildTopology(b).edgeList)
        for (const uint32_t i : {e.a, e.b}) {
            const Vec3& p = b.vertices[i];
            out.push_back(static_cast<float>(p.x));
            out.push_back(static_cast<float>(p.y));
            out.push_back(static_cast<float>(p.z));
        }
    return out;
}

// ---- presets ----------------------------------------------------------------------------------------------------------------------------------
const std::vector<PresetInfo>& presets() {
    static const std::vector<PresetInfo> p = {
        {Preset::Flat, "flat", "Flat", "One quad at depth 0"},
        {Preset::Recess, "recess", "Recessed panel", "The middle sinks into the surface"},
        {Preset::Raise, "raise", "Raised panel", "The middle stands out of the surface"},
        {Preset::Window, "window", "Window", "A recessed frame with a pane set slightly forward"},
    };
    return p;
}

Relief buildPreset(Preset kind, double width, double height) {
    core::Brush b = reliefToBrush(flatRelief(), width, height);
    if (kind == Preset::Flat) return brushToRelief(b, width, height);
    const double s = std::max(1e-3, std::min(width, height)), depth = std::min(0.25, s * 0.08);
    std::vector<uint32_t> faces = {0};
    const auto run = [&](const std::function<mesh::OpResult(const core::Brush&, const std::vector<uint32_t>&)>& fn) {
        const mesh::OpResult r = fn(b, faces);
        if (r.ok()) {
            b = r.brush;
            faces = r.sel.faces;
        }
    };
    const auto inset = [](double amount) { return [amount](const core::Brush& x, const std::vector<uint32_t>& f) { return mesh::insetFaces(x, f, amount); }; };
    const auto extrude = [](double dist) { return [dist](const core::Brush& x, const std::vector<uint32_t>& f) { return mesh::extrudeFaces(x, f, dist); }; };
    run(inset(s * (kind == Preset::Window ? 0.1 : 0.12)));
    if (kind == Preset::Recess) {
        run(extrude(-depth));
    } else if (kind == Preset::Raise) {
        run(extrude(depth));
    } else {
        run(extrude(-depth));
        run(inset(s * 0.07));
        run(extrude(depth * 0.5));
    }
    return brushToRelief(b, width, height);
}

// ---- the session -------------------------------------------------------------------------------------------------------------------------------
std::vector<uint32_t> faceIdsTouching(const core::Brush& b, const mesh::MeshSel& s) {
    std::vector<uint32_t> out;
    for (const core::BrushFace& f : b.faces) {
        bool all = true;
        for (const uint32_t i : f.loop)
            if (std::find(s.verts.begin(), s.verts.end(), i) == s.verts.end()) all = false;
        if (all) out.push_back(f.id);
    }
    return out;
}

ReliefSession::ReliefSession(const Relief* relief, double width, double height) : width_(width), height_(height) {
    brush_ = reliefToBrush(relief ? *relief : flatRelief(), width, height);
}

void ReliefSession::resize(double width, double height) {
    if (width == width_ && height == height_) return;
    const Relief r = toRelief();
    width_ = width;
    height_ = height;
    brush_ = reliefToBrush(r, width, height);
    sel_ = {};
    past_.clear();
    future_.clear();
}

void ReliefSession::push() {
    past_.push_back({brush_, sel_});
    if (past_.size() > kHistoryLimit) past_.erase(past_.begin());
    future_.clear();
}

ReliefSession::Snapshot ReliefSession::settle(const mesh::OpResult& r) const {
    bool changed = false;
    const core::Brush c = confine(r.brush, width_, height_, changed);
    const mesh::Cleaned f = mesh::finish(ensureBrush(c, width_, height_), changed ? mesh::cleanSel(c, r.sel) : r.sel, {});
    return {f.brush, mesh::cleanSel(f.brush, f.sel)};
}

std::string ReliefSession::apply(const ReliefOp& op) {
    if (modalBase_) return "A tool is active";
    const mesh::OpResult r = op(brush_, sel_);
    if (!r.ok()) return error_ = r.error;
    error_.clear();
    push();
    Snapshot s = settle(r);
    brush_ = std::move(s.brush);
    sel_ = std::move(s.sel);
    return {};
}

void ReliefSession::beginModal() {
    if (!modalBase_) modalBase_ = Snapshot{brush_, sel_};
}

std::string ReliefSession::modalUpdate(const ReliefOp& op) {
    if (!modalBase_) return "No tool is active";
    const mesh::OpResult r = op(modalBase_->brush, modalBase_->sel);
    if (!r.ok()) return error_ = r.error;
    error_.clear();
    Snapshot s = settle(r);
    brush_ = std::move(s.brush);
    sel_ = std::move(s.sel);
    return {};
}

void ReliefSession::endModal(bool confirm) {
    if (!modalBase_) return;
    Snapshot base = std::move(*modalBase_);
    modalBase_.reset();
    const bool changed = brush_.vertices.size() != base.brush.vertices.size() || brush_.faces.size() != base.brush.faces.size() || reliefToJson(brushToRelief(brush_, width_, height_)) != reliefToJson(brushToRelief(base.brush, width_, height_));
    if (confirm && changed) {
        past_.push_back(std::move(base));
        if (past_.size() > kHistoryLimit) past_.erase(past_.begin());
        future_.clear();
    } else {
        brush_ = std::move(base.brush);
        sel_ = std::move(base.sel);
    }
}

bool ReliefSession::undo() {
    if (past_.empty() || modalBase_) return false;
    Snapshot s = std::move(past_.back());
    past_.pop_back();
    future_.push_back({brush_, sel_});
    brush_ = std::move(s.brush);
    sel_ = std::move(s.sel);
    return true;
}

bool ReliefSession::redo() {
    if (future_.empty() || modalBase_) return false;
    Snapshot s = std::move(future_.back());
    future_.pop_back();
    past_.push_back({brush_, sel_});
    brush_ = std::move(s.brush);
    sel_ = std::move(s.sel);
    return true;
}

void ReliefSession::setMode(mesh::SelMode mode) {
    if (mode == mode_) return;
    sel_ = mesh::convertSel(brush_, sel_, mode_, mode);
    mode_ = mode;
}
void ReliefSession::setSel(const mesh::MeshSel& s) { sel_ = mesh::cleanSel(brush_, s); }
void ReliefSession::selectAll() { sel_ = mesh::allSel(brush_); }
void ReliefSession::selectNone() { sel_ = {}; }
void ReliefSession::invert() { sel_ = mesh::invertSel(brush_, sel_, mode_); }
void ReliefSession::grow() { sel_ = mesh::growSel(brush_, sel_, mode_); }
void ReliefSession::shrink() { sel_ = mesh::shrinkSel(brush_, sel_, mode_); }

core::Vec3 ReliefSession::pivot() const {
    std::vector<uint32_t> verts = sel_.verts;
    if (verts.empty())
        for (uint32_t i = 0; i < brush_.vertices.size(); ++i) verts.push_back(i);
    return mesh::centroid(brush_, verts);
}

std::string ReliefSession::dissolve() {
    return apply([this](const core::Brush& b, const mesh::MeshSel& s) {
        return mode_ == mesh::SelMode::Vertex ? mesh::dissolveVerts(b, s.verts) : mode_ == mesh::SelMode::Edge ? mesh::dissolveEdges(b, s.edges) : mesh::dissolveFaces(b, s.faces);
    });
}
std::string ReliefSession::deleteSelectedFaces() {
    return apply([](const core::Brush& b, const mesh::MeshSel& s) { return mesh::deleteFaces(b, s.faces.empty() ? faceIdsTouching(b, s) : s.faces); });
}
std::string ReliefSession::merge() {
    return apply([](const core::Brush& b, const mesh::MeshSel& s) { return mesh::mergeVerts(b, s.verts); });
}
std::string ReliefSession::fill() {
    return apply([](const core::Brush& b, const mesh::MeshSel& s) { return mesh::fillHole(b, s.verts); });
}
std::string ReliefSession::flip() {
    return apply([](const core::Brush& b, const mesh::MeshSel& s) { return mesh::flipFaces(b, s.faces.empty() ? faceIdsTouching(b, s) : s.faces); });
}
std::string ReliefSession::setDepth(double z) {
    return apply([z](const core::Brush& b, const mesh::MeshSel& s) { return mesh::transformVerts(b, s.verts, [z](const Vec3& p, uint32_t) { return Vec3(p.x, p.y, z); }); });
}
void ReliefSession::reset() {
    push();
    brush_ = reliefToBrush(flatRelief(), width_, height_);
    sel_ = {};
    error_.clear();
}
void ReliefSession::load(const Relief& r) {
    push();
    brush_ = reliefToBrush(r, width_, height_);
    sel_ = {};
    error_.clear();
}

} // namespace native::tools::relief
