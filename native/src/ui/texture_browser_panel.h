// ui/texture_browser_panel -- the Textures dock (the Content Browser of shell/src/ui/contentBrowser.ts): the folders of the game's texture list on the left, the thumbnails of the
// folder's textures (filtered by a search text) on the right. A click makes the texture the active one and applies it to the selection, a right click offers the same plus
// "Set active only". Thumbnails are decoded on a worker pool, only for the tiles that are on screen, and kept in a cache until the list is rescanned.
// The list is scanned from the assets folder (core::listTextures) when the window's game profile changes; `setTextures` gives it directly (tests).
// Thread: UI thread only (the decoding runs on QThreadPool and posts back).
// Spec: 07_EDITOR_PORT.md B (contentBrowser, textureThumbs), 08_MILESTONES.md M11 item 3.
#pragma once

#include <QImage>
#include <QWidget>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

class QLineEdit;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QTimer;

namespace native::ui {

class MainWindow;

class TextureBrowserPanel : public QWidget {
    Q_OBJECT
public:
    explicit TextureBrowserPanel(MainWindow& window, QWidget* parent = nullptr);
    ~TextureBrowserPanel() override;

    // Rescans the game's texture folder (nothing without an assets folder).
    void reload();
    // The list of the textures (what reload() finds); tests set it.
    void setTextures(std::vector<std::string> textures);
    const std::vector<std::string>& textures() const { return all_; }
    // Folder entries as listed ("" = all, tools::tex::kSkyFolder) and the shown textures.
    std::vector<std::string> folders() const;
    std::vector<std::string> shown() const { return shown_; }
    const std::string& folder() const { return folder_; }
    void setFolder(const std::string& folder);
    void setQuery(const QString& query);
    // A click on the tile: the texture becomes the active one and goes on the selection.
    bool choose(const std::string& path, bool apply = true);
    // Thumbnails decoded so far (tests wait for it).
    size_t thumbnailCount() const { return thumbs_.size(); }
    // Starts the decoding of the tiles that are on screen.
    void requestVisible();
    // The result of a decoding job (an empty image = not an image); called by the worker's job on the UI thread.
    void thumbnailReady(const QString& path, const QImage& image, int width, int height);

private:
    void rebuildFolders();
    void rebuildGrid();

    MainWindow& win_;
    QLineEdit* search_ = nullptr;
    QLabel* count_ = nullptr;
    QListWidget* folders_ = nullptr;
    QListWidget* grid_ = nullptr;
    QTimer* visibleTimer_ = nullptr;
    std::vector<std::string> all_, shown_;
    std::string folder_;
    std::string query_;
    struct Thumb {
        QImage image;
        int width = 0, height = 0;
    };
    std::map<std::string, Thumb> thumbs_;
    std::map<std::string, bool> requested_;
    std::shared_ptr<std::atomic<uint64_t>> generation_;   // jobs of an older generation (another assets folder) are dropped
};

} // namespace native::ui
