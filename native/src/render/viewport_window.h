// render/viewport_window -- a QWindow with a Vulkan surface, shown inside a dock through QWidget::createWindowContainer.
//
// Owns: the Qt side of one viewport: it asks Qt for the VkSurfaceKHR (QVulkanInstance wraps the app's VkInstance), registers
// the surface with the RenderThread, forwards resize / expose events and drops the viewport *before* Qt destroys the surface
// (the render thread owns the swapchain, 05_RENDERER.md §3). Not QVulkanWindow: that creates its own device and frame loop.
// T1.4 adds the camera: the window owns the viewport's CameraRig, turns mouse / key events into rig calls (RMB look / orbit /
// ortho pan with the pointer hidden and held in place, MMB pan, wheel, RMB + WASDQE flight, Shift x3) and sends the resulting view
// to the render thread. The keys are fixed here; the action registry and input router of T1.6 take them over.
// Thread: UI thread. The only file below ui/ that includes Qt headers (01_ARCHITECTURE.md §4).
// Spec: 05_RENDERER.md §3, 08_MILESTONES.md T1.3 and T1.4.
#pragma once

#include <QElapsedTimer>
#include <QPoint>
#include <QTimer>
#include <QVulkanInstance>
#include <QWindow>

#include <functional>

#include "render/camera_rig.h"
#include "render/render_thread.h"

namespace native::render {

class ViewportWindow : public QWindow {
public:
    ViewportWindow(RenderThread& renderThread, QVulkanInstance& instance, const ViewportSettings& settings = defaultViewportSettings(),
                   QWindow* parent = nullptr);
    ~ViewportWindow() override;

    // 0 while the window has no surface (not yet exposed, or its native window was destroyed by a re-dock).
    ViewportId viewportId() const { return id_; }
    // Something this viewport shows changed: draw one frame (05 §3 redraw policy).
    void requestRedraw();
    void setClearColor(float r, float g, float b);
    // The cameras and settings of this viewport. After changing them directly call viewChanged().
    CameraRig& rig() { return rig_; }
    const CameraRig& rig() const { return rig_; }
    // Sends the rig's view to the render thread (one frame) and calls the change callback.
    void viewChanged();
    void setCameraMode(CameraMode mode);
    // Called after every change made by user input (so the layout can be saved); UI thread.
    void setChangedCallback(std::function<void()> cb) { onChanged_ = std::move(cb); }
    // Called when a mouse button is pressed in this viewport (the UI marks it as the active one).
    void setActivatedCallback(std::function<void()> cb) { onActivated_ = std::move(cb); }
    // True while a mouse button that grabs the pointer is down.
    bool pointerGrabbed() const { return rmb_ || mmb_; }

    // Renders the next frame and returns it (device pixels); empty on failure. Blocks the UI thread: tests / screenshots.
    CapturedImage capture();

protected:
    bool event(QEvent* e) override;
    void exposeEvent(QExposeEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    // While a mouse button is held the fly keys are taken from the whole application: keyboard focus may sit on another widget
    // (the container hosting this window) without that stopping the flight.
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    void ensureViewport();
    void dropViewport();
    void pushSize();
    void pushView();
    void grabPointer(const QPoint& globalPos);
    void releasePointer();
    void stopFlight();
    bool handleKey(QKeyEvent* e, bool press);
    void setKeyFilter(bool on);
    void flightTick();
    QSize pixelSize() const;

    RenderThread& renderThread_;
    ViewportId id_ = 0;
    float clear_[3] = {0.105f, 0.105f, 0.115f};
    CameraRig rig_;
    std::function<void()> onChanged_, onActivated_;
    bool rmb_ = false, mmb_ = false, cursorHidden_ = false, keyFilter_ = false;
    QPoint lockPos_;
    QTimer flightTimer_;
    QElapsedTimer flightClock_;
};

} // namespace native::render
