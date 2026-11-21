// ui/main_window_ftt -- MainWindow: what the Fast Texture Tool window asks of it (T11.3g): the faces it edits (the selected faces of one brush, else the faces of the selected
// brush), one-step edits (mirror, rotate, align edge, unwrap) and the live drag of the island (one undo step for the whole drag, Esc cancels).
#include <QStatusBar>

#include <algorithm>

#include "doc/commands.h"
#include "doc/edit.h"
#include "doc/selection.h"
#include "tools/texturing/face_edit.h"
#include "tools/texturing/ftt.h"
#include "ui/ftt_window.h"
#include "ui/main_window.h"

namespace native::ui {

namespace tex = tools::tex;

std::optional<FttTarget> MainWindow::fttTarget() const {
    if (!document_) return std::nullopt;
    // faces: the selected faces of the first selected face's brush; a brush: all its faces
    std::optional<FttTarget> t;
    for (const doc::SelRef& s : selection_->items()) {
        if (!s.instancePath.empty() || s.brushId.empty()) continue;
        if (!t) {
            t = FttTarget{s.entityId, s.brushId, {}};
        } else if (t->entityId != s.entityId || t->brushId != s.brushId) {
            continue;   // one brush only: the first one counts
        }
        if (s.faceId) t->faceIds.push_back(*s.faceId);
    }
    if (!t) return std::nullopt;
    const core::Brush* b = tex::findBrushOf(*document_, {t->entityId, t->brushId, 0});
    if (!b) return std::nullopt;
    if (t->faceIds.empty())
        for (const core::BrushFace& f : b->faces) t->faceIds.push_back(f.id);
    std::sort(t->faceIds.begin(), t->faceIds.end());
    t->faceIds.erase(std::unique(t->faceIds.begin(), t->faceIds.end()), t->faceIds.end());
    return t;
}

const core::Brush* MainWindow::fttBrush(const FttTarget& t) const { return document_ ? tex::findBrushOf(*document_, {t.entityId, t.brushId, 0}) : nullptr; }

namespace {

std::shared_ptr<const doc::DocumentVersion> withBrush(const std::shared_ptr<const doc::DocumentVersion>& base, const std::string& entityId, const core::Brush& brush) {
    doc::VersionEditor ed(base);
    if (!ed.replaceBrush(entityId, brush)) return nullptr;
    return ed.finish();
}

} // namespace

bool MainWindow::fttApply(const std::string& label, const tex::Xform& xform) {
    const auto t = fttTarget();
    if (!t || !document_) return false;
    const core::Brush* b = fttBrush(*t);
    if (!b) return false;
    const tex::FttResult r = tex::transformFaces(*b, t->faceIds, xform, textureInfo());
    if (r.changed == 0) {
        statusBar()->showMessage(QStringLiteral("Nothing changed"), 3000);
        return false;
    }
    const auto after = withBrush(document_, t->entityId, r.brush);
    return after && commitEdit(label, after);
}

size_t MainWindow::fttUnwrap(tex::UnwrapMode mode) {
    const auto t = fttTarget();
    if (!t || !document_) return 0;
    const core::Brush* b = fttBrush(*t);
    if (!b) return 0;
    const tex::FttResult r = tex::unwrapFaces(*b, t->faceIds, mode, textureInfo());
    if (r.changed == 0) return 0;
    const auto after = withBrush(document_, t->entityId, r.brush);
    const char* label = mode == tex::UnwrapMode::Follow ? "Unwrap UV (follow quads)" : mode == tex::UnwrapMode::Ortho ? "Unwrap UV (ortho)" : "Unwrap UV (axial)";
    if (!after || !commitEdit(label, after)) return 0;
    return r.changed;
}

bool MainWindow::fttBeginDrag(const std::string& label) {
    if (!document_ || fttDrag_ || transform_) return false;
    fttDrag_ = FttDrag{document_, label};
    history_->beginInteraction(label);
    return true;
}

bool MainWindow::fttDragTo(const FttTarget& t, const tex::Xform& xform) {
    if (!fttDrag_) return false;
    const core::Brush* b = tex::findBrushOf(*fttDrag_->base, {t.entityId, t.brushId, 0});
    if (!b) return false;
    const tex::FttResult r = tex::transformFaces(*b, t.faceIds, xform, textureInfo());
    const auto after = r.changed == 0 ? fttDrag_->base : withBrush(fttDrag_->base, t.entityId, r.brush);
    if (!after) return false;   // a refused edit: the last good version stays
    if (after != document_) history_->commit({fttDrag_->label, document_, after});
    return true;
}

void MainWindow::fttEndDrag(bool keep) {
    if (!fttDrag_) return;
    if (!keep && document_ != fttDrag_->base) history_->commit({fttDrag_->label, document_, fttDrag_->base});
    history_->endInteraction();
    fttDrag_.reset();
}

void MainWindow::openFastTextureTool() {
    if (!fttWindow_) fttWindow_ = new FttWindow(*this);
    fttWindow_->show();
    fttWindow_->raise();
    fttWindow_->refreshNow(true);
}

void MainWindow::toggleFastTextureTool() {
    if (fttWindow_ && fttWindow_->isVisible()) fttWindow_->close();
    else openFastTextureTool();
}

} // namespace native::ui
