// ui/main_window_edit -- delete, duplicate, group, the clipboard and hiding in the main window (T4.3): the document edits come from tools/edit_ops (one undo step each),
// the hide state from tools/visibility. The OS clipboard carries the Shell's clipboard JSON, so both editors paste each other's copies.
#include <QClipboard>
#include <QGuiApplication>
#include <DockWidget.h>

#include <QAction>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QTimer>
#include <QSettings>
#include <QStatusBar>

#include <cmath>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "doc/io.h"
#include "doc/level_settings.h"
#include "tools/context_menu.h"
#include "tools/texturing/face_edit.h"
#include "ui/app_actions.h"
#include "render/viewport_window.h"
#include "doc/selection.h"
#include "tools/prefabize.h"
#include "tools/select_ops.h"
#include "render/picking.h"
#include "render/render_thread.h"
#include "ui/hide_dialog.h"
#include "ui/main_window.h"

namespace native::ui {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

} // namespace

// ---- edits ---------------------------------------------------------------------------------------------------------------------------

bool MainWindow::applyEdit(const tools::EditResult& r, bool selectResult) {
    if (!r.ok()) {
        if (!r.error.empty()) statusBar()->showMessage(qs(r.error), 6000);
        return false;
    }
    if (!commitEdit(r.label, r.version)) return false;
    if (selectResult) {
        if (r.select.empty()) selection_->clear();
        else selection_->selectMany(*document_, r.select);
    }
    statusBar()->showMessage(qs(r.label), 3000);
    return true;
}

bool MainWindow::deleteSelected() {
    if (!document_ || selection_->empty()) return false;
    return applyEdit(tools::deleteSelection(document_, selection_->items()));
}

bool MainWindow::canSelectItems() const {
    return document_ && !meshMode() && !transforming() && !knifing() && !clipperActive() && !box_;
}

bool MainWindow::canNudge() const { return canSelectItems() && !transformSelection().empty(); }

bool MainWindow::selectAllItems() {
    if (!document_ || meshMode()) return false;
    const std::vector<doc::SelRef> all = tools::pruneHidden(*document_, tools::selectableItems(*document_), hide_);
    if (all.empty()) {
        statusBar()->showMessage(QStringLiteral("Nothing to select"), 3000);
        return false;
    }
    selection_->selectMany(*document_, all);
    statusBar()->showMessage(QString("Selected %1 item%2").arg(all.size()).arg(all.size() == 1 ? "" : "s"), 3000);
    return true;
}

bool MainWindow::invertSelectionItems() {
    if (!document_ || meshMode()) return false;
    const std::vector<doc::SelRef> rest = tools::pruneHidden(*document_, tools::invertedItems(*document_, selection_->items()), hide_);
    if (rest.empty()) {
        statusBar()->showMessage(QStringLiteral("Everything was selected"), 3000);
        return false;
    }
    selection_->selectMany(*document_, rest);
    statusBar()->showMessage(QString("Selected %1 item%2").arg(rest.size()).arg(rest.size() == 1 ? "" : "s"), 3000);
    return true;
}

bool MainWindow::deselectAll() {
    if (!selection_ || selection_->empty()) return false;
    selection_->clear();
    return true;
}

bool MainWindow::nudgeSelected(const std::string& key, bool fine) {
    if (!document_ || meshMode() || transforming()) return false;
    const std::vector<doc::SelRef> sels = transformSelection();
    if (sels.empty()) return false;
    render::ViewportWindow* vp = activeViewport();
    const render::CameraBasis b = vp ? vp->rig().active().basis() : render::CameraBasis{{1, 0, 0}, {0, 1, 0}, {0, 0, -1}};
    // looking straight down / up (top and bottom views): the screen's up is the forward of the ground
    const render::Vec3 fwd = std::fabs(b.forward.y) > 0.7 ? b.up : b.forward;
    tools::V3 dir = tools::nudgeDirection(key, {b.right.x, b.right.y, b.right.z}, {fwd.x, fwd.y, fwd.z});
    if (dir[0] == 0 && dir[1] == 0 && dir[2] == 0) return false;
    double step = vp ? render::snapStep(vp->rig().settings()) : 0.25;
    if (step <= 0) step = 0.25;
    if (fine) step /= 8;
    for (double& c : dir) c *= step;
    const auto after = tools::transformSelections(document_, sels, tools::translateBy(dir), false, 0, true);
    if (!after || after == document_) return false;
    if (!commitEdit("Nudge", after)) return false;
    statusBar()->showMessage(QString("Nudged %1 m").arg(step), 2000);
    return true;
}

