// ui/status_bar -- see status_bar.h.
#include "ui/status_bar.h"

#include <QLabel>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>

#include "render/viewport_window.h"
#include "tools/status_text.h"
#include "ui/main_window.h"

namespace native::ui {

namespace {
QString qs(const std::string& s) { return QString::fromUtf8(s.c_str()); }

QLabel* field(QStatusBar* bar, const char* name) {
    auto* l = new QLabel(bar);
    l->setObjectName(QString::fromLatin1(name));
    l->setMargin(2);
    l->setFrameShape(QFrame::NoFrame);
    bar->addPermanentWidget(l);
    return l;
}
} // namespace

StatusFields::StatusFields(MainWindow& window) : QObject(&window), win_(window) {
    QStatusBar* bar = window.statusBar();
    tool_ = field(bar, "status:tool");
    grid_ = field(bar, "status:grid");
    selection_ = field(bar, "status:selection");
    map_ = field(bar, "status:map");
    folder_ = new QToolButton(bar);
    folder_->setObjectName(QStringLiteral("status:folder"));
    folder_->setAutoRaise(true);
    folder_->setToolTip(QStringLiteral("The game folder. Click to open Settings."));
    bar->addPermanentWidget(folder_);
    QObject::connect(folder_, &QToolButton::clicked, &window, [&window] { window.openSettings(0); });

    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(0);
    QObject::connect(timer_, &QTimer::timeout, this, [this] { refreshNow(); });
    for (const auto signal : {&MainWindow::selectionChanged, &MainWindow::toolChanged, &MainWindow::documentChanged, &MainWindow::assetsRootChanged,
                              &MainWindow::viewportsChanged, &MainWindow::entityDefinitionsChanged})
        QObject::connect(&window, signal, this, [this] { schedule(); });
    QObject::connect(&window, &MainWindow::documentEdited, this, [this] { schedule(); });
    refreshNow();
}

void StatusFields::schedule() { timer_->start(); }

void StatusFields::set(QLabel* label, const QString& text) {
    if (label->text() == text) return;
    label->setText(text);
    ++updates_;
}

QString StatusFields::toolText() const { return tool_->text(); }
QString StatusFields::gridText() const { return grid_->text(); }
QString StatusFields::selectionText() const { return selection_->text(); }
QString StatusFields::mapText() const { return map_->text(); }
QString StatusFields::folderText() const { return folder_->text(); }

void StatusFields::refreshNow() {
    timer_->stop();
    set(tool_, win_.tool() == MainWindow::Tool::Box ? QStringLiteral("Tool: Box") : QStringLiteral("Tool: Select"));
    if (render::ViewportWindow* vp = win_.activeViewport()) set(grid_, qs(tools::gridText(vp->rig().settings())));
    else set(grid_, QString());
    set(selection_, qs(tools::selectionText(win_.selection(), win_.document().get())));
    set(map_, qs(tools::mapText(win_.document().get(), win_.isDirty())));
    const QString folder = qs(tools::folderText(win_.assetsRoot()));
    if (folder_->text() != folder) {
        folder_->setText(folder);
        ++updates_;
    }
}

} // namespace native::ui
