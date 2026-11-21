// ui/texgroup_editor -- the texture group editor window (REQUIREMENTS section 20 T8, shell/src/ui/texGroupEditor.ts + groupPreview.ts): the group file as text on the left, a live
// preview of a wall / floor of a chosen size on the right (drawn from Core's solver and cell maps, so it shows the layout a face gets), the group's parameters, the seed, the slots
// with their textures, and every problem the reader and the solver found. Not a node-by-node visual tree editor. Save writes the file through Core and the scene is rebuilt;
// "Apply to selection" saves and lays the group over the selected faces / brushes.
// Thread: UI thread only.
// Spec: 07_EDITOR_PORT.md B (texGroupEditor, groupPreview), 08_MILESTONES.md M11 item 4.
#pragma once

#include <QDialog>
#include <QImage>
#include <QWidget>

#include <core/texplan.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;
class QTimer;

namespace native::ui {

class MainWindow;

// A group laid out as a face of w x h metres. y is up in the layout and down on the screen.
class GroupPreviewCanvas : public QWidget {
    Q_OBJECT
public:
    explicit GroupPreviewCanvas(QWidget* parent = nullptr);
    QSize sizeHint() const override { return {520, 440}; }
    using InfoSource = std::function<const core::texplan::TexInfo*(const std::string&)>;
    using ImageSource = std::function<QImage(const std::string&)>;
    void setSources(InfoSource info, ImageSource image);   // clears the picture cache
    void setLayout(std::vector<core::texplan::Cell> cells, double w, double h);
    const std::vector<core::texplan::Cell>& cells() const { return cells_; }
    void setOutlines(bool on) { outlines_ = on; update(); }
    void setLabels(bool on) { labels_ = on; update(); }
    int highlight() const { return highlight_; }
    void setHighlight(int index);
    int hovered() const { return hovered_; }
    // Pointer in canvas pixels (the mouse events call these; tests too).
    int cellAtPixel(const QPointF& p) const;
    QPointF pixelOf(double x, double y) const;   // layout metres -> canvas pixels
    void hover(const QPointF& p);
    void click(const QPointF& p);                // marks the cell under the pointer (again = unmark)

signals:
    void hoveredChanged(int cell);
    void highlightChanged(int cell);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    const QImage& image(const std::string& path);
    InfoSource info_;
    ImageSource imageOf_;
    std::map<std::string, QImage> images_;
    std::vector<core::texplan::Cell> cells_;
    double w_ = 9, h_ = 12;
    int highlight_ = -1, hovered_ = -1;
    bool outlines_ = true, labels_ = true;
};

class TexGroupEditor : public QDialog {
    Q_OBJECT
public:
    // `name` empty: a new group that starts from the first template.
    TexGroupEditor(MainWindow& window, const std::string& name);
    ~TexGroupEditor() override;

    const std::string& groupName() const { return original_; }
    QString text() const;
    void setText(const QString& text);       // as if typed: parsed and drawn at once
    void reparse();                          // text -> problems, parameters, slots, preview
    bool valid() const { return valid_; }
    const std::vector<std::string>& errors() const { return errors_; }
    const std::vector<std::string>& warnings() const { return warnings_; }   // the reader's, the solver's and the missing textures
    GroupPreviewCanvas* preview() const { return preview_; }
    // Saves under the name in the name field; "" = saved, else the reason (also shown in the status line).
    std::string save();
    // Saves, then applies to the selected faces; returns the result of save() when that failed, else "".
    std::string saveAndApply();
    void setName(const QString& name);
    void setSlot(const std::string& slot, const std::string& path);   // reformats the text
    // A named child widget: "name", "template", "text", "width", "height", "facing", "seed", "outlines", "labels", "params", "slots", "slotTexture", "problems", "info",
    // "param:<name>", "btn:save", "btn:apply", "btn:reroll", "btn:slotActive", "btn:slotSet".
    QWidget* part(const QString& name) const;
    QString status() const;

signals:
    void saved(const QString& name);

private:
    void build();
    void rebuildParams();
    void rebuildSlots();
    void redraw();
    void applyTemplate(int index);
    void showProblems();
    QString describe(int cell) const;
    void slotSelected();

    MainWindow& win_;
    std::string original_;
    bool valid_ = false;
    std::vector<std::string> errors_, warnings_, solveWarnings_;
    nlohmann::ordered_json parsed_;
    core::texplan::GroupPtr group_;
    std::map<std::string, core::texplan::ParamValue> values_;
    std::vector<std::string> paramKeys_;
    std::vector<std::pair<std::string, std::string>> slots_;
    QLineEdit* name_ = nullptr;
    QComboBox* template_ = nullptr;
    QPlainTextEdit* text_ = nullptr;
    QLabel *status_ = nullptr, *info_ = nullptr;
    QDoubleSpinBox *w_ = nullptr, *h_ = nullptr;
    QComboBox* facing_ = nullptr;
    QSpinBox* seed_ = nullptr;
    QCheckBox *outlines_ = nullptr, *labels_ = nullptr;
    QWidget* paramBox_ = nullptr;
    QListWidget *slotList_ = nullptr, *problems_ = nullptr;
    QLineEdit* slotTexture_ = nullptr;
    GroupPreviewCanvas* preview_ = nullptr;
    QTimer* timer_ = nullptr;
    bool filling_ = false;
};

} // namespace native::ui