bool MainWindow::duplicateSelected() {
    if (!document_ || selection_->empty()) return false;
    return applyEdit(tools::duplicateSelection(document_, selection_->items()));
}

bool MainWindow::groupSelected() {
    if (!document_ || selection_->empty()) return false;
    return applyEdit(tools::makeEntityFromSelection(document_, selection_->items(), "func_group"));
}

bool MainWindow::ungroupSelected() {
    if (!document_ || selection_->empty()) return false;
    return applyEdit(tools::moveSelectionToWorld(document_, selection_->items()));
}

bool MainWindow::makeEntitySelected(const std::string& classname) {
    if (!document_ || selection_->empty()) return false;
    return applyEdit(tools::makeEntityFromSelection(document_, selection_->items(), classname));
}

void MainWindow::showProperties() {
    if (!detailsDock_) return;
    detailsDock_->toggleView(true);
    detailsDock_->setAsCurrentTab();
}

bool MainWindow::surfaceOnto(bool flow, const tools::tex::FaceRef& to) {
    const auto from = selectedFaceRef();
    if (!document_ || !from) {
        statusBar()->showMessage(QStringLiteral("Select the face to copy from first"), 4000);
        return false;
    }
    const tools::tex::InfoFn info = textureInfo();
    return applyBrushEdit(flow ? tools::tex::flowTexture(document_, *from, to, info) : tools::tex::copySurface(document_, *from, to, info));
}

std::unique_ptr<QMenu> MainWindow::buildContextMenu(render::ViewportWindow* vp, QPointF p) {
    if (!vp || !document_ || !picker_ || transforming() || knifing() || clipperActive() || box_) return nullptr;
    std::optional<render::PickHit> hit;
    if (!meshMode()) {
        const QSizeF size = vp->size();
        if (size.width() < 1 || size.height() < 1) return nullptr;
        const render::Ray ray = render::rayThrough(vp->currentView().viewProj, p.x() / size.width() * 2.0 - 1.0, p.y() / size.height() * 2.0 - 1.0);
        hit = picker_->pick(ray, pickOptions());
    } else if (!meshTarget()) {
        return nullptr;
    }
    return contextMenuOf(contextEntries(hit), vp);
}

std::vector<tools::MenuEntry> MainWindow::contextEntries(const std::optional<render::PickHit>& hit) {
    if (meshMode()) return tools::meshContextMenu();
    const auto face = selectedFaceRef();
    if (face && hit && hit->faceId && hit->instance.empty() && !hit->pointEntity &&
        !(hit->entityId == face->entityId && hit->brushId == face->brushId && *hit->faceId == face->faceId)) {
        // a face is selected and another face is right-clicked: offer to copy onto it (the selection stays, so it can be pasted again)
        return tools::surfaceContextMenu(hit->entityId, hit->brushId, *hit->faceId);
    }
    if (hit) {   // what is under the pointer joins the selection unless it is part of it
        const doc::SelRef ref = doc::wholeEntityIfSole(*document_, hit->toSelRef(false));
        const auto& items = selection_->items();
        const bool selected = std::any_of(items.begin(), items.end(), [&](const doc::SelRef& s) { return s == ref || (s.entityId == ref.entityId && s.brushId.empty() && ref.instancePath.empty() && s.instancePath.empty()); });
        if (!selected) selectHit(*hit);
    }
    std::vector<std::string> groups;
    if (entityDefs_)
        for (const core::EntClass& c : entityDefs_->classes)
            if (c.kind == core::EntClass::Kind::Group) groups.push_back(c.name);
    return tools::objectContextMenu(groups, hide_.keys.size());
}

std::unique_ptr<QMenu> MainWindow::contextMenuOf(const std::vector<tools::MenuEntry>& entries, render::ViewportWindow* vp) {
    auto menu = std::make_unique<QMenu>();
    menu->setObjectName(QStringLiteral("contextMenu"));
    UiEnv ui{this, vp};
    const actions::ActionEnv env{&ui};
    std::function<void(QMenu*, const std::vector<tools::MenuEntry>&)> fill = [&](QMenu* into, const std::vector<tools::MenuEntry>& list) {
        for (const tools::MenuEntry& e : list) {
            if (e.separator) {
                into->addSeparator();
            } else if (!e.sub.empty()) {
                QMenu* sub = into->addMenu(qs(e.label));
                fill(sub, e.sub);
            } else {
                QAction* a = into->addAction(qs(e.label));
                const actions::ActionDef* def = registry_.find(e.action);
                a->setObjectName(qs(e.action));
                a->setEnabled(def && registry_.isEnabled(*def, env, e.params));
                const std::string action = e.action;
                const nlohmann::json params = e.params;
                QObject::connect(a, &QAction::triggered, this, [this, vp = QPointer<render::ViewportWindow>(vp), action, params] {
                    UiEnv run{this, vp ? vp.data() : activeViewport()};
                    actions::ActionEnv e2{&run};
                    registry_.run(action, e2, params);
                });
            }
        }
    };
    fill(menu.get(), entries);
    return menu;
}

