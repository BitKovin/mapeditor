// ui/main_window_texprops -- MainWindow: the texture properties (T11.4): reading and writing the sidecar next to a texture through Core, the texture picture, and the window that edits them.
#include <QImage>
#include <QStatusBar>

#include "core/albedo.h"
#include "core/textures.h"
#include "render/render_thread.h"
#include "tools/texturing/props_edit.h"
#include "ui/main_window.h"
#include "ui/relief_editor.h"
#include "ui/texture_props_window.h"

namespace native::ui {

nlohmann::json MainWindow::textureProps(const std::string& texture) const {
    if (propsRead_) return propsRead_(texture);
    if (!profile_ || assetsRoot_.empty()) return nlohmann::json::object();
    const auto props = core::readTextureProps(std::filesystem::path(assetsRoot_), *profile_, texture);
    return props ? *props : nlohmann::json::object();
}

std::string MainWindow::saveTextureProps(const std::string& texture, const nlohmann::json& props) {
    const nlohmann::json value = tools::tex::forWrite(props);
    std::string err;
    if (propsWrite_) err = propsWrite_(texture, value);
    else if (!profile_ || assetsRoot_.empty()) err = "no game folder";
    else err = core::writeTextureProps(std::filesystem::path(assetsRoot_), *profile_, texture, value);
    if (!err.empty()) {
        statusBar()->showMessage(QString::fromStdString("Texture properties: " + err), 6000);
        return err;
    }
    rebuildTextureEnvironment();   // every face that shows the texture is drawn again with the new properties
    if (services_.renderThread) services_.renderThread->reloadTexture(texture);   // the viewports read the transparency from the sidecar when the texture is loaded
    return {};
}

QImage MainWindow::loadTextureImage(const std::string& texture) const {
    if (!profile_ || assetsRoot_.empty()) return {};
    const auto file = core::readTexture(std::filesystem::path(assetsRoot_), *profile_, texture);
    if (!file) return {};
    const auto d = core::decodeImage(file->bytes, file->ext);
    if (!d || d->width <= 0 || d->height <= 0 || d->rgba.size() < static_cast<size_t>(d->width) * d->height * 4) return {};
    return QImage(d->rgba.data(), d->width, d->height, d->width * 4, QImage::Format_RGBA8888).copy();
}

TexturePropsDialog* MainWindow::openTextureProperties(const std::string& texture, bool show) {
    if (texture.empty()) return nullptr;
    if (propsDialog_) {
        if (propsDialog_->texture() == texture) {
            if (show) {
                propsDialog_->show();
                propsDialog_->raise();
            }
            return propsDialog_;
        }
        propsDialog_->flush();
        propsDialog_->close();
        propsDialog_->deleteLater();
        propsDialog_ = nullptr;
    }
    propsDialog_ = new TexturePropsDialog(*this, texture);
    QObject::connect(propsDialog_, &QObject::destroyed, this, [this](QObject* o) {
        if (o == propsDialog_) propsDialog_ = nullptr;
    });
    if (show) {
        propsDialog_->show();
        propsDialog_->raise();
    }
    return propsDialog_;
}

ReliefEditor* MainWindow::openReliefEditor(const std::string& texture, bool show) {
    if (texture.empty()) return nullptr;
    if (reliefEditor_) {
        reliefEditor_->discard();
        reliefEditor_->deleteLater();
        reliefEditor_ = nullptr;
    }
    reliefEditor_ = new ReliefEditor(*this, texture);
    QObject::connect(reliefEditor_, &QObject::destroyed, this, [this](QObject* o) {
        if (o == reliefEditor_) reliefEditor_ = nullptr;
    });
    if (show) {
        reliefEditor_->show();
        reliefEditor_->raise();
    }
    return reliefEditor_;
}

} // namespace native::ui
