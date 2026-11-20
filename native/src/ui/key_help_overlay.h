// ui/key_help_overlay -- the key help sheet: a translucent panel over the active viewport that lists the bindings of the contexts that are active now (the most specific
// first), so a user who does not know what works can hold F1 and look. It is a top-level window of its own (the viewports are native windows: a child widget cannot draw
// over them) that takes no input, so the keys and the mouse go on working under it. The content is made again at every show and when the keymap changes.
// Thread: UI thread only.
// Spec: 10_UI_AND_INPUT.md §6, 08_MILESTONES.md T4.7.
#pragma once

#include <QWidget>

#include <vector>

#include "app/actions/palette.h"

class QLabel;

namespace native::ui {

class KeyHelpOverlay : public QWidget {
    Q_OBJECT
public:
    explicit KeyHelpOverlay(QWidget* parent = nullptr);

    // Sets the sections and the text; the overlay is moved over `area` (global coordinates) when shown.
    void setSections(const std::vector<actions::HelpSection>& sections);
    void showOver(const QRect& globalArea);
    // The plain text of the sheet (tests): one line per binding, "<keys>  <label>", with the section titles.
    QString plainText() const { return plain_; }
    int sectionCount() const { return sections_; }

    // "tool:box" -> "Box tool"
    static QString titleOf(const std::string& context);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    std::vector<QLabel*> columns_;
    QString plain_;
    int sections_ = 0;
};

} // namespace native::ui
