// ui/tileset_editor -- the editor of simple texture sets (shell/src/ui/tileSetEditor.ts, REQUIREMENTS section 20 simple sets): no rules, no text. A wall set is three lanes of pieces
// (Top / Middle / Bottom); a piece is a texture or a block of textures that always stay together (a door of two halves, a two storey window bay). A frame (9-slice) is nine slots.
// The preview on the right is the real layout (Core's solver). Pieces move between lanes with the lane box of the inspector (the Shell dragged them). Textures come from the Textures
// panel: "Use active texture" puts it on the selected tile; "Add textures..." picks several and sorts them into lanes by name.
// Thread: UI thread only.
// Spec: 07_EDITOR_PORT.md B (tileSetEditor), 08_MILESTONES.md M11 item 4.
#pragma once

#include <QDialog>
#include <QImage>

#include <core/texplan.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "tools/texturing/tileset_edit.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QWidget;

namespace native::ui {

class MainWindow;
class GroupPreviewCanvas;

class TileSetEditor : public QDialog {
    Q_OBJECT
public:
    // `name` empty: a new set of `kind`; else the set in that group file (a file without a kit opens an empty set and says so).
    TileSetEditor(MainWindow& window, const std::string& name, native::tools::tex::KitKind kind = native::tools::tex::KitKind::Wall);
    ~TileSetEditor() override;

    const std::string& groupName() const { return original_; }
    const native::tools::tex::Kit& kit() const { return kit_; }
    void setKit(const native::tools::tex::Kit& kit);
    bool valid() const { return valid_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    GroupPreviewCanvas* preview() const { return preview_; }
    void setName(const QString& name);
    // Selection: a piece of a lane, then one of its tiles; or a slot of a frame (0..8).
    void selectPiece(native::tools::tex::Lane lane, int index);
    void selectCell(int row, int col);
    void selectSlot(int slot);
    // The texture of the selected tile / slot ("" clears it).
    void setTargetTexture(const std::string& path);
    int addTextures(const std::vector<std::string>& paths);   // sorted into lanes by their names
    // The piece commands of the inspector.
    int addPiece(native::tools::tex::Lane lane, const std::string& texture);
    void moveSelectedTo(native::tools::tex::Lane lane);
    void duplicateSelected();
    void deleteSelected();
    std::string save();           // "" = saved, else the reason (also in the status line)
    std::string saveAndApply();   // saves, then lays the set over the selection
    QWidget* part(const QString& name) const;
    QString status() const;

signals:
    void saved(const QString& name);

private:
    void build();
    void refresh(bool boards = true);
    void rebuildLanes();
    void rebuildInspector();
    void rebuildFrame();
    void redraw();
    void showProblems();
    void clearBox(QWidget* box);
    QIcon iconOf(const native::tools::tex::KitPiece& p);
    native::tools::tex::KitPiece* selectedPiece();

    MainWindow& win_;
    std::string original_;
    native::tools::tex::Kit kit_;
    bool valid_ = false, building_ = false;
    std::vector<std::string> warnings_, solveWarnings_, kitWarnings_;
    std::string title_, description_;
    // selection
    std::optional<native::tools::tex::Lane> selLane_;
    int selIndex_ = -1, selRow_ = 0, selCol_ = 0, selSlot_ = -1;
    std::map<std::string, QImage> images_;
    QLineEdit *name_ = nullptr, *titleEdit_ = nullptr, *texture_ = nullptr, *match_ = nullptr;
    QLabel* status_ = nullptr;
    QWidget *lanesBox_ = nullptr, *inspectorBox_ = nullptr, *frameBox_ = nullptr, *cellsBox_ = nullptr, *pieceBox_ = nullptr;
    QListWidget* laneLists_[3] = {};
    QLabel* laneRows_[3] = {};
    QSpinBox *wide_ = nullptr, *tall_ = nullptr, *maxStretch_ = nullptr, *seed_ = nullptr;
    QComboBox *weight_ = nullptr, *place_ = nullptr, *laneBox_ = nullptr;
    QCheckBox *tileAuto_ = nullptr, *avoid_ = nullptr;
    QDoubleSpinBox *tileW_ = nullptr, *tileH_ = nullptr, *w_ = nullptr, *h_ = nullptr;
    QListWidget* problems_ = nullptr;
    GroupPreviewCanvas* preview_ = nullptr;
};

} // namespace native::ui
