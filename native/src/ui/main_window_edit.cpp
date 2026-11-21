// ui/main_window_edit -- delete, duplicate, group, the clipboard and hiding in the main window (T4.3): the document edits come from tools/edit_ops (one undo step each),
// the hide state from tools/visibility. The OS clipboard carries the Shell's clipboard JSON, so both editors paste each other's copies.
#include <QClipboard>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QSettings>
#include <QStatusBar>

#include <filesystem>

#include <nlohmann/json.hpp>

#include "doc/io.h"
#include "doc/selection.h"
#include "tools/prefabize.h"
#include "render/picking.h"
#include "render/render_thread.h"
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
