// Skeleton checks (T0.4): every module library links, the Core is usable from native code, and the main window builds.
#include <doctest.h>

#include <QImage>

#include <string>

#include "base/module_info.h"
#include "doc/module_info.h"
#include "gpu/module_info.h"
#include "light/module_info.h"
#include "render/module_info.h"
#include "scene/module_info.h"
#include "tools/module_info.h"
#include "ui/main_window.h"
#include "ui/module_info.h"

#include <core/math3.h>

TEST_CASE("every module library links and names itself") {
    CHECK(std::string(native::base::moduleName()) == "base");
    CHECK(std::string(native::doc::moduleName()) == "doc");
    CHECK(std::string(native::scene::moduleName()) == "scene");
    CHECK(std::string(native::gpu::moduleName()) == "gpu");
    CHECK(std::string(native::light::moduleName()) == "light");
    CHECK(std::string(native::render::moduleName()) == "render");
    CHECK(std::string(native::tools::moduleName()) == "tools");
    CHECK(std::string(native::ui::moduleName()) == "ui");
}

TEST_CASE("the Core is linked into the same process") {
    const core::Vec3 a(1, 2, 3), b(4, 5, 6);
    CHECK(a.dot(b) == doctest::Approx(32.0));
}

TEST_CASE("the main window has its title and paints a non-empty image") {
    native::ui::MainWindow w;
    CHECK(w.windowTitle() == QString::fromLatin1("Map Editor (native)"));
    w.show();
    const QImage img = w.grab().toImage();
    REQUIRE(!img.isNull());
    CHECK(img.width() >= 640);
    CHECK(img.height() >= 400);
}
