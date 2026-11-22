// ui/theme -- see theme.h.
#include "ui/theme.h"

#include <QApplication>
#include <QFont>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>

#include <algorithm>
#include <optional>

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

} // namespace

Appearance sanitize(const Appearance& a) {
    Appearance out = a;
    if (out.theme != "dark" && out.theme != "light") out.theme = "system";
    out.scalePercent = std::clamp(out.scalePercent, kScaleMin, kScaleMax);
    return out;
}

Appearance readAppearance(QSettings* settings) {
    Appearance a;
    if (!settings) return a;
    a.theme = settings->value(QStringLiteral("ui/theme"), QStringLiteral("system")).toString().toStdString();
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
    QPalette p;
    const QColor window(45, 45, 48), base(30, 30, 32), text(228, 228, 228), button(58, 58, 62), highlight(42, 110, 190), disabled(128, 128, 128);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, QColor(40, 40, 43));
    p.setColor(QPalette::ToolTipBase, QColor(60, 60, 64));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, QColor(255, 80, 80));
    p.setColor(QPalette::Link, QColor(90, 160, 240));
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, QColor(140, 140, 140));
    p.setColor(QPalette::Light, QColor(80, 80, 85));
    p.setColor(QPalette::Midlight, QColor(68, 68, 72));
    p.setColor(QPalette::Mid, QColor(50, 50, 54));
    p.setColor(QPalette::Dark, QColor(25, 25, 27));
    p.setColor(QPalette::Shadow, Qt::black);
    for (const QPalette::ColorRole r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) p.setColor(QPalette::Disabled, r, disabled);
    return p;
}

void applyAppearance(const Appearance& wanted) {
    if (!qApp) return;
    const Appearance a = sanitize(wanted);
    const Base& b = base();   // before anything is changed
    if (a.theme == "system") {
        if (!b.style.isEmpty() && QApplication::style() && QApplication::style()->name().compare(b.style, Qt::CaseInsensitive) != 0) QApplication::setStyle(QStyleFactory::create(b.style));
        QApplication::setPalette(b.palette);
    } else {
        if (!QApplication::style() || QApplication::style()->name().compare(QStringLiteral("fusion"), Qt::CaseInsensitive) != 0) QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        QApplication::setPalette(a.theme == "dark" ? darkPalette() : QApplication::style()->standardPalette());
    }
    QFont f = b.font;
    if (f.pointSizeF() > 0) f.setPointSizeF(f.pointSizeF() * a.scalePercent / 100.0);
    else if (f.pixelSize() > 0) f.setPixelSize(std::max(1, f.pixelSize() * a.scalePercent / 100));
    QApplication::setFont(f);
}

} // namespace native::ui
