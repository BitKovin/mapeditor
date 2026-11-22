// ui/viewport_header -- see viewport_header.h.
#include "ui/viewport_header.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QStyle>
#include <QToolButton>

#include "render/viewport_window.h"
#include "ui/icons.h"
#include "ui/main_window.h"
#include "ui/registry_menus.h"

namespace native::ui {

namespace {

const QString kArrow = QString::fromUtf8(" \xE2\x96\xBE");

QString cameraName(render::CameraMode m) {
    switch (m) {
        case render::CameraMode::Fly: return QStringLiteral("Persp");
        case render::CameraMode::Orbit: return QStringLiteral("Orbit");
        case render::CameraMode::Top: return QStringLiteral("Top");
        case render::CameraMode::Front: return QStringLiteral("Front");
        case render::CameraMode::Side: return QStringLiteral("Side");
    }
    return QStringLiteral("View");
}

QString renderName(render::RenderMode m) {
    switch (m) {
        case render::RenderMode::Unlit: return QStringLiteral("Unlit");
        case render::RenderMode::Lit: return QStringLiteral("Lit");
        case render::RenderMode::Wireframe: return QStringLiteral("Wireframe");
        case render::RenderMode::LightingOnly: return QStringLiteral("Lighting");
    }
    return QStringLiteral("Render");
}

} // namespace

ViewportHeader::ViewportHeader(MainWindow& window, size_t index, QWidget* parent) : QWidget(parent), win_(window), index_(index) {
    setObjectName(QStringLiteral("viewportHeader"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(26);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(6, 0, 4, 0);
    row->setSpacing(2);
    camera_ = menuButton(QStringLiteral("viewportHeader:camera"), "&View/&Camera (active viewport)", QStringLiteral("Camera of this view: perspective, orbit, top, front, side"));
    render_ = menuButton(QStringLiteral("viewportHeader:render"), "&View/&Render mode (active viewport)", QStringLiteral("How this view is drawn: unlit, lit (the live lighting), wireframe, lighting only"));
    show_ = menuButton(QStringLiteral("viewportHeader:show"), "&View/&Hide", QStringLiteral("What is shown: hide classes of objects (triggers, clips, lights ...)"));
    row->addWidget(camera_);
    row->addWidget(render_);
    row->addWidget(show_);
    row->addStretch(1);
    maximise_ = new QToolButton(this);
    maximise_->setObjectName(QStringLiteral("viewportHeader:maximise"));
    maximise_->setCheckable(true);
    maximise_->setToolTip(QStringLiteral("Show only this view, or all of them again"));
    connect(maximise_, &QToolButton::clicked, this, [this] {
        win_.setActiveViewport(index_);
        win_.toggleMaximizeViewport(index_);
    });
    row->addWidget(maximise_);
    float_ = new QToolButton(this);
    float_->setObjectName(QStringLiteral("viewportHeader:float"));
    float_->setToolTip(QStringLiteral("Move this view into its own window (drag it to another monitor)"));
    connect(float_, &QToolButton::clicked, this, [this] { win_.floatViewport(index_); });
    row->addWidget(float_);
    refreshIcons();
    refresh();
    connect(&win_, &MainWindow::viewportsChanged, this, [this] { refresh(); });
}

QToolButton* ViewportHeader::menuButton(const QString& objectName, const std::string& menuPath, const QString& tip) {
    auto* b = new QToolButton(this);
    b->setObjectName(objectName);
    b->setToolTip(tip);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    b->setCursor(Qt::PointingHandCursor);
    connect(b, &QToolButton::clicked, this, [this, b, menuPath] {
        win_.setActiveViewport(index_);   // the menus act on the active viewport
        RegistryMenus* menus = win_.menus();
        QMenu* menu = menus ? menus->menu(menuPath) : nullptr;
        if (!menu) return;
        menus->refresh();
        menu->popup(b->mapToGlobal(QPoint(0, b->height())));
    });
    return b;
}

void ViewportHeader::refresh() {
    const std::vector<ViewportPanel>& panels = win_.viewports();
    if (index_ >= panels.size() || !panels[index_].window) return;
    const render::ViewportSettings& s = panels[index_].window->rig().settings();
    camera_->setText(cameraName(panels[index_].window->rig().mode()) + kArrow);
    render_->setText(renderName(s.renderMode) + kArrow);
    show_->setText(QStringLiteral("Show") + kArrow);
    maximise_->setVisible(panels.size() > 1 || win_.maximisedViewport() >= 0);
    maximise_->setChecked(win_.maximisedViewport() == static_cast<int>(index_));
    const bool active = win_.activeViewportIndex() == static_cast<int>(index_);
    if (property("active").toBool() != active) {   // the strip of the active viewport has the accent line (theme.cpp)
        setProperty("active", active);
        style()->unpolish(this);
        style()->polish(this);
    }
}

void ViewportHeader::refreshIcons() {
    if (!maximise_ || !float_) return;
    maximise_->setIcon(icon("maximize"));
    float_->setIcon(icon("popout"));
}

void ViewportHeader::mousePressEvent(QMouseEvent* e) {
    win_.setActiveViewport(index_);
    QWidget::mousePressEvent(e);
}

void ViewportHeader::changeEvent(QEvent* e) {
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::ApplicationPaletteChange || e->type() == QEvent::StyleChange) refreshIcons();
}

} // namespace native::ui
