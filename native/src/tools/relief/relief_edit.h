// tools/relief/relief_edit -- the editing state of the relief editor, without any widgets (port of shell/src/model/relief.ts edit-space part and model/reliefSession.ts).
// A relief (core::texplan::Relief, read by Core) is a small polygon mesh of a texture: x, y in 0..1 across one repeat, z the depth in metres. The editor works on the same mesh in
// metres ("edit space": x * width, y * height, z) as an open-surface core::Brush, so every mesh tool of tools/mesh applies unchanged. The session runs those tools, keeps the mesh
// inside its tile, records an undo history and holds the element selection; a modal tool (move / rotate / scale / extrude / inset / bevel / loop cut) re-runs its function from the
// mesh as it was when the modal began, so a whole drag is one undo step.
// Thread: any (pure). Spec: 07_EDITOR_PORT.md B (relief.ts, reliefSession.ts), 08_MILESTONES.md M11 item 5.
#pragma once

#include <core/brush.h>
#include <core/texplan.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "tools/mesh/ops.h"
#include "tools/mesh/topology.h"

namespace native::tools::relief {

using Relief = core::texplan::Relief;
inline constexpr double kMaxDepth = 8;   // metres either way (RELIEF_MAX_DEPTH)

/// One quad at depth 0: the start of every new relief.
Relief flatRelief();
/// The stored form (vertices rounded to 1e-6, faces as loops).
nlohmann::json reliefToJson(const Relief& r);
bool reliefEqual(const Relief* a, const Relief* b);   // null = no relief

// ---- edit space ----
/// The mesh in metres for a repeat `width` x `height` metres large, as the brush the mesh tools accept (id "relief", face ids = indices).
core::Brush reliefToBrush(const Relief& r, double width, double height);
/// Back to the stored form: positions clamped into the repeat and the depth limit, unused vertices dropped, faces renumbered.
Relief brushToRelief(const core::Brush& b, double width, double height);
/// Keeps every vertex inside the repeat and within the depth limit (faces that stop being planar are split by the tool that ran).
core::Brush confineBrush(const core::Brush& b, double width, double height);
/// A mesh with no faces is replaced by the flat quad.
core::Brush ensureBrush(const core::Brush& b, double width, double height);

struct ReliefReport {
    int vertices = 0, faces = 0, triangles = 0;
    double minZ = 0, maxZ = 0;   // deepest sink (<= 0) and highest rise (>= 0), metres
    std::vector<std::string> warnings;
};
/// What the status line says about the mesh: open edges inside the tile, a raised border (the repeats would not join), non-manifold edges, back faces, how much of the tile it covers.
ReliefReport reliefReport(const core::Brush& b, double width, double height);

/// Interleaved triangles for a viewer: position (3), normal (3), picture uv (2); the uv is the position across the repeat with v down (image convention).
std::vector<float> reliefTriangles(const core::Brush& b, double width, double height);
/// Edge list for line drawing, each edge once: 6 floats per edge.
std::vector<float> reliefEdgeLines(const core::Brush& b);

// ---- starting points ----
enum class Preset { Flat, Recess, Raise, Window };
struct PresetInfo {
    Preset id;
    const char* key;     // "flat", "recess", "raise", "window"
    const char* label;
    const char* hint;
};
const std::vector<PresetInfo>& presets();
/// A preset for a repeat `width` x `height` metres large; depths scale with the smaller side.
Relief buildPreset(Preset kind, double width, double height);

// ---- the session ----
using ReliefOp = std::function<mesh::OpResult(const core::Brush&, const mesh::MeshSel&)>;
inline constexpr size_t kHistoryLimit = 200;

class ReliefSession {
public:
    ReliefSession(const Relief* relief, double width, double height);

    const core::Brush& brush() const { return brush_; }
    const mesh::MeshSel& sel() const { return sel_; }
    mesh::SelMode mode() const { return mode_; }
    double width() const { return width_; }
    double height() const { return height_; }
    const std::string& error() const { return error_; }   // the last error of a tool, cleared by the next success
    bool canUndo() const { return !past_.empty(); }
    bool canRedo() const { return !future_.empty(); }
    bool inModal() const { return modalBase_.has_value(); }
    Relief toRelief() const { return brushToRelief(brush_, width_, height_); }
    /// The texture's size in the world changed: the same relief in the new size; selection and history are reset.
    void resize(double width, double height);

    /// Runs a tool on the current mesh as one undo step; returns an error text or "".
    std::string apply(const ReliefOp& op);
    // Modal tools.
    void beginModal();
    std::string modalUpdate(const ReliefOp& op);   // shows the result for the current parameter; errors leave the last good result on screen
    void endModal(bool confirm);
    bool undo();
    bool redo();

    // Selection (not part of the history, as in mesh mode).
    void setMode(mesh::SelMode mode);
    void setSel(const mesh::MeshSel& s);
    void selectAll();
    void selectNone();
    void invert();
    void grow();
    void shrink();
    bool hasSelection() const { return !sel_.verts.empty(); }
    core::Vec3 pivot() const;   // centre of the selected vertices, or of the whole mesh

    // One-shot tools.
    std::string dissolve();
    std::string deleteSelectedFaces();
    std::string merge();
    std::string fill();
    std::string flip();
    std::string setDepth(double z);   // every selected vertex to depth z (metres)
    void reset();                     // back to the flat quad
    void load(const Relief& r);       // replaces the mesh (a preset or another relief) as one undo step

private:
    struct Snapshot {
        core::Brush brush;
        mesh::MeshSel sel;
    };
    void push();
    Snapshot settle(const mesh::OpResult& r) const;

    core::Brush brush_;
    mesh::MeshSel sel_;
    mesh::SelMode mode_ = mesh::SelMode::Face;
    double width_, height_;
    std::vector<Snapshot> past_, future_;
    std::optional<Snapshot> modalBase_;
    std::string error_;
};

/// Ids of the faces whose every corner is selected (so vertex / edge selections can drive face tools).
std::vector<uint32_t> faceIdsTouching(const core::Brush& b, const mesh::MeshSel& s);

} // namespace native::tools::relief
