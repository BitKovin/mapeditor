// ui/viewport_header -- the strip above each viewport (14_UI_REDESIGN.md section 6.3): the camera mode, the render mode and the Show (visibility) menus of THAT viewport, and its
// maximise and float buttons. Clicking anything on it makes the viewport the active one first, so the registry actions behind the menus (which act on the active viewport) hit the
// right one. The viewports are native windows, so the strip sits above them in the dock instead of over the image.
// Owns: four tool buttons. The menus are the menu bar's own (View > Camera, Render mode, Hide), shared. Thread: UI thread.
// Spec: 14_UI_REDESIGN.md section 6.3, 08_MILESTONES.md T14.4.
#pragma once

#include <QWidget>

class QToolButton;

namespace native::ui {

class MainWindow;

class ViewportHeader : public QWidget {
    Q_OBJECT
public:
    ViewportHeader(MainWindow& window, size_t index, QWidget* parent = nullptr);

    // Re-reads the texts (camera and render mode names) and the maximise button state.
    void refresh();

    QToolButton* cameraButton() const { return camera_; }
    QToolButton* renderButton() const { return render_; }
    QToolButton* showButton() const { return show_; }
    QToolButton* maximiseButton() const { return maximise_; }
    QToolButton* floatButton() const { return float_; }
    size_t index() const { return index_; }

protected:
    void mousePressEvent(QMouseEvent* e) override;   // a click on the empty strip activates the viewport
    void changeEvent(QEvent* e) override;

private:
    QToolButton* menuButton(const QString& objectName, const std::string& menuPath, const QString& tip);
    void refreshIcons();

    MainWindow& win_;
    size_t index_;
    QToolButton* camera_ = nullptr;
    QToolButton* render_ = nullptr;
    QToolButton* show_ = nullptr;
    QToolButton* maximise_ = nullptr;
    QToolButton* float_ = nullptr;
};

} // namespace native::ui
