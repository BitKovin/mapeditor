// ui/texture_browser_panel -- see texture_browser_panel.h.
#include "ui/texture_browser_panel.h"

#include <QApplication>
#include <QSignalBlocker>
#include <algorithm>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMetaObject>
#include <QPixmap>
#include <QPointer>
#include <QRunnable>
#include <QScrollBar>
#include <QSplitter>
#include <QStatusBar>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

#include "core/albedo.h"
#include "core/textures.h"
#include "tools/texturing/browser.h"
#include "ui/main_window.h"

namespace native::ui {

namespace {

constexpr int kThumb = 84;
constexpr int kPath = Qt::UserRole;

QString qs(const std::string& s) { return QString::fromStdString(s); }

// Decodes one texture to a thumbnail on a pool thread and posts the result to the panel.
class ThumbJob : public QRunnable {
public:
    ThumbJob(QPointer<TextureBrowserPanel> panel, std::filesystem::path root, std::shared_ptr<const core::GameProfile> profile, std::string path, std::shared_ptr<std::atomic<uint64_t>> gen, uint64_t myGen)
        : panel_(std::move(panel)), root_(std::move(root)), profile_(std::move(profile)), path_(std::move(path)), gen_(std::move(gen)), myGen_(myGen) {
        setAutoDelete(true);
    }
    void run() override {
        if (gen_->load() != myGen_) return;
        QImage thumb;
        int w = 0, h = 0;
        if (const auto file = core::readTexture(root_, *profile_, path_)) {
            if (const auto img = core::decodeImage(file->bytes, file->ext); img && img->width > 0 && img->height > 0 && img->rgba.size() >= static_cast<size_t>(img->width) * img->height * 4) {
                w = img->width;
                h = img->height;
                const QImage full(img->rgba.data(), w, h, w * 4, QImage::Format_RGBA8888);
                thumb = full.scaled(kThumb, kThumb, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);
            }
        }
        if (gen_->load() != myGen_) return;
        QPointer<TextureBrowserPanel> panel = panel_;
        const QString path = QString::fromStdString(path_);
        QMetaObject::invokeMethod(qApp, [panel, path, thumb, w, h] {
            if (panel) panel->thumbnailReady(path, thumb, w, h);
        }, Qt::QueuedConnection);
    }

private:
    QPointer<TextureBrowserPanel> panel_;
    std::filesystem::path root_;
    std::shared_ptr<const core::GameProfile> profile_;
    std::string path_;
    std::shared_ptr<std::atomic<uint64_t>> gen_;
    uint64_t myGen_;
};

} // namespace

TextureBrowserPanel::TextureBrowserPanel(MainWindow& window, QWidget* parent) : QWidget(parent), win_(window), generation_(std::make_shared<std::atomic<uint64_t>>(1)) {
    setObjectName(QStringLiteral("textureBrowser"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(4, 4, 4, 0);
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("search"));
    search_->setPlaceholderText(QStringLiteral("Search textures"));
    search_->setClearButtonEnabled(true);
    bar->addWidget(search_, 1);
    count_ = new QLabel(this);
    count_->setObjectName(QStringLiteral("count"));
    bar->addWidget(count_);
    v->addLayout(bar);
    auto* split = new QSplitter(Qt::Horizontal, this);
    folders_ = new QListWidget(split);
    folders_->setObjectName(QStringLiteral("folders"));
    grid_ = new QListWidget(split);
    grid_->setObjectName(QStringLiteral("grid"));
    grid_->setViewMode(QListView::IconMode);
    grid_->setIconSize(QSize(kThumb, kThumb));
    grid_->setGridSize(QSize(kThumb + 24, kThumb + 34));
    grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static);
    grid_->setUniformItemSizes(true);
    grid_->setWordWrap(true);
    grid_->setSelectionMode(QAbstractItemView::SingleSelection);
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    grid_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({140, 400});
    v->addWidget(split, 1);

    visibleTimer_ = new QTimer(this);
    visibleTimer_->setSingleShot(true);
    visibleTimer_->setInterval(60);
    QObject::connect(visibleTimer_, &QTimer::timeout, this, [this] { requestVisible(); });
    QObject::connect(grid_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { visibleTimer_->start(); });
    QObject::connect(search_, &QLineEdit::textChanged, this, [this](const QString& t) { setQuery(t); });
    QObject::connect(folders_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item, QListWidgetItem*) {
        if (item) setFolder(item->data(kPath).toString().toStdString());
    });
    QObject::connect(grid_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { choose(item->data(kPath).toString().toStdString(), true); });
    QObject::connect(grid_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) { win_.openTextureProperties(item->data(kPath).toString().toStdString()); });
    QObject::connect(grid_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        QListWidgetItem* item = grid_->itemAt(p);
        if (!item) return;
        const std::string path = item->data(kPath).toString().toStdString();
        QMenu menu(this);
        menu.addAction(QStringLiteral("Apply to selection"), [this, path] { choose(path, true); });
        menu.addAction(QStringLiteral("Set as active texture"), [this, path] { choose(path, false); });
        menu.addAction(QStringLiteral("Properties..."), [this, path] { win_.openTextureProperties(path); });
        menu.exec(grid_->viewport()->mapToGlobal(p));
    });
    QObject::connect(&win_, &MainWindow::entityDefinitionsChanged, this, [this] { reload(); });   // the game profile was (re)loaded
    QObject::connect(&win_, &MainWindow::assetsRootChanged, this, [this] { reload(); });
    rebuildFolders();
    rebuildGrid();
}

