// native_gui_tests entry point: tests that need real top-level windows and a GPU (Qt's normal "windows" platform, one
// QApplication). They open and close windows on the desktop for a moment; run them on the development machine, not in
// headless CI. Each case skips itself with a message when there is no Vulkan device.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include <QApplication>

int main(int argc, char** argv) {
    QApplication qtApp(argc, argv);
    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    return ctx.run();
}
