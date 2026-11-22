// Visual audit helper (not an assertion test): with NATIVE_SHOT_DIR set it opens the main window on a real map (NATIVE_AUDIT_ROOT + NATIVE_AUDIT_MAP, default the demo) and
// saves a PNG of every dock tab and of every dialog / editor window, with the selection kinds that change what they show. Without NATIVE_SHOT_DIR it does nothing.
#include <doctest.h>

#include <cstdio>
#include <functional>

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QTimer>
#include <QWidget>

#include <DockAreaWidget.h>
#include <DockWidget.h>

#include "base/jobs.h"
#include "doc/selection.h"
#include "gui_support.h"
#include "ui/hide_dialog.h"
#include "ui/main_window.h"
#include "ui/texgroup_editor.h"
#include "ui/texture_props_window.h"
#include "ui/theme.h"
#include "ui/tileset_editor.h"

using namespace native;
using namespace guitest;

namespace {

QString shotDir() { return qEnvironmentVariable("NATIVE_SHOT_DIR"); }

void shoot(QWidget* w, const QString& name) {
    if (!w) return;
    pumpFor(150);
    const QImage img = w->grab().toImage();
    img.save(shotDir() + QLatin1Char('/') + name + QStringLiteral(".png"));
}

void shootTopLevels(const QString& prefix, QWidget* except) {
    int n = 0;
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w != except && w->isVisible() && !w->windowTitle().isEmpty()) shoot(w, prefix + QLatin1Char('_') + QString::number(n++));
}

void closeTopLevels(QWidget* except) {
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w != except && w->isVisible() && qobject_cast<QDialog*>(w)) w->close();
}

} // namespace

TEST_CASE("audit: a picture of every panel and window" * doctest::test_suite("audit")) {
    const auto step = [](const char* n) { std::fprintf(stderr, "[audit] %s\n", n); std::fflush(stderr); };
    if (shotDir().isEmpty()) return;
    App app;
    if (!app.ok) return;
    struct Theme {
        Theme() { ui::applyAppearance({"dark", 100}); }
        ~Theme() { ui::applyAppearance({"system", 100}); }
    } theme;   // the look of the real editor (the test harness itself runs with the platform style)
    base::JobPool pool(4);
    ui::Services sv = app.services();
    sv.jobs = &pool;
    sv.assetsRoot = qEnvironmentVariable("NATIVE_AUDIT_ROOT", QStringLiteral("F:/Git/mapeditor/core/assets")).toStdString();
    const std::string map = qEnvironmentVariable("NATIVE_AUDIT_MAP", QStringLiteral("maps/demo_lit.map.json")).toStdString();
    ui::MainWindow window(sv);
    window.resize(1600, 950);
    window.show();
    REQUIRE(waitUntil([&] { return allReady(window); }));
    window.openDocument(map);
    REQUIRE(waitUntil([&] { return window.sceneReady(); }, 30000));
    window.frameScene();
    pumpFor(1500);
    QDir().mkpath(shotDir());
    shoot(&window, "main_idle");

    const auto docks = [&](const QString& tag) {
        struct D { ads::CDockWidget* d; const char* n; };
        const D list[] = {{window.outlinerDock(), "outliner"}, {window.detailsDock(), "details"}, {window.surfaceDock(), "surface"}, {window.levelDock(), "level"},
                          {window.historyDock(), "history"}, {window.messagesDock(), "messages"}, {window.adjustDock(), "adjust"}, {window.lightingDock(), "lighting"},
                          {window.toolOptionsDock(), "tool"}, {window.statsDock(), "stats"}, {window.placeDock(), "place"}, {window.texturesDock(), "textures"},
                          {window.groupsDock(), "groups"}};
        for (const D& x : list) {
            if (!x.d) continue;
            x.d->toggleView(true);
            x.d->setAsCurrentTab();
            pumpFor(200);
            shoot(x.d->widget(), tag + QLatin1Char('_') + QString::fromLatin1(x.n));
        }
        shoot(&window, tag + QStringLiteral("_window"));
    };
    docks(QStringLiteral("nosel"));

    // a brush, a face, an entity
    const auto doc = window.document();
    REQUIRE(doc);
    std::string brushEntity, brushId, pointEntity;
    for (const auto& e : doc->entities) {
        if (brushId.empty() && !e->brushes.empty()) { brushEntity = e->typed.id; brushId = e->brushes[0]->brush.id; }
        if (pointEntity.empty() && e->brushes.empty() && e->typed.classname != "worldspawn") pointEntity = e->typed.id;
    }
    window.selection().select(doc::SelRef::brush(brushEntity, brushId));
    pumpFor(300);
    docks(QStringLiteral("brush"));
    window.selection().select(doc::SelRef::face(brushEntity, brushId, 0));
    pumpFor(300);
    docks(QStringLiteral("face"));
    if (!pointEntity.empty()) {
        window.selection().select(doc::SelRef::entity(pointEntity));
        pumpFor(300);
        docks(QStringLiteral("entity"));
    }

    // dialogs and editor windows
    // the modal dialogs block in exec(): a timer takes the picture and closes them
    const auto modal = [&](const char* name, const QString& prefix, const std::function<void()>& open) {
        step(name);
        QTimer::singleShot(500, [&window, prefix] {
            shootTopLevels(prefix, &window);
            closeTopLevels(&window);
        });
        open();
    };
    for (int page = 0; page < 5; ++page) modal("settings", QStringLiteral("settings%1").arg(page), [&] { window.openSettings(page); });
    modal("palette", QStringLiteral("palette"), [&] { window.openPalette(); });
    step("openHideDialog");
    window.openHideDialog(true);
    pumpFor(300);
    shootTopLevels(QStringLiteral("hide"), &window);
    closeTopLevels(&window);
    std::string tex = "dev/grey";
    if (doc && !brushId.empty())
        for (const auto& e : doc->entities)
            if (!e->brushes.empty() && !e->brushes[0]->brush.faces.empty()) { tex = e->brushes[0]->brush.faces[0].texture; break; }
    step("openTextureProperties");
    window.openTextureProperties(tex, true);
    pumpFor(400);
    shootTopLevels(QStringLiteral("texprops"), &window);
    closeTopLevels(&window);
    step("openTexGroupEditor");
    window.openTexGroupEditor("", true);
    pumpFor(400);
    shootTopLevels(QStringLiteral("texgroup"), &window);
    closeTopLevels(&window);
    step("openTileSetEditor");
    window.openTileSetEditor("", tools::tex::KitKind::Wall, true);
    pumpFor(400);
    shootTopLevels(QStringLiteral("tileset"), &window);
    closeTopLevels(&window);
    step("openReliefEditor");
    window.openReliefEditor(tex, true);
    pumpFor(400);
    shootTopLevels(QStringLiteral("relief"), &window);
    closeTopLevels(&window);
    step("openFastTextureTool");
    window.openFastTextureTool();
    pumpFor(400);
    shootTopLevels(QStringLiteral("ftt"), &window);
    closeTopLevels(&window);
    // the context menu
    step("if ");
    if (render::ViewportWindow* vp = window.activeViewport()) {
        QTimer::singleShot(500, [] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (w->isVisible() && w->inherits("QMenu")) {
                    shoot(w, QStringLiteral("contextmenu"));
                    w->close();
                }
        });
        window.openContextMenu(vp, QPointF(vp->width() / 2.0, vp->height() / 2.0));
        QApplication::closeAllWindows();
    }
}