TextureBrowserPanel::~TextureBrowserPanel() { generation_->fetch_add(1); }

void TextureBrowserPanel::reload() {
    const auto profile = win_.gameProfile();
    const std::string root = win_.assetsRoot();
    generation_->fetch_add(1);   // the files may have changed
    thumbs_.clear();
    requested_.clear();
    std::vector<std::string> list;
    if (profile && !root.empty()) list = core::listTextures(root, *profile);
    setTextures(std::move(list));
}

void TextureBrowserPanel::setTextures(std::vector<std::string> textures) {
    all_ = std::move(textures);
    const std::vector<std::string> entries = tools::tex::folderEntries(all_);
    if (std::find(entries.begin(), entries.end(), folder_) == entries.end()) folder_.clear();   // the folder is gone
    rebuildFolders();
    rebuildGrid();
}

std::vector<std::string> TextureBrowserPanel::folders() const { return tools::tex::folderEntries(all_); }

void TextureBrowserPanel::setFolder(const std::string& folder) {
    if (folder == folder_ && !shown_.empty()) return;
    folder_ = folder;
    for (int i = 0; i < folders_->count(); ++i)
        if (folders_->item(i)->data(kPath).toString().toStdString() == folder_) {
            QSignalBlocker block(folders_);
            folders_->setCurrentRow(i);
        }
    rebuildGrid();
}

void TextureBrowserPanel::setQuery(const QString& query) {
    query_ = query.toStdString();
    if (search_->text() != query) {
        QSignalBlocker block(search_);
        search_->setText(query);
    }
    rebuildGrid();
}

bool TextureBrowserPanel::choose(const std::string& path, bool apply) {
    if (path.empty()) return false;
    win_.setActiveTexture(path);
    if (!apply) {
        win_.statusBar()->showMessage(QStringLiteral("Active texture: ") + qs(path), 4000);
        return true;
    }
    return win_.textureCommand("apply", {{"texture", path}});
}

void TextureBrowserPanel::rebuildFolders() {
    QSignalBlocker block(folders_);
    folders_->clear();
    int current = 0;
    int row = 0;
    for (const std::string& f : tools::tex::folderEntries(all_)) {
        auto* item = new QListWidgetItem(qs(tools::tex::folderLabel(f)), folders_);
        item->setData(kPath, qs(f));
        if (f == folder_) current = row;
        ++row;
    }
    folders_->setCurrentRow(current);
}

void TextureBrowserPanel::rebuildGrid() {
    shown_ = tools::tex::filterTextures(all_, folder_, query_);
    count_->setText(QString::number(shown_.size()) + QStringLiteral(" of ") + QString::number(all_.size()));
    grid_->setUpdatesEnabled(false);
    grid_->clear();
    for (const std::string& p : shown_) {
        const size_t slash = p.rfind('/');
        auto* item = new QListWidgetItem(qs(slash == std::string::npos ? p : p.substr(slash + 1)), grid_);
        item->setData(kPath, qs(p));
        item->setToolTip(qs(p));
        if (const auto it = thumbs_.find(p); it != thumbs_.end()) {
            item->setIcon(QIcon(QPixmap::fromImage(it->second.image)));
            item->setToolTip(qs(p) + QStringLiteral("\n%1 x %2 px").arg(it->second.width).arg(it->second.height));
        }
        item->setSizeHint(grid_->gridSize());
    }
    grid_->setUpdatesEnabled(true);
    visibleTimer_->start();
}

void TextureBrowserPanel::requestVisible() {
    const auto profile = win_.gameProfile();
    const std::string root = win_.assetsRoot();
    if (!profile || root.empty()) return;
    const QRect view = grid_->viewport()->rect().adjusted(0, -kThumb, 0, kThumb);
    for (int i = 0; i < grid_->count(); ++i) {
        QListWidgetItem* item = grid_->item(i);
        if (!view.intersects(grid_->visualItemRect(item))) continue;
        const std::string path = item->data(kPath).toString().toStdString();
        if (thumbs_.count(path) || requested_.count(path)) continue;
        requested_[path] = true;
        QThreadPool::globalInstance()->start(new ThumbJob(QPointer<TextureBrowserPanel>(this), root, profile, path, generation_, generation_->load()));
    }
}

void TextureBrowserPanel::thumbnailReady(const QString& path, const QImage& image, int width, int height) {
    const std::string p = path.toStdString();
    requested_.erase(p);
    if (image.isNull()) {   // not an image (or gone): remembered as blank so it is not asked again
        thumbs_[p] = {QImage(), 0, 0};
        return;
    }
    thumbs_[p] = {image, width, height};
    for (int i = 0; i < grid_->count(); ++i) {
        QListWidgetItem* item = grid_->item(i);
        if (item->data(kPath).toString() != path) continue;
        item->setIcon(QIcon(QPixmap::fromImage(image)));
        item->setToolTip(path + QStringLiteral("\n%1 x %2 px").arg(width).arg(height));
        break;
    }
}

} // namespace native::ui
