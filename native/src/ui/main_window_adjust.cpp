// ui/main_window_adjust -- MainWindow: the last operation that can be adjusted (10_UI_AND_INPUT.md U1).
#include <QStatusBar>

#include "doc/commands.h"
#include "ui/main_window.h"

namespace native::ui {

const LastOperation* MainWindow::lastOperation() const {
    if (!lastOp_ || !document_ || lastOp_->after != document_) return nullptr;
    return &*lastOp_;
}

void MainWindow::setLastOperation(LastOperation op) {
    op.after = document_;
    lastOp_ = std::move(op);
    emit lastOperationChanged();
}

bool MainWindow::adjustLastOperation(const std::vector<double>& values) {
    const LastOperation* cur = lastOperation();
    if (!cur || values.size() != cur->params.size() || !history_ || !history_->canUndo()) return false;
    const LastOperation op = *cur;   // the rerun records a new one
    const std::string label = op.label;
    history_->undo();
    selection_->selectMany(*document_, op.before);
    if (op.rerun(values)) return true;
    // refused: the first result comes back
    history_->redo();
    lastOp_ = op;
    lastOp_->after = document_;
    emit lastOperationChanged();
    statusBar()->showMessage(QString::fromStdString(label) + QStringLiteral(": these values do not work"), 4000);
    return false;
}

} // namespace native::ui
