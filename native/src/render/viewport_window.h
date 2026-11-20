// render/viewport_window -- a QWindow with a Vulkan surface, shown inside a dock through QWidget::createWindowContainer.
//
// Owns: the Qt side of one viewport: it asks Qt for the VkSurfaceKHR (QVulkanInstance wraps the app's VkInstance), registers
// the surface with the RenderThread, forwards resize / expose events and drops the viewport *before* Qt destroys the surface
// (the render thread owns the swapchain, 05_RENDERER.md §3). Not QVulkanWindow: that creates its own device and frame loop.
// T1.4 added the camera; T1.6 moved its input out: the window forwards mouse and wheel events to an input hook (the router of the
// main window turns them into actions: RMB look, MMB pan, wheel dolly...) and keeps only the services an action cannot do from a
// callback: hiding the pointer and holding it in place while a dragging button is down (the Shell's pointer lock, delta reported to
// the hook) and the flight timer that moves the fly camera while keys are held. Keys reach the router through the application filter
// of the main window, not through this window.
// Thread: UI thread. The only file below ui/ that includes Qt headers (01_ARCHITECTURE.md §4).
// Spec: 05_RENDERER.md §3, 08_MILESTONES.md T1.3 and T1.4.
#pragma once

#include <QElapsedTimer>
#include <QEvent>
#include <QPoint>
#include <QPointF>
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
    // The view the next frame uses (matrices for picking). Positions of input events are in the logical pixels of size(): ndc = p / size * 2 - 1.
    ViewState currentView();
    // The last pointer position over this window (logical pixels), also without a button down.
    QPointF lastMousePos() const { return lastPos_; }
    // The box-select rectangle in logical window pixels; `on` false hides it.
    void setRubberBand(bool on, QPointF a = QPointF(), QPointF b = QPointF());
    void setCameraMode(CameraMode mode);
    // Called after every change made by user input (so the layout can be saved); UI thread.
    void setChangedCallback(std::function<void()> cb) { onChanged_ = std::move(cb); }
    // Called when a mouse button is pressed in this viewport (the UI marks it as the active one).
    void setActivatedCallback(std::function<void()> cb) { onActivated_ = std::move(cb); }
    // Mouse press / move / release and wheel events (QMouseEvent / QWheelEvent) go here first. `lockedDelta` is the pointer motion
    // since the lock position while the pointer is locked, else since the previous event. Returns true when an action took the event;
    // a handled right / middle press locks the pointer until the button is released.
    using InputHook = std::function<bool(QEvent* event, QPointF delta)>;
    void setInputHook(InputHook hook) { hook_ = std::move(hook); }
    // Called when the gesture is cancelled from outside (the application lost the focus): the router ends its holds.
    void setCancelCallback(std::function<void()> cb) { onCancel_ = std::move(cb); }
    // True while the pointer is locked (a dragging button is down).
    bool pointerGrabbed() const { return lockedButtons_ != 0; }

    // Flight service: the fly camera moves (in 8 ms ticks) while any fly key is down.
    void flyKey(FlyKey key, bool down);
    void setBoost(bool on) { rig_.setBoost(on); }
    void stopFlight();
    // Forgets everything a running gesture left behind: the pointer lock and its hidden cursor, the fly keys and the boost. Used when input is
    // cancelled (focus lost, a popup took the mouse) so nothing can stay "held" without the button or key being down.
    void resetInput();
    // Whether the system's button state (QMouseEvent::buttons) overrules a gesture that thinks a button is down (default). Tests that inject
    // presses without a real button turn it off, since the real moves that follow would report no button.
    void setVerifyButtons(bool on) { verifyButtons_ = on; }

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
    void focusOutEvent(QFocusEvent* e) override;

private:
    void ensureViewport();
    void dropViewport();
    void pushSize();
    void pushView();
    void grabPointer(const QPoint& globalPos);
    void releasePointer();
    void flightTick();
    QSize pixelSize() const;

    RenderThread& renderThread_;
    ViewportId id_ = 0;
    float clear_[3] = {0.105f, 0.105f, 0.115f};
    CameraRig rig_;
    std::function<void()> onChanged_, onActivated_, onCancel_;
    InputHook hook_;
    bool verifyButtons_ = true;
    unsigned lockedButtons_ = 0;        // bit per mouse button (1 << button number) that locked the pointer
    bool cursorHidden_ = false;
    QPoint lockPos_;
    QPointF lastPos_;
    QTimer flightTimer_;
    QElapsedTimer flightClock_;
};

} // namespace native::render