bool MainWindow::liftMaterialAt(render::ViewportWindow* vp, QPointF p) {
    if (!vp || !document_ || !picker_ || meshMode() || transforming() || knifing() || clipperActive() || box_) return false;
    const QSizeF size = vp->size();
    if (size.width() < 1 || size.height() < 1) return false;
    const render::Ray ray = render::rayThrough(vp->currentView().viewProj, p.x() / size.width() * 2.0 - 1.0, p.y() / size.height() * 2.0 - 1.0);
    const std::optional<render::PickHit> hit = picker_->pick(ray, pickOptions());
    if (!hit || hit->brushId.empty() || !hit->faceId) {
        statusBar()->showMessage(QStringLiteral("Lift material: no face under the cursor"), 4000);
        return false;
    }
    const auto texture = tools::tex::liftMaterial(*document_, {hit->entityId, hit->brushId, *hit->faceId});
    if (!texture || texture->empty()) return false;
    setActiveTexture(*texture);
    statusBar()->showMessage(QString::fromStdString("Lifted " + *texture + "; click a face and a texture tile (or Apply) to use it"), 6000);
    return true;
}

void MainWindow::openContextMenu(render::ViewportWindow* vp, QPointF p) {
    if (!vp) return;
    // after the event that ended the right button has been handled (the menu takes the mouse; the router must not be in the middle of a hold)
    QTimer::singleShot(0, this, [this, vp = QPointer<render::ViewportWindow>(vp), p] {
        if (!vp) return;
        std::unique_ptr<QMenu> menu = buildContextMenu(vp, p);
        if (!menu) return;
        QMenu* raw = menu.release();
        raw->setAttribute(Qt::WA_DeleteOnClose);
        raw->popup(vp->mapToGlobal(p.toPoint()));
    });
}

bool MainWindow::turnIntoPrefab(const std::string& typedName) {
    namespace fs = std::filesystem;
    if (!document_ || selection_->empty()) return false;
    if (assetsRoot_.empty()) {
        statusBar()->showMessage(QStringLiteral("Turn into prefab: choose the game folder first"), 5000);
        return false;
    }
    const auto extract = tools::extractPrefab(*document_, selection_->items());
    if (!extract) {
        statusBar()->showMessage(QStringLiteral("Turn into prefab: nothing selected can go into a prefab"), 5000);
        return false;
    }
    std::string name = typedName;
    if (name.empty()) {
        const QString title = QStringLiteral("Turn into prefab"), label = QStringLiteral("Name of the prefab (a folder may come first, e.g. props/crate):");
        std::optional<std::string> answer;
        if (nameHandler_) {
            answer = nameHandler_(title, label, QString());
        } else {
            bool ok = false;
            const QString text = QInputDialog::getText(this, title, label, QLineEdit::Normal, QString(), &ok);
            if (ok) answer = text.toStdString();
        }
        if (!answer || answer->empty()) return false;
        name = *answer;
    }
    std::string problem;
    const std::string file = tools::prefabFileName(prefabRoot_, name, problem);
    if (file.empty()) {
        statusBar()->showMessage(qs(problem), 6000);
        return false;
    }
    const fs::path target = fs::path(assetsRoot_) / file;
    std::error_code ec;
    if (fs::exists(target, ec) && !confirm(QStringLiteral("Turn into prefab"), qs("\"" + file + "\" exists already. Replace it? (Instances of it in other maps change too.)"))) return false;

    const tools::EditResult swap = tools::replaceWithInstance(document_, selection_->items(), file, extract->pivot);
    if (!swap.ok()) {
        if (!swap.error.empty()) statusBar()->showMessage(qs(swap.error), 6000);
        return false;
    }
    const doc::LoadResult prefab = doc::loadDocumentText(extract->file.dump());
    if (!prefab.version) {
        reportError(qs("Turn into prefab: " + prefab.error));
        return false;
    }
    const std::string written = doc::saveDocumentFile(*prefab.version, target);
    if (!written.empty()) {
        reportError(qs("Turn into prefab: " + written));
        return false;
    }
    prefabLib_->refresh();
    emit prefabFileWritten();
    return applyEdit(swap);
}

