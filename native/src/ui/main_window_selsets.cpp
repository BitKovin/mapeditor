// ui/main_window_selsets -- named selection sets (10_UI_AND_INPUT.md U9): nine per map, kept in the user settings under the map's path (view state, never in the map). A set is
// the selection as it was saved; recalling selects what of it still exists in the document.
#include <QSettings>
#include <QStatusBar>

#include <nlohmann/json.hpp>

#include "doc/selection.h"
#include "ui/main_window.h"

namespace native::ui {

namespace {

// "selsets/<map path with / and \ turned into |>/<slot>"; an unsaved map has the name "(unsaved)".
std::string keyFor(const std::string& mapPath, int slot) {
    std::string p = mapPath.empty() ? "(unsaved)" : mapPath;
    for (char& c : p)
        if (c == '/' || c == '\\') c = '|';
    return "selsets/" + p + "/" + std::to_string(slot);
}

nlohmann::json toJson(const std::vector<doc::SelRef>& items) {
    nlohmann::json out = nlohmann::json::array();
    for (const doc::SelRef& r : items) {
        nlohmann::json j = {{"e", r.entityId}, {"b", r.brushId}, {"p", r.instancePath}};
        if (r.faceId) j["f"] = *r.faceId;
        out.push_back(std::move(j));
    }
    return out;
}

std::vector<doc::SelRef> fromJson(const nlohmann::json& j) {
    std::vector<doc::SelRef> out;
    if (!j.is_array()) return out;
    for (const nlohmann::json& o : j) {
        if (!o.is_object()) continue;
        doc::SelRef r;
        r.entityId = o.value("e", std::string());
        r.brushId = o.value("b", std::string());
        if (o.contains("p") && o["p"].is_array())
            for (const nlohmann::json& s : o["p"])
                if (s.is_string()) r.instancePath.push_back(s.get<std::string>());
        if (o.contains("f") && o["f"].is_number_unsigned()) r.faceId = o["f"].get<uint32_t>();
        if (r.entityId.empty() && r.instancePath.empty()) continue;
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace

bool MainWindow::hasSelectionSet(int slot) const {
    if (!document_ || slot < 1 || slot > 9) return false;
    const std::string key = keyFor(document_->path, slot);
    if (QSettings* st = services_.settings) return st->contains(QString::fromStdString(key));
    return selectionSets_.count(key) != 0;
}

bool MainWindow::saveSelectionSet(int slot) {
    if (!document_ || !selection_ || slot < 1 || slot > 9) return false;
    if (selection_->empty()) {
        statusBar()->showMessage(QStringLiteral("Nothing is selected: select something to save it as set %1").arg(slot), 4000);
        return false;
    }
    const std::string text = toJson(selection_->items()).dump();
    const std::string key = keyFor(document_->path, slot);
    if (QSettings* st = services_.settings) st->setValue(QString::fromStdString(key), QString::fromStdString(text));
    else selectionSets_[key] = text;
    statusBar()->showMessage(QStringLiteral("Selection set %1 saved (%2 items)").arg(slot).arg(selection_->items().size()), 3000);
    return true;
}

bool MainWindow::recallSelectionSet(int slot) {
    if (!document_ || !selection_ || slot < 1 || slot > 9) return false;
    std::string text;
    const std::string key = keyFor(document_->path, slot);
    if (QSettings* st = services_.settings) text = st->value(QString::fromStdString(key)).toString().toStdString();
    else if (auto it = selectionSets_.find(key); it != selectionSets_.end()) text = it->second;
    if (text.empty()) {
        statusBar()->showMessage(QStringLiteral("Selection set %1 is empty").arg(slot), 3000);
        return false;
    }
    const std::vector<doc::SelRef> items = fromJson(nlohmann::json::parse(text, nullptr, false));
    selection_->selectMany(*document_, items);
    selection_->follow(*document_, true);   // what was deleted since is dropped
    if (selection_->empty()) {
        statusBar()->showMessage(QStringLiteral("Selection set %1: nothing of it exists any more").arg(slot), 4000);
        return false;
    }
    statusBar()->showMessage(QStringLiteral("Selection set %1 (%2 items)").arg(slot).arg(selection_->items().size()), 3000);
    return true;
}

} // namespace native::ui
