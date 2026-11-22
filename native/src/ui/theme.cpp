// ui/theme -- see theme.h.
#include "ui/theme.h"

#include <QApplication>
#include <QFont>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace native::ui {

namespace {

struct Base {
    QString style;
    QPalette palette;
    QFont font;
};

const Base& base() {
    static std::optional<Base> b;
    if (!b) b = Base{QApplication::style() ? QApplication::style()->name() : QString(), QApplication::palette(), QApplication::font()};
    return *b;
}

Tokens& current() {
    static Tokens t = darkTokens();
    return t;
}

// "@name" in a style sheet template becomes the colour as #rrggbb (longer names first, so "@accentBg" is not read as "@accent" + "Bg").
QString fill(QString text, const Tokens& t) {
    const std::vector<std::pair<const char*, QColor>> names = {
        {"@lineHover", t.lineHover}, {"@textStrong", t.textStrong}, {"@accentBg", t.accentBg}, {"@line2", t.line2}, {"@accent", t.accent}, {"@line", t.line},
        {"@text", t.text}, {"@dim", t.dim}, {"@bg0", t.bg0}, {"@bg1", t.bg1}, {"@bg2", t.bg2}, {"@bg3", t.bg3}, {"@bg4", t.bg4},
        {"@ok", t.ok}, {"@warn", t.warn}, {"@bad", t.bad}};
    for (const auto& [name, color] : names) text.replace(QLatin1String(name), color.name());
    return text;
}

} // namespace

Tokens darkTokens() {
    Tokens t;
    t.bg0 = QColor(0x14, 0x14, 0x16);
    t.bg1 = QColor(0x1c, 0x1d, 0x20);
    t.bg2 = QColor(0x24, 0x25, 0x2a);
    t.bg3 = QColor(0x2e, 0x30, 0x36);
    t.bg4 = QColor(0x3a, 0x3d, 0x44);
    t.line = QColor(0x0c, 0x0c, 0x0e);
    t.line2 = QColor(0x39, 0x3c, 0x43);
    t.lineHover = QColor(0x4a, 0x4e, 0x57);
    t.text = QColor(0xd2, 0xd4, 0xd8);
    t.textStrong = QColor(0xf2, 0xf3, 0xf5);
    t.dim = QColor(0x86, 0x8a, 0x93);
    t.accent = QColor(0x3b, 0x8e, 0xea);
    t.accentBg = QColor(0x1d, 0x4f, 0x86);
    t.ok = QColor(0x4e, 0xc4, 0x6a);
    t.warn = QColor(0xf0, 0xa0, 0x30);
    t.bad = QColor(0xe5, 0x64, 0x5e);
    return t;
}

Tokens lightTokens() {
    Tokens t;
    t.bg0 = QColor(0xdf, 0xe0, 0xe4);
    t.bg1 = QColor(0xef, 0xef, 0xef);
    t.bg2 = QColor(0xe6, 0xe7, 0xea);
    t.bg3 = QColor(0xd9, 0xda, 0xde);
    t.bg4 = QColor(0xc9, 0xcb, 0xd1);
    t.line = QColor(0xa9, 0xac, 0xb4);
    t.line2 = QColor(0xc2, 0xc5, 0xcc);
    t.lineHover = QColor(0x8f, 0x93, 0x9c);
    t.text = QColor(0x20, 0x22, 0x26);
    t.textStrong = QColor(0x00, 0x00, 0x00);
    t.dim = QColor(0x68, 0x6c, 0x75);
    t.accent = QColor(0x2a, 0x74, 0xd0);
    t.accentBg = QColor(0xb7, 0xd3, 0xf4);
    t.ok = QColor(0x1f, 0x8f, 0x3c);
    t.warn = QColor(0xb8, 0x6e, 0x00);
    t.bad = QColor(0xc4, 0x3a, 0x34);
    return t;
}

const Tokens& tokens() { return current(); }

Appearance sanitize(const Appearance& a) {
    Appearance out = a;
    if (out.theme != "light" && out.theme != "system") out.theme = "dark";
    out.scalePercent = std::clamp(out.scalePercent, kScaleMin, kScaleMax);
    return out;
}

Appearance readAppearance(QSettings* settings) {
    Appearance a;
    if (!settings) return a;
    a.theme = settings->value(QStringLiteral("ui/theme"), QStringLiteral("dark")).toString().toStdString();
    a.scalePercent = settings->value(QStringLiteral("ui/scale"), 100).toInt();
    return sanitize(a);
}

