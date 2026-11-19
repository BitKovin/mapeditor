// native_tests entry point: doctest plus one QApplication (offscreen platform unless QT_QPA_PLATFORM is set) so UI tests can
// create real widgets without a visible window.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include <QApplication>
#include <QtGlobal>

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication qtApp(argc, argv);
    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    return ctx.run();
}