// ---- the clipboard -------------------------------------------------------------------------------------------------------------------

bool MainWindow::copySelected() {
    if (!document_) return false;
    const auto payload = tools::buildClipboard(*document_, selection_->items());
    if (!payload) {
        statusBar()->showMessage(QStringLiteral("Copy: nothing selected"), 4000);
        return false;
    }
    if (QClipboard* cb = QGuiApplication::clipboard()) cb->setText(qs(tools::serializeClipboard(*payload)));
    statusBar()->showMessage(QStringLiteral("Copied %1 item%2").arg(payload->size()).arg(payload->size() > 1 ? "s" : ""), 4000);
    return true;
}

bool MainWindow::cutSelected() {
    if (!document_ || !copySelected()) return false;
    return deleteSelected();
}

bool MainWindow::pasteClipboard(const core::Vec3& offset) {
    if (!document_) return false;
    const QClipboard* cb = QGuiApplication::clipboard();
    const auto payload = cb ? tools::parseClipboard(cb->text().toStdString()) : std::nullopt;
    if (!payload) {
        statusBar()->showMessage(QStringLiteral("Paste: the clipboard holds no copied editor items"), 4000);
        return false;
    }
    return applyEdit(tools::pasteInto(document_, *payload, offset));
}

bool MainWindow::setLevelKeys(const std::string& label, const std::vector<std::pair<std::string, std::optional<std::string>>>& changes) {
    if (!document_) return false;
    const auto after = doc::setLevelKeys(document_, changes);
    return after && commitEdit(label, after);
}

// ---- hiding --------------------------------------------------------------------------------------------------------------------------

render::PickOptions MainWindow::pickOptions() const {
    render::PickOptions o;
    o.hidden = hiddenSet_.get();
    return o;
}

void MainWindow::refreshHidden() {
    std::shared_ptr<const std::unordered_set<scene::ObjectKey, scene::ObjectKey::Hash>> next;
    if (hide_.any() && picker_ && picker_->snapshot()) {
        auto set = tools::hiddenObjects(*picker_->snapshot(), hide_);
        if (!set.empty()) next = std::make_shared<const std::unordered_set<scene::ObjectKey, scene::ObjectKey::Hash>>(std::move(set));
    }
    const bool changed = (next != nullptr) != (hiddenSet_ != nullptr) || (next && hiddenSet_ && *next != *hiddenSet_);
    hiddenSet_ = std::move(next);
    if (changed && services_.renderThread) services_.renderThread->setHiddenObjects(hiddenSet_);
    if (changed) refreshModels();
    // a hidden thing cannot stay selected: it could still be moved or deleted unseen
    if (document_ && !selection_->empty()) {
        const std::vector<doc::SelRef> keep = tools::pruneHidden(*document_, selection_->items(), hide_);
        if (keep.size() != selection_->items().size()) {
            if (keep.empty()) selection_->clear();
            else selection_->selectMany(*document_, keep);
        }
    }
    if (changed) refreshOutline();
}

bool MainWindow::hideSelected() {
    if (!document_) return false;
    const std::vector<std::string> keys = tools::hideKeys(selection_->items());
    if (keys.empty()) return false;
    std::set<std::string> all = hide_.keys;
    all.insert(keys.begin(), keys.end());
    setHiddenKeys(all);
    statusBar()->showMessage(QStringLiteral("Hidden: %1. Unhide all shows everything again.").arg(keys.size()), 5000);
    return true;
}

void MainWindow::unhideAll() { setHiddenKeys({}); }

HideDialog* MainWindow::openHideDialog(bool show) {
    if (!hideDialog_) hideDialog_ = new HideDialog(*this);
    hideDialog_->refresh();
    if (show) {
        hideDialog_->show();
        hideDialog_->raise();
        hideDialog_->activateWindow();
    }
    return hideDialog_;
}

void MainWindow::setHiddenKeys(const std::set<std::string>& keys) {
    if (hide_.keys == keys) return;
    hide_.keys = keys;
    refreshHidden();
    refreshOverlays();
    emit hiddenChanged();
}

void MainWindow::setHideRules(const std::vector<tools::HideRule>& rules) {
    if (hide_.rules == rules) return;
    hide_.rules = rules;
    if (services_.settings)
        services_.settings->setValue("view/hideRules", qs(tools::hideRulesToJson(rules).dump()));
    refreshHidden();
    refreshOverlays();
    emit hiddenChanged();
}

} // namespace native::ui
