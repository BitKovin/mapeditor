// render/viewport_window -- see viewport_window.h.
#include "render/viewport_window.h"

#include <QEvent>
#include <QExposeEvent>
#include <QResizeEvent>
#include <QtGui/qevent.h>

namespace native::render {

ViewportWindow::ViewportWindow(RenderThread& renderThread, QVulkanInstance& instance, QWindow* parent)
    : QWindow(parent), renderThread_(renderThread) {
    setSurfaceType(QSurface::VulkanSurface);
    setVulkanInstance(&instance);
}

ViewportWindow::~ViewportWindow() { dropViewport(); }

QSize ViewportWindow::pixelSize() const { return size() * devicePixelRatio(); }

void ViewportWindow::ensureViewport() {
    if (id_ != 0 || !isExposed()) return;
    const QSize px = pixelSize();
    if (px.isEmpty()) return;
    const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(this);
    if (surface == VK_NULL_HANDLE) return;
    id_ = renderThread_.addViewport(surface, static_cast<uint32_t>(px.width()), static_cast<uint32_t>(px.height()));
    renderThread_.setClearColor(id_, clear_[0], clear_[1], clear_[2]);
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
    requestRedraw();
}

void ViewportWindow::resizeEvent(QResizeEvent*) {
    ensureViewport();
    pushSize();
}

} // namespace native::render
