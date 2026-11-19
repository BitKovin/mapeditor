// render/viewport_window -- see viewport_window.h.
#include "render/viewport_window.h"

#include <QCoreApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QEvent>
#include <QExposeEvent>
#include <QResizeEvent>
#include <QtGui/qevent.h>

#include <algorithm>

namespace native::render {

ViewportWindow::ViewportWindow(RenderThread& renderThread, QVulkanInstance& instance, const ViewportSettings& settings, QWindow* parent)
    : QWindow(parent), renderThread_(renderThread), rig_(settings) {
    setSurfaceType(QSurface::VulkanSurface);
    setVulkanInstance(&instance);
    flightTimer_.setTimerType(Qt::PreciseTimer);
    flightTimer_.setInterval(8);
    QObject::connect(&flightTimer_, &QTimer::timeout, [this] { flightTick(); });
}

ViewportWindow::~ViewportWindow() {
    setKeyFilter(false);
    flightTimer_.stop();
    releasePointer();
    dropViewport();
}

QSize ViewportWindow::pixelSize() const { return size() * devicePixelRatio(); }

void ViewportWindow::ensureViewport() {
    if (id_ != 0 || !isExposed()) return;
    const QSize px = pixelSize();
    if (px.isEmpty()) return;
    const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(this);
    if (surface == VK_NULL_HANDLE) return;
    id_ = renderThread_.addViewport(surface, static_cast<uint32_t>(px.width()), static_cast<uint32_t>(px.height()));
    renderThread_.setClearColor(id_, clear_[0], clear_[1], clear_[2]);
    pushView();
}

void ViewportWindow::dropViewport() {
    if (id_ == 0) return;
    renderThread_.removeViewport(id_);   // blocks until the swapchain is destroyed: the surface may go away after this
    id_ = 0;
}

void ViewportWindow::pushSize() {
    if (id_ == 0) return;
    const QSize px = pixelSize();
    renderThread_.resizeViewport(id_, static_cast<uint32_t>(std::max(0, px.width())), static_cast<uint32_t>(std::max(0, px.height())));
}

void ViewportWindow::pushView() {
    if (id_ == 0) return;
    const QSize px = pixelSize();
    renderThread_.setView(id_, rig_.viewState(px.width(), px.height()));
}

void ViewportWindow::viewChanged() {
    pushView();
    if (onChanged_) onChanged_();
}

void ViewportWindow::setCameraMode(CameraMode mode) {
    if (mode == rig_.mode()) return;
    rig_.setMode(mode);
    viewChanged();
}

void ViewportWindow::requestRedraw() {
    if (id_ != 0) renderThread_.markDirty(id_);
}

void ViewportWindow::setClearColor(float r, float g, float b) {
    clear_[0] = r;
    clear_[1] = g;
    clear_[2] = b;
    if (id_ != 0) renderThread_.setClearColor(id_, r, g, b);
}

CapturedImage ViewportWindow::capture() {
    ensureViewport();
    if (id_ == 0) return {};
    return renderThread_.capture(id_);
}

bool ViewportWindow::event(QEvent* e) {
    if (e->type() == QEvent::PlatformSurface) {
        // Qt destroys the VkSurfaceKHR with the native window (re-docking, closing): release the swapchain first.
        if (static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            dropViewport();
    }
    return QWindow::event(e);
}

void ViewportWindow::exposeEvent(QExposeEvent*) {
    if (!isExposed()) return;
    ensureViewport();
    pushSize();
    pushView();
    requestRedraw();
}

void ViewportWindow::resizeEvent(QResizeEvent*) {
    ensureViewport();
    pushSize();
    pushView();
}

// ---- camera input (T1.4) ----------------------------------------------------------------------------------------------------
// The Shell's bindings: RMB drag looks (fly) / orbits (orbit) / pans (ortho), MMB drag pans, the wheel dollies / zooms, RMB held +
// W A S D Q E flies (Shift x3), RMB held + wheel changes the fly speed. The pointer is hidden and put back after every move so
// it never hits the screen edge (the Shell's pointer lock).

void ViewportWindow::grabPointer(const QPoint& globalPos) {
    if (!rmb_ && !mmb_) return;
    lockPos_ = globalPos;
    if (!cursorHidden_) {
        setCursor(Qt::BlankCursor);
        cursorHidden_ = true;
    }
}

void ViewportWindow::releasePointer() {
    if (cursorHidden_) {
        unsetCursor();
        cursorHidden_ = false;
    }
}

void ViewportWindow::stopFlight() {
    rig_.clearFlyKeys();
    rig_.setBoost(false);
    flightTimer_.stop();
}

void ViewportWindow::mousePressEvent(QMouseEvent* e) {
    requestActivate();
    if (onActivated_) onActivated_();
    if (e->button() == Qt::RightButton) rmb_ = true;
    else if (e->button() == Qt::MiddleButton) mmb_ = true;
    else return;
    grabPointer(e->globalPosition().toPoint());
    setKeyFilter(true);
    e->accept();
}

void ViewportWindow::mouseMoveEvent(QMouseEvent* e) {
    if (!rmb_ && !mmb_) return;
    const QPoint now = e->globalPosition().toPoint();
    const QPoint d = now - lockPos_;
    if (d.isNull()) return;
    const double h = std::max(1, height());
    bool changed = false;
    if (mmb_) {
        rig_.pan(d.x(), d.y(), h);
        changed = true;
    } else if (rmb_) {
        rig_.drag(d.x(), d.y(), h);
        changed = true;
    }
    QCursor::setPos(lockPos_);
    if (changed) viewChanged();
    e->accept();
}

void ViewportWindow::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton) {
        rmb_ = false;
        stopFlight();           // the fly keys belong to the camera only while the right button is held
    } else if (e->button() == Qt::MiddleButton) {
        mmb_ = false;
    } else {
        return;
    }
    if (!rmb_ && !mmb_) {
        releasePointer();
        setKeyFilter(false);
    }
    e->accept();
}