void writeAppearance(QSettings* settings, const Appearance& a) {
    if (!settings) return;
    const Appearance s = sanitize(a);
    settings->setValue(QStringLiteral("ui/theme"), QString::fromStdString(s.theme));
    settings->setValue(QStringLiteral("ui/scale"), s.scalePercent);
}

QPalette darkPalette() {
    const Tokens t = darkTokens();
    QPalette p;
    p.setColor(QPalette::Window, t.bg1);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.bg0);
    p.setColor(QPalette::AlternateBase, t.bg1);
    p.setColor(QPalette::ToolTipBase, t.bg2);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Button, t.bg3);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::BrightText, QColor(255, 80, 80));
    p.setColor(QPalette::Link, t.accent);
    p.setColor(QPalette::Highlight, t.accentBg);
    p.setColor(QPalette::HighlightedText, t.textStrong);
    p.setColor(QPalette::PlaceholderText, t.dim);
    p.setColor(QPalette::Light, t.lineHover);
    p.setColor(QPalette::Midlight, t.bg4);
    p.setColor(QPalette::Mid, t.bg2);
    p.setColor(QPalette::Dark, t.line);
    p.setColor(QPalette::Shadow, Qt::black);
    for (const QPalette::ColorRole r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) p.setColor(QPalette::Disabled, r, t.dim);
    return p;
}

