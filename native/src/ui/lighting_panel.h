// ui/lighting_panel -- the Lighting dock (10_UI_AND_INPUT.md U12): every light of the map in a list (name, class, brightness, colour) with Mute and Solo boxes. Muting and soloing
// only change what the live lighting (the Lit bake and the ray traced view) uses; the map is not edited and the export is not affected. Clicking a row selects the light.
// Thread: UI thread only.
// Spec: 10_UI_AND_INPUT.md section 7 (U12), 08_MILESTONES.md M11 item 11. Not done: brightness / colour sliders (the Details panel edits them), the false colour view,
// the stale chunks overlay (the live bake no longer has chunks to show).
#pragma once

#include <QStringList>
#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;
class QTreeWidget;

namespace native::ui {

class MainWindow;

class LightingPanel : public QWidget {
    Q_OBJECT
public:
    explicit LightingPanel(MainWindow& window, QWidget* parent = nullptr);

    /// Rebuilds the list from the document now (it follows edits by itself, a turn of the event loop later).
    void refreshNow();
    /// "name | class | brightness | mute | solo" per row ("x" in a box that is checked), in document order.
    QStringList rows() const;
    /// The summary line ("3 lights, 1 muted, solo: lamp").
    QString summary() const;
    /// What a click on the box of a light does (tests).
    void setMuteBox(const QString& id, bool checked);
    void setSoloBox(const QString& id, bool checked);

private:
    MainWindow& win_;
    QTreeWidget* tree_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* clear_ = nullptr;
    QTimer* timer_ = nullptr;
    bool filling_ = false;
};

} // namespace native::ui