void ViewportWindow::wheelEvent(QWheelEvent* e) {
    // Qt: 120 per notch, positive = away from the user. Shell (browser): ~100 per notch, positive = towards the user.
    const double deltaY = -e->angleDelta().y() * (100.0 / 120.0);
    if (deltaY == 0) return;
    rig_.wheel(deltaY, rmb_);
    viewChanged();
    e->accept();
}

namespace {
bool flyKeyOf(int key, FlyKey& out) {
    switch (key) {
        case Qt::Key_W: out = FlyKey::Forward; return true;
        case Qt::Key_S: out = FlyKey::Back; return true;
        case Qt::Key_D: out = FlyKey::Right; return true;
        case Qt::Key_A: out = FlyKey::Left; return true;
        case Qt::Key_E: out = FlyKey::Up; return true;
        case Qt::Key_Q: out = FlyKey::Down; return true;
        default: return false;
    }
}
} // namespace

bool ViewportWindow::handleKey(QKeyEvent* e, bool press) {
    rig_.setBoost(e->modifiers().testFlag(Qt::ShiftModifier));
    FlyKey k;
    if (!flyKeyOf(e->key(), k)) return false;
    if (!press) {
        if (e->isAutoRepeat()) return true;
        rig_.setFlyKey(k, false);
        if (!rig_.flying()) flightTimer_.stop();
        return true;
    }
    if (!rmb_ || rig_.mode() != CameraMode::Fly) return false;
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
    if (e->isAutoRepeat()) return true;
    rig_.setFlyKey(k, true);
    if (!flightTimer_.isActive()) {
        flightClock_.start();
        flightTimer_.start();
    }
    return true;
}

void ViewportWindow::keyPressEvent(QKeyEvent* e) {
    if (handleKey(e, true)) e->accept();
}

void ViewportWindow::keyReleaseEvent(QKeyEvent* e) {
    if (handleKey(e, false)) e->accept();
}

void ViewportWindow::setKeyFilter(bool on) {
    if (on == keyFilter_ || !QCoreApplication::instance()) return;
    keyFilter_ = on;
    if (on) QCoreApplication::instance()->installEventFilter(this);
    else QCoreApplication::instance()->removeEventFilter(this);
}

bool ViewportWindow::eventFilter(QObject* watched, QEvent* e) {
    if (e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease) {
        auto* ke = static_cast<QKeyEvent*>(e);
        FlyKey k;
        if (flyKeyOf(ke->key(), k) && (rmb_ || rig_.flying())) return handleKey(ke, e->type() == QEvent::KeyPress);
    }
    return QWindow::eventFilter(watched, e);
}

void ViewportWindow::focusOutEvent(QFocusEvent*) {
    // Focus may merely move to the widget hosting this window while a button is held: the mouse stays captured and the keys are
    // taken by the application filter. Only losing the application itself ends the gesture.
    if (QGuiApplication::applicationState() == Qt::ApplicationActive) return;
    rmb_ = mmb_ = false;
    stopFlight();
    releasePointer();
    setKeyFilter(false);
}

void ViewportWindow::flightTick() {
    const double dt = std::min(0.1, flightClock_.restart() / 1000.0);
    if (!rig_.flying()) { flightTimer_.stop(); return; }
    if (rig_.advance(dt)) viewChanged();
}

} // namespace native::render
