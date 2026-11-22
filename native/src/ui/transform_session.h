// ui/transform_session -- the state of one running transform: a modal G / R / S or a gizmo drag.
//
// Owns: what was selected and where the pivot was when the operation began (the document `base` it always starts from), the constraint axis, typed digits,
// the pointer at the start and now. MainWindow turns every pointer move into a document version from `base` and commits it into the open history interaction,
// so the whole operation is one undo step (ui/main_window_transform.cpp).
// Thread: UI thread.
// Spec: 08_MILESTONES.md T4.1, DESIGN sections 8 and 17.
#pragma once

#include <QPointF>

#include <memory>
#include <string>
#include <vector>

#include "doc/document.h"
#include "doc/selection.h"
#include "render/math3d.h"
#include "tools/mesh/topology.h"
#include "tools/resize2d.h"
#include "tools/modal.h"

namespace native::render { class ViewportWindow; }

namespace native::ui {

// The part of a session that edits the elements of one brush (mesh mode, T11.1g): G / R / S and the gizmo move the picked vertices; E, I, Ctrl+B and Ctrl+R take a
// distance / fraction from the mouse. Every update starts from the base version and the selection the session began with (vertex indices may change as faces split).
struct MeshSession {
    bool active = false;
    std::string entityId, brushId;                    // the edited brush in the base version
    std::string detailId;                             // the func_detail entity a concave result goes into (the same during the whole session)
    tools::mesh::SelMode mode = tools::mesh::SelMode::Vertex;
    tools::mesh::MeshSel startSel;
    bool proportional = false;
    double radius = 1;                                // metres
    std::vector<uint32_t> faces;                      // extrude / inset
    bool extrudeDrag = false;                         // a gizmo drag that extrudes the faces along the arrow
    std::vector<tools::mesh::Edge> edges;             // bevel
    render::Vec3 dir{0, 0, 0};                        // extrude: the average normal
    double t0 = 0;                                    // extrude: where the pointer's ray met the normal line at the start
    double metresPerPixel = 0;                        // inset / bevel: the scale of the mouse distance at the pivot
    int segments = 1;                                 // bevel
    tools::mesh::Edge edge;                           // loop cut: the edge whose ring is cut
    render::Vec3 railFrom{0, 0, 0}, railDir{0, 0, 0};  // loop cut: the first rail of the ring
    double railLength = 0;
    double fraction = 0.5;
    // what the last good update produced
    tools::mesh::MeshSel resultSel;
    doc::SelRef select;
    bool haveResult = false;
};

// A drag of one face along its normal (T11.2c): the handle of the selected face, or Alt + drag on any face. Every update drags the face of the base brush by the
// pointer's distance along the normal line (snapped), so the whole drag is one undo step.
struct FaceDragSession {
    bool active = false;
    std::string entityId, brushId;
    uint32_t faceId = 0;
    render::Vec3 dir{0, 0, 0};     // the face's outward normal at the start
};

// A 2D resize in an orthographic view (T11.2c): one of the eight handles of the selection's bounding rectangle.
struct ResizeSession {
    bool active = false;
    tools::V3 min{0, 0, 0}, max{0, 0, 0};
    tools::ScreenAxis right, up;
    int h = 0, v = 0;               // the handle
    double metresPerPixel = 0;
    double width = 0, height = 0;   // the size along the screen axes now, for the status line
};

// A drag of the angle handle of a point entity (REQUIREMENTS 33 H2): the plane the pointer's ray is intersected with, through the arrow's base.
struct AngleSession {
    bool active = false;
    std::string entityId;
    render::Vec3 base{0, 0, 0};
    render::Vec3 normal{0, 1, 0};
    bool keepPitch = true;   // the plane is horizontal: only the yaw follows
};

struct TransformSession {
    enum class Source { Modal, Gizmo };
    Source source = Source::Modal;
    tools::ModalKind kind = tools::ModalKind::Grab;   // Grab, Rotate or Scale
    render::ViewportWindow* viewport = nullptr;
    size_t panelIndex = 0;
    std::shared_ptr<const doc::DocumentVersion> base;
    std::vector<doc::SelRef> selection;
    render::Vec3 pivot;
    tools::Axis axis = -1;            // modal: the constraint (-1 = none); gizmo: the handle's axis
    std::string typed;                // typed number (modal)
    QPointF mouse0, mouse;            // logical pixels of the viewport window
    render::Vec3 startPoint;          // modal grab: the point under the pointer on the view plane at the start
    render::Vec3 forward;             // modal: the view direction
    tools::Pixel screenPivot;   // the pivot on the screen, logical pixels
    double startT = 0;                // gizmo: the drag's start along the axis (move / scale) or on the ring (rotate, radians)
    std::string originEntity;         // gizmo moving the origin of this entity alone
    double value = 0;                 // the current parameter (metres, radians or factor), for the status line
    std::string label;
    MeshSession mesh;
    FaceDragSession face;
    ResizeSession resize;
    AngleSession angle;
};

} // namespace native::ui
