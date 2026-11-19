// ui/main_window -- the application's top-level window.
//
// Owns: the QMainWindow shell (title, geometry persistence arrives with T1.4, dock manager with T1.4, menus built from the
// action registry with T1.6). For now it is empty on purpose (T0.4).
// Thread: UI thread only.
// Spec: 07_EDITOR_PORT.md group C (app/main_window), 10_UI_AND_INPUT.md.
#pragma once

#include <QMainWindow>

namespace native::ui {

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);

    static const char* windowTitle();
};

} // namespace native::ui
