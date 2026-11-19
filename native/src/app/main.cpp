// app/main -- entry point of mapeditor_native.exe.
//
// Owns: QApplication, command line, the main window, the development --screenshot mode. Thread: UI thread (main).
// Spec: 01_ARCHITECTURE.md §2, 02_BUILD_AND_TOOLING.md §6.
#include <QApplication>
#include <QImage>
#include <QMessageBox>
#include <QStringList>
#include <QTimer>

#include <string>
#include <vector>

#include "app/app_options.h"
#include "ui/main_window.h"

int main(int argc, char** argv) {
    QApplication qtApp(argc, argv);

    std::vector<std::string> args;
    const QStringList qargs = QCoreApplication::arguments();
    for (int i = 1; i < qargs.size(); ++i) args.push_back(qargs[i].toStdString());
    const native::app::ParseResult parsed = native::app::parseArgs(args);
    if (!parsed.error.empty() || parsed.options.help) {
        const QString text = QString::fromStdString((parsed.error.empty() ? std::string() : parsed.error + "\n\n") +
                                                    native::app::usageText());
        QMessageBox::information(nullptr, QStringLiteral("Map Editor (native)"), text);
        return parsed.error.empty() ? 0 : 2;
    }

    native::ui::MainWindow window;
    window.show();

    int shotResult = 0;
    if (!parsed.options.screenshotPath.empty()) {
        // Visual-check mode: wait for the first paint, save the window, quit.
        QTimer::singleShot(300, &qtApp, [&] {
            const QImage img = window.grab().toImage();
            if (img.isNull() || !img.save(QString::fromStdString(parsed.options.screenshotPath))) shotResult = 3;
            QApplication::quit();
        });
    }
    const int rc = qtApp.exec();
    return shotResult != 0 ? shotResult : rc;
}
