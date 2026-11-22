// ui/status_bar -- see status_bar.h.
#include "ui/status_bar.h"

#include <cmath>

#include <QActionGroup>
#include <QCursor>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
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
    hint_ = new QLabel(bar);   // permanent, so a message of the line on the left (a running transform) does not hide it
    hint_->setObjectName(QStringLiteral("status:hint"));
    hint_->setMargin(2);
    hint_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // long hints are cut instead of widening the window
    hint_->setTextInteractionFlags(Qt::NoTextInteraction);
    bar->addPermanentWidget(hint_, 1);
    tool_ = field(bar, "status:tool");
    grid_ = field(bar, "status:grid");
    grid_->setToolTip(QStringLiteral("Click for the Snapping menu (snap, grid size, rotation, scale)."));
    grid_->setCursor(Qt::PointingHandCursor);
    grid_->installEventFilter(this);
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
                              &MainWindow::viewportsChanged, &MainWindow::entityDefinitionsChanged, &MainWindow::keymapChanged, &MainWindow::transformChanged})
        QObject::connect(&window, signal, this, [this] { schedule(); });
    QObject::connect(&window, &MainWindow::documentEdited, this, [this] { schedule(); });
    refreshNow();
}

void StatusFields::schedule() { timer_->start(); }

bool StatusFields::eventFilter(QObject* watched, QEvent* event) {
    if (watched == grid_ && event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        const std::unique_ptr<QMenu> menu = buildSnapMenu();
        if (menu && !menu->isEmpty()) menu->exec(grid_->mapToGlobal(QPoint(0, 0)) - QPoint(0, menu->sizeHint().height()));
        return true;
    }
    return QObject::eventFilter(watched, event);
}

std::unique_ptr<QMenu> StatusFields::buildSnapMenu() const {
    render::ViewportWindow* vp = win_.activeViewport();
    if (!vp) return nullptr;
    const render::ViewportSettings s = vp->rig().settings();
    auto menu = std::make_unique<QMenu>(grid_);
    menu->setObjectName(QStringLiteral("snapMenu"));
    MainWindow* w = &win_;
    QAction* toggle = menu->addAction(QStringLiteral("Snap to grid"));
    toggle->setCheckable(true);
    toggle->setChecked(s.snapEnabled);
    QObject::connect(toggle, &QAction::triggered, menu.get(), [w] { w->runAction("snap.toggle"); });
    const auto values = [&](const char* title, const char* id, const std::vector<double>& list, double current, const QString& unit) {
        QMenu* sub = menu->addMenu(QString::fromLatin1(title));
        auto* group = new QActionGroup(sub);
        for (const double v : list) {
            QAction* a = sub->addAction(qs(tools::shortNumber(v)) + unit);
            a->setCheckable(true);
            a->setChecked(std::abs(v - current) < 1e-9);
            group->addAction(a);
            QObject::connect(a, &QAction::triggered, menu.get(), [w, id, v] { w->runAction(id, nlohmann::json{{"value", v}}); });
        }
    };
    values("Grid size", "snap.grid_size", render::gridSizes(), s.gridSize, QStringLiteral(" m"));
    values("Rotation snap", "snap.rotation", render::rotSnaps(), s.rotSnap, QString::fromUtf8("\xC2\xB0"));
    values("Scale snap", "snap.scale", render::scaleSnaps(), s.scaleSnap, QString());
    return menu;
}

void StatusFields::set(QLabel* label, const QString& text) {
    if (label->text() == text) return;
    label->setText(text);
    ++updates_;
}

QString StatusFields::hintText() const { return hint_->text(); }
QString StatusFields::toolText() const { return tool_->text(); }
QString StatusFields::gridText() const { return grid_->text(); }
QString StatusFields::selectionText() const { return selection_->text(); }
QString StatusFields::mapText() const { return map_->text(); }
QString StatusFields::folderText() const { return folder_->text(); }

void StatusFields::refreshNow() {
    timer_->stop();
    set(hint_, qs(win_.hintText()));
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
