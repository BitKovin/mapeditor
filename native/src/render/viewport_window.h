// render/viewport_window -- a QWindow with a Vulkan surface, shown inside a dock through QWidget::createWindowContainer.
//
// Owns: the Qt side of one viewport: it asks Qt for the VkSurfaceKHR (QVulkanInstance wraps the app's VkInstance), registers
// the surface with the RenderThread, forwards resize / expose events and drops the viewport *before* Qt destroys the surface
// (the render thread owns the swapchain, 05_RENDERER.md §3). Not QVulkanWindow: that creates its own device and frame loop.
// Thread: UI thread. The only file below ui/ that includes Qt headers (01_ARCHITECTURE.md §4).
// Spec: 05_RENDERER.md §3, 08_MILESTONES.md T1.3.
#pragma once

#include <QVulkanInstance>
#include <QWindow>

#include "render/render_thread.h"

namespace native::render {

class ViewportWindow : public QWindow {
public:
    ViewportWindow(RenderThread& renderThread, QVulkanInstance& instance, QWindow* parent = nullptr);
    ~ViewportWindow() override;

    // 0 while the window has no surface (not yet exposed, or its native window was destroyed by a re-dock).
    ViewportId viewportId() const { return id_; }
    // Something this viewport shows changed: draw one frame (05 §3 redraw policy).
    void requestRedraw();
    void setClearColor(float r, float g, float b);
    // Renders the next frame and returns it (device pixels); empty on failure. Blocks the UI thread: tests / screenshots.
    CapturedImage capture();

protected:
    bool event(QEvent* e) override;
    void exposeEvent(QExposeEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void ensureViewport();
    void dropViewport();
    void pushSize();
    QSize pixelSize() const;

    RenderThread& renderThread_;
    ViewportId id_ = 0;
    float clear_[3] = {0.105f, 0.105f, 0.115f};
};

} // namespace native::render
