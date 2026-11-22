// ui/key_help_overlay -- see key_help_overlay.h.
#include "ui/key_help_overlay.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>

#include <algorithm>

namespace native::ui {

namespace {
QString qs(const std::string& s) { return QString::fromUtf8(s.c_str()); }
} // namespace

KeyHelpOverlay::KeyHelpOverlay(QWidget* parent) : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus) {
    setObjectName(QStringLiteral("keyhelp"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(28);
}

QString KeyHelpOverlay::titleOf(const std::string& context) {
    static const std::pair<const char*, const char*> names[] = {
        {"global", "Everywhere"},       {"window:main", "Main window"},   {"area:viewport", "Viewport"},   {"view:perspective", "Perspective view"},
        {"view:fly", "Fly camera"},     {"view:orbit", "Orbit camera"},   {"view:ortho", "Orthographic view"}, {"tool:select", "Select tool"},
        {"tool:box", "Box tool"},       {"tool:decal", "Decal tool"},       {"modal:transform", "While moving / rotating / scaling"}, {"hold:camera.fly", "While flying"},
    };
    for (const auto& n : names)
        if (context == n.first) return QString::fromLatin1(n.second);
    return qs(context);
}

void KeyHelpOverlay::setSections(const std::vector<actions::HelpSection>& sections) {
    // sections are not split: they fill the columns in order, a new column starts when the current one has its share of the lines
    size_t total = 0;
    for (const actions::HelpSection& s : sections) total += s.lines.size() + 2;
    const size_t columns = total > 36 ? 3 : (total > 18 ? 2 : 1);
    const size_t share = (total + columns - 1) / columns;
    for (QLabel* l : columns_) delete l;
    columns_.clear();
    plain_.clear();
    sections_ = static_cast<int>(sections.size());
    QString html;
    size_t used = 0;
    const auto flush = [&] {
        if (html.isEmpty()) return;
        auto* label = new QLabel(QStringLiteral("<table cellspacing='0' cellpadding='1'>") + html + QStringLiteral("</table>"), this);
        label->setObjectName(QStringLiteral("keyhelp:column"));
        label->setTextFormat(Qt::RichText);
        label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        label->setStyleSheet(QStringLiteral("color: #f2f2f2;"));
        static_cast<QHBoxLayout*>(layout())->addWidget(label, 0, Qt::AlignTop);
        columns_.push_back(label);
        html.clear();
        used = 0;
    };
    for (const actions::HelpSection& s : sections) {
        if (used >= share && columns_.size() + 1 < columns) flush();
        const QString title = titleOf(s.context);
        html += QStringLiteral("<tr><td colspan='2'><b style='color:#ffd36b'>%1</b></td></tr>").arg(title.toHtmlEscaped());
        plain_ += title + QLatin1Char('\n');
        for (const actions::HelpLine& l : s.lines) {
            const QString keys = qs(l.input), label = qs(l.label);
            html += QStringLiteral("<tr><td style='padding-right:14px'><code>%1</code></td><td>%2</td></tr>").arg(keys.toHtmlEscaped(), label.toHtmlEscaped());
            plain_ += keys + QStringLiteral("  ") + label + QLatin1Char('\n');
        }
        html += QStringLiteral("<tr><td colspan='2'>&nbsp;</td></tr>");
        used += s.lines.size() + 2;
    }
    flush();
    for (QLabel* l : columns_) l->show();
    adjustSize();
}

void KeyHelpOverlay::showOver(const QRect& area) {
    adjustSize();
    QSize size = sizeHint();
    size.setWidth(std::min(size.width(), std::max(120, area.width() - 20)));
    size.setHeight(std::min(size.height(), std::max(80, area.height() - 20)));
    resize(size);
    move(area.center() - QPoint(size.width() / 2, size.height() / 2));
    show();
    raise();
}

void KeyHelpOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 60), 1));
    p.setBrush(QColor(18, 20, 24, 215));
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
}

} // namespace native::ui
