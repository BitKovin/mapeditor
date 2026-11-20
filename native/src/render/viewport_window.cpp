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

ViewState ViewportWindow::currentView() {
    const QSize px = pixelSize();
    return rig_.viewState(px.width(), px.height());
}

void ViewportWindow::setRubberBand(bool on, QPointF a, QPointF b) {
    if (id_ == 0) return;
    const double s = devicePixelRatio();
    renderThread_.setRubberBand(id_, on, static_cast<float>(a.x() * s), static_cast<float>(a.y() * s), static_cast<float>(b.x() * s), static_cast<float>(b.y() * s));
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

// ---- pointer lock, flight and input forwarding (T1.4, input moved to the router in T1.6) --------------------------------------
// The pointer is hidden and put back after every move so it never hits the screen edge (the Shell's pointer lock).

void ViewportWindow::grabPointer(const QPoint& globalPos) {
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

void ViewportWindow::resetInput() {
    lockedButtons_ = 0;
    releasePointer();
    stopFlight();
}

void ViewportWindow::flyKey(FlyKey key, bool down) {
    rig_.setFlyKey(key, down);
    if (down) {
        if (!flightTimer_.isActive()) {
            flightClock_.start();
            flightTimer_.start();
        }
    } else if (!rig_.flying()) {
        flightTimer_.stop();
    }
}

void ViewportWindow::mousePressEvent(QMouseEvent* e) {
    requestActivate();
    if (onActivated_) onActivated_();
    lastPos_ = e->position();
    const bool handled = hook_ && hook_(e, QPointF());
    const Qt::MouseButton b = e->button();
    if (handled && (b == Qt::RightButton || b == Qt::MiddleButton)) {
        lockedButtons_ |= 1u << (b == Qt::RightButton ? 2 : 3);
        grabPointer(e->globalPosition().toPoint());
    }
    if (handled) e->accept();
}

void ViewportWindow::mouseMoveEvent(QMouseEvent* e) {
    if (verifyButtons_ && lockedButtons_ != 0) {
        // The system is the authority on which buttons are down: a lock whose button is up lost its release (a popup or another window took the mouse
        // mid-drag). Drop it, or the pointer stays hidden and pinned and the camera never answers again.
        const unsigned down = (e->buttons().testFlag(Qt::RightButton) ? 1u << 2 : 0u) | (e->buttons().testFlag(Qt::MiddleButton) ? 1u << 3 : 0u);
        if ((lockedButtons_ & ~down) != 0) {
            lockedButtons_ &= down;
            if (lockedButtons_ == 0) releasePointer();
        }
    }
    if (e->buttons() == Qt::NoButton && lockedButtons_ == 0) {
        // a plain move (no button): the hook sees it too (hover highlights, modal operators follow the pointer); no action holds it
        const QPointF hover = e->position() - lastPos_;
        lastPos_ = e->position();
        if (hook_ && hook_(e, hover)) e->accept();
        return;
    }
    QPointF delta;
    if (lockedButtons_ != 0) {
        delta = QPointF(e->globalPosition().toPoint() - lockPos_);
        if (delta.isNull()) return;
    } else {
        delta = e->position() - lastPos_;
    }
    lastPos_ = e->position();
    const bool handled = hook_ && hook_(e, delta);
    if (lockedButtons_ != 0) QCursor::setPos(lockPos_);
    if (handled) e->accept();
}

void ViewportWindow::mouseReleaseEvent(QMouseEvent* e) {
    const bool handled = hook_ && hook_(e, QPointF());
    const Qt::MouseButton b = e->button();
    if (b == Qt::RightButton) lockedButtons_ &= ~(1u << 2);
    else if (b == Qt::MiddleButton) lockedButtons_ &= ~(1u << 3);
    if (lockedButtons_ == 0) releasePointer();
    if (handled) e->accept();
}

void ViewportWindow::wheelEvent(QWheelEvent* e) {
    if (hook_ && hook_(e, QPointF())) e->accept();
}

void ViewportWindow::focusOutEvent(QFocusEvent*) {
    // Focus may merely move to the widget hosting this window while a button is held: the mouse stays captured. Only losing the
    // application itself ends the gesture.
    if (QGuiApplication::applicationState() == Qt::ApplicationActive) return;
    lockedButtons_ = 0;
    releasePointer();
    stopFlight();
    if (onCancel_) onCancel_();
}

void ViewportWindow::flightTick() {
    const double dt = std::min(0.1, flightClock_.restart() / 1000.0);
    if (!rig_.flying()) { flightTimer_.stop(); return; }
    if (rig_.advance(dt)) viewChanged();
}

} // namespace native::render
