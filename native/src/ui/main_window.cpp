// ui/main_window -- see main_window.h.
#include "ui/main_window.h"

namespace native::ui {

const char* MainWindow::windowTitle() { return "Map Editor (native)"; }

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QString::fromLatin1(windowTitle()));
    resize(1280, 800);
}

} // namespace native::ui