QString dockStyleSheet(const Tokens& t) {
    // The docking system (Qt Advanced Docking System, native/third_party/qtads) with its own style sheet switched off: flat panels, a dark 3 px gap as the splitter,
    // a quiet caption bar, the active tab marked by a 2 px accent line on top (the Shell's rule, 14_UI_REDESIGN.md section 9.2).
    return fill(QStringLiteral(R"(
ads--CDockContainerWidget { background: @bg0; }
ads--CDockContainerWidget > QSplitter { padding: 0; }
ads--CDockSplitter { qproperty-handleWidth: 3; }
ads--CDockContainerWidget ads--CDockSplitter::handle { background: @line; }
ads--CDockAreaWidget { background: @bg1; border: none; }
ads--CDockAreaTitleBar { background: @bg0; border-bottom: 1px solid @line; }
ads--CTitleBarButton { padding: 0; }
ads--CDockWidgetTab { background: @bg0; border: none; border-top: 2px solid transparent; padding: 0 2px; }
ads--CDockWidgetTab:hover { background: @bg2; }
ads--CDockWidgetTab QLabel { color: @dim; }
ads--CDockWidgetTab:hover QLabel { color: @text; }
ads--CDockWidgetTab[activeTab="true"] { background: @bg1; border-top: 2px solid @accent; }
ads--CDockWidgetTab[activeTab="true"] QLabel { color: @textStrong; }
ads--CDockWidget { background: @bg1; border: none; }
QScrollArea#dockWidgetScrollArea { padding: 0; border: none; }
ads--CAutoHideTab { background: none; border: none; padding: 2px 8px; min-height: 20px; color: @dim; }
ads--CAutoHideTab:hover { color: @textStrong; }
ads--CAutoHideTab[activeTab="true"] { color: @textStrong; }
ads--CAutoHideTab[sideBarLocation="3"] { border-top: 2px solid transparent; }
ads--CAutoHideTab[sideBarLocation="3"][activeTab="true"], ads--CAutoHideTab[sideBarLocation="3"]:hover { border-top: 2px solid @accent; }
ads--CAutoHideSideBar { background: @bg0; border: none; qproperty-spacing: 4; }
ads--CAutoHideSideBar[sideBarLocation="3"] { border-top: 1px solid @line; }
#sideTabsContainerWidget { background: transparent; }
ads--CAutoHideDockContainer { background: @bg1; }
ads--CAutoHideDockContainer ads--CDockAreaTitleBar { background: @bg2; padding: 0; border: none; }
ads--CAutoHideDockContainer #autoHideTitleLabel { padding-left: 4px; color: @text; }
ads--CResizeHandle { background: @line; }
)"), t);
}

QString appStyleSheet(const Tokens& t, bool full) {
    QString out = dockStyleSheet(t);
    if (!full) return out;
    out += fill(QStringLiteral(R"(
QToolTip { background: @bg2; color: @text; border: 1px solid @line2; padding: 3px 5px; }
QMenuBar { background: @bg0; color: @text; border-bottom: 1px solid @line; }
QMenuBar::item { padding: 4px 10px; background: transparent; }
QMenuBar::item:selected, QMenuBar::item:pressed { background: @bg3; color: @textStrong; }
QMenu { background: @bg2; border: 1px solid @line2; }
QStatusBar { background: @bg0; color: @dim; border-top: 1px solid @line; }
QStatusBar::item { border: none; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:vertical { background: @bg4; border-radius: 4px; min-height: 24px; margin: 1px; }
QScrollBar::handle:horizontal { background: @bg4; border-radius: 4px; min-width: 24px; margin: 1px; }
QScrollBar::handle:hover { background: @lineHover; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; background: none; border: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QPushButton { background: @bg3; color: @text; border: 1px solid @line; border-radius: 3px; padding: 3px 10px; min-height: 18px; }
QPushButton:hover { background: @bg4; border-color: @lineHover; }
QPushButton:pressed { background: @bg2; }
QPushButton[compact="true"] { padding: 3px 4px; }
QPushButton:checked { background: @accentBg; border-color: @accent; color: @textStrong; }
QPushButton:disabled { color: @dim; background: @bg2; }
QLineEdit { background: @bg0; color: @text; border: 1px solid @line2; border-radius: 3px; padding: 2px 5px; selection-background-color: @accentBg; selection-color: @textStrong; }
QLineEdit:hover { border-color: @lineHover; }
QLineEdit:focus { border-color: @accent; }
QLineEdit:disabled { color: @dim; }
QToolBar { background: @bg2; border: none; border-bottom: 1px solid @line; spacing: 2px; padding: 2px 6px; }
QToolBar::separator { background: @line2; width: 1px; margin: 4px 5px; }
QToolButton { background: transparent; color: @text; border: 1px solid transparent; border-radius: 3px; padding: 3px 5px; }
QToolButton:hover { background: @bg4; }
QToolButton:pressed { background: @bg2; }
QToolButton:checked { background: @accentBg; border-color: @accent; color: @textStrong; }
QToolButton:disabled { color: @dim; }
QToolButton[card="true"] { background: @bg3; border: 1px solid @line; border-radius: 4px; padding: 5px 4px; }
QToolButton[card="true"]:hover { background: @bg4; border-color: @lineHover; }
QToolButton[card="true"]:checked { background: @accentBg; border-color: @accent; }
QToolButton::menu-indicator { image: none; width: 0; }
#viewportHeader { background: @bg1; border-top: 2px solid transparent; border-bottom: 1px solid @line; }
#viewportHeader[active="true"] { border-top: 2px solid @accent; }
#viewportHeader QToolButton { color: @dim; padding: 2px 7px; }
#viewportHeader QToolButton:hover { color: @textStrong; background: @bg3; }
#viewportHeader QToolButton:checked { color: @textStrong; background: @bg3; border-color: transparent; }
QTabBar::tab { background: @bg0; color: @dim; padding: 5px 12px; border: none; border-top: 2px solid transparent; }
QTabBar::tab:hover { background: @bg2; color: @text; }
QTabBar::tab:selected { background: @bg1; color: @textStrong; border-top: 2px solid @accent; }
QTabBar QToolButton { border-radius: 0; }
QTabWidget::pane { border: none; border-top: 1px solid @line; background: @bg1; }
QTabWidget > QTabBar::tab { padding: 5px 9px; }
)"), t);
    return out;
}

void applyAppearance(const Appearance& wanted) {
    if (!qApp) return;
    const Appearance a = sanitize(wanted);
    const Base& b = base();   // before anything is changed
    if (a.theme == "system") {
        if (!b.style.isEmpty() && QApplication::style() && QApplication::style()->name().compare(b.style, Qt::CaseInsensitive) != 0) QApplication::setStyle(QStyleFactory::create(b.style));
        QApplication::setPalette(b.palette);
        current() = b.palette.color(QPalette::Window).lightness() < 128 ? darkTokens() : lightTokens();
        qApp->setStyleSheet(appStyleSheet(current(), false));
    } else {
        if (!QApplication::style() || QApplication::style()->name().compare(QStringLiteral("fusion"), Qt::CaseInsensitive) != 0) QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        QApplication::setPalette(a.theme == "dark" ? darkPalette() : QApplication::style()->standardPalette());
        current() = a.theme == "dark" ? darkTokens() : lightTokens();
        qApp->setStyleSheet(appStyleSheet(current(), true));
    }
    QFont f = b.font;
    if (f.pointSizeF() > 0) f.setPointSizeF(f.pointSizeF() * a.scalePercent / 100.0);
    else if (f.pixelSize() > 0) f.setPixelSize(std::max(1, f.pixelSize() * a.scalePercent / 100));
    QApplication::setFont(f);
}

} // namespace native::ui
