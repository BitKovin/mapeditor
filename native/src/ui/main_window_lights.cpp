// ui/main_window_lights -- MainWindow: muted / soloed lights of the live lighting (10_UI_AND_INPUT.md U12).
#include <QStatusBar>

#include "render/picking.h"
#include "ui/main_window.h"

namespace native::ui {

void MainWindow::setLightFilter(light::LightFilter filter) {
    const light::LightFilter now = lightFilter_ ? *lightFilter_ : light::LightFilter();
    if (filter == now) return;
    lightFilter_ = filter.active() ? std::make_shared<const light::LightFilter>(std::move(filter)) : nullptr;
    if (picker_ && picker_->snapshot()) feedLighting(picker_->snapshot());   // the bake / the ray traced view start again without (or with only) these lights
    emit lightFilterChanged();
}

void MainWindow::setLightMuted(const std::string& id, bool muted) {
    light::LightFilter f = lightFilter_ ? *lightFilter_ : light::LightFilter();
    if (muted) f.muted.insert(id);
    else f.muted.erase(id);
    setLightFilter(std::move(f));
}

void MainWindow::setLightSolo(const std::string& id) {
    light::LightFilter f = lightFilter_ ? *lightFilter_ : light::LightFilter();
    f.solo = id;
    setLightFilter(std::move(f));
}

void MainWindow::clearLightFilter() { setLightFilter({}); }

} // namespace native::ui
