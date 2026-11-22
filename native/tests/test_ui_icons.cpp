// M14 T14.2: the icon set: every name draws something, in the colours of the theme; unknown names give nothing.
#include <doctest.h>

#include "ui/icons.h"

#include <QApplication>
#include <QImage>

using namespace native;

namespace {

bool hasInk(const QPixmap& pm) {
    const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(img.pixel(x, y)) > 40) return true;
    return false;
}

} // namespace

TEST_CASE("icons: every name draws ink, in every state") {
    REQUIRE(ui::iconNames().size() >= 30);
    for (const std::string& name : ui::iconNames()) {
        CAPTURE(name);
        REQUIRE(ui::hasIcon(name));
        const QIcon ic = ui::icon(name);
        REQUIRE_FALSE(ic.isNull());
        CHECK(hasInk(ic.pixmap(20, 20, QIcon::Normal, QIcon::Off)));
        CHECK(hasInk(ic.pixmap(20, 20, QIcon::Active, QIcon::Off)));
        CHECK(hasInk(ic.pixmap(20, 20, QIcon::Normal, QIcon::On)));
        CHECK(hasInk(ic.pixmap(20, 20, QIcon::Disabled, QIcon::Off)));
    }
}

TEST_CASE("icons: unknown names give a null icon, the toolbar's names all exist") {
    CHECK_FALSE(ui::hasIcon("nope"));
    CHECK(ui::icon("nope").isNull());
    CHECK(ui::iconSvg("nope", Qt::white).isEmpty());
    for (const char* n : {"save", "undo", "redo", "select", "box", "clipper", "decal", "measure", "object", "mesh", "vertex", "edge", "face", "move", "rotate", "scale", "grid", "snap",
                          "uvlock", "layout1", "layout2", "layout4", "play", "search", "maximize", "popout", "eye", "camera", "sun"})
        CHECK_MESSAGE(ui::hasIcon(n), n);
}

TEST_CASE("icons: the stroke colour comes from the theme") {
    const QString svg = ui::iconSvg("vertex", QColor("#3b8eea"));
    CHECK(svg.contains("#3b8eea"));
    CHECK_FALSE(svg.contains("currentColor"));
    // the idle and the hover colour of the dark theme differ
    const ui::Tokens t = ui::darkTokens();
    const QImage a = ui::icon("move", t).pixmap(24, 24, QIcon::Normal, QIcon::Off).toImage();
    const QImage b = ui::icon("move", t).pixmap(24, 24, QIcon::Active, QIcon::Off).toImage();
    CHECK(a != b);
}
