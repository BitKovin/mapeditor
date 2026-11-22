// ui/icons -- see icons.h.
#include "ui/icons.h"

#include <QByteArray>
#include <QImage>
#include <QMap>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

namespace native::ui {

namespace {

struct Def {
    const char* name;
    const char* body;   // the inside of the <svg>: stroke drawing on a 24 x 24 grid
};

// Simple geometry in the Lucide style: 2 px stroke, round caps and joins, no fill (a few shapes use fill="currentColor", replaced by the stroke colour).
const Def kDefs[] = {
    {"save", R"(<path d="M15.2 3a2 2 0 0 1 1.4.6l3.8 3.8a2 2 0 0 1 .6 1.4V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z"/><path d="M17 21v-7a1 1 0 0 0-1-1H8a1 1 0 0 0-1 1v7"/><path d="M7 3v4a1 1 0 0 0 1 1h7"/>)"},
    {"undo", R"(<path d="M9 14 4 9l5-5"/><path d="M4 9h10.5a5.5 5.5 0 0 1 5.5 5.5a5.5 5.5 0 0 1-5.5 5.5H11"/>)"},
    {"redo", R"(<path d="m15 14 5-5-5-5"/><path d="M20 9H9.5A5.5 5.5 0 0 0 4 14.5A5.5 5.5 0 0 0 9.5 20H13"/>)"},
    {"select", R"(<path d="M4.04 4.69a.5.5 0 0 1 .65-.65l16 6.5a.5.5 0 0 1-.06.95l-6.12 1.58a2 2 0 0 0-1.44 1.43l-1.58 6.13a.5.5 0 0 1-.95.06z"/>)"},
    {"box", R"(<path d="M21 8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z"/><path d="m3.3 7 8.7 5 8.7-5"/><path d="M12 22V12"/>)"},
    {"clipper", R"(<rect x="4" y="5" width="13" height="13" rx="1.5"/><path d="M2 22 22 2"/>)"},
    {"decal", R"(<path d="M4 4h12l4 4v12H4z"/><path d="M16 4v4h4"/><circle cx="10" cy="14" r="2.5"/>)"},
    {"measure", R"(<path d="M3 17 17 3l4 4L7 21z"/><path d="m7.5 12.5 2 2"/><path d="m10.5 9.5 2 2"/><path d="m13.5 6.5 2 2"/>)"},
    {"object", R"(<path d="M12 3 3 8v8l9 5 9-5V8z"/><path d="m3 8 9 5 9-5"/><path d="M12 13v8"/>)"},
    {"mesh", R"(<path d="M12 3 21 9l-4 11H7L3 9z"/><path d="M12 3v6M21 9l-9 0M3 9h9M12 9 7 20M12 9l5 11"/>)"},
    {"vertex", R"(<circle cx="12" cy="12" r="3.5" fill="currentColor"/><path d="M12 2v4M12 18v4M2 12h4M18 12h4"/>)"},
    {"edge", R"(<path d="M5 19 19 5"/><circle cx="5" cy="19" r="2.5" fill="currentColor"/><circle cx="19" cy="5" r="2.5" fill="currentColor"/>)"},
    {"face", R"(<path d="M12 4 21 19H3z" fill="currentColor" fill-opacity=".25"/>)"},
    {"move", R"(<path d="M12 2v20M2 12h20"/><path d="m15 19-3 3-3-3M19 9l3 3-3 3M5 9l-3 3 3 3M9 5l3-3 3 3"/>)"},
    {"rotate", R"(<path d="M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8"/><path d="M21 3v5h-5"/>)"},
    {"scale", R"(<rect x="3" y="11" width="10" height="10" rx="1.5"/><path d="M14 10 21 3"/><path d="M15 3h6v6"/>)"},
    {"grid", R"(<rect x="3" y="3" width="18" height="18" rx="2"/><path d="M3 9h18M3 15h18M9 3v18M15 3v18"/>)"},
    {"snap", R"(<path d="M6 15V4M18 15V4"/><path d="M6 15a6 6 0 0 0 12 0"/><path d="M6 8h4M14 8h4"/>)"},
    {"uvlock", R"(<rect x="3" y="11" width="18" height="11" rx="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/>)"},
    {"layout1", R"(<rect x="3" y="4" width="18" height="16" rx="2"/>)"},
    {"layout2", R"(<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M12 4v16"/>)"},
    {"layout4", R"(<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M12 4v16M3 12h18"/>)"},
    {"play", R"(<path d="M6 3 20 12 6 21z"/>)"},
    {"search", R"(<circle cx="11" cy="11" r="7"/><path d="m21 21-4.3-4.3"/>)"},
    {"maximize", R"(<path d="M15 3h6v6M9 21H3v-6M21 3l-7 7M3 21l7-7"/>)"},
    {"popout", R"(<path d="M15 3h6v6"/><path d="M10 14 21 3"/><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/>)"},
    {"eye", R"(<path d="M2 12s3-7 10-7 10 7 10 7-3 7-10 7-10-7-10-7z"/><circle cx="12" cy="12" r="3"/>)"},
    {"plus", R"(<path d="M5 12h14M12 5v14"/>)"},
    {"chevron-down", R"(<path d="m6 9 6 6 6-6"/>)"},
    {"camera", R"(<path d="M3 8a2 2 0 0 1 2-2h2l2-2h6l2 2h2a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><circle cx="12" cy="13" r="3.5"/>)"},
    {"sun", R"(<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>)"},
    {"layers", R"(<path d="m12 3 9 5-9 5-9-5z"/><path d="m3 13 9 5 9-5"/>)"},
    {"trash", R"(<path d="M3 6h18"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/><path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/>)"},
    {"x", R"(<path d="M18 6 6 18M6 6l12 12"/>)"},
    {"star", R"(<path d="m12 2 3.1 6.3 6.9 1-5 4.9 1.2 6.8-6.2-3.3-6.2 3.3L7 14.2 2 9.3l6.9-1z"/>)"},
    {"cylinder", R"(<ellipse cx="12" cy="5" rx="8" ry="3"/><path d="M4 5v14c0 1.7 3.6 3 8 3s8-1.3 8-3V5"/>)"},
    {"wedge", R"(<path d="M3 19h18V6z"/><path d="M3 19 8 15"/>)"},
    {"arch", R"(<path d="M4 20V11a8 8 0 0 1 16 0v9"/><path d="M9 20v-8a3 3 0 0 1 6 0v8"/>)"},
    {"stairs", R"(<path d="M3 20v-4h4v-4h4V8h4V4h6v16z"/>)"},
    {"sphere", R"(<circle cx="12" cy="12" r="9"/><ellipse cx="12" cy="12" rx="9" ry="3.5"/><ellipse cx="12" cy="12" rx="3.5" ry="9"/>)"},
    {"folder", R"(<path d="M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.7-.9l-.8-1.2A2 2 0 0 0 7.9 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2z"/>)"},
};

const QMap<QString, const char*>& table() {
    static const QMap<QString, const char*> t = [] {
        QMap<QString, const char*> m;
        for (const Def& d : kDefs) m.insert(QString::fromLatin1(d.name), d.body);
        return m;
    }();
    return t;
}

QPixmap render(const std::string& name, const QColor& color, int px) {
    const QString svg = iconSvg(name, color);
    if (svg.isEmpty()) return {};
    QSvgRenderer r(svg.toUtf8());
    if (!r.isValid()) return {};
    QImage img(px, px, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    r.render(&p);
    p.end();
    return QPixmap::fromImage(img);
}

} // namespace

const std::vector<std::string>& iconNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const Def& d : kDefs) v.emplace_back(d.name);
        return v;
    }();
    return names;
}

bool hasIcon(const std::string& name) { return table().contains(QString::fromStdString(name)); }

QString iconSvg(const std::string& name, const QColor& stroke) {
    const auto it = table().constFind(QString::fromStdString(name));
    if (it == table().constEnd()) return {};
    QString body = QString::fromLatin1(*it);
    body.replace(QLatin1String("currentColor"), stroke.name());
    return QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"%1\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\">%2</svg>")
        .arg(stroke.name(), body);
}

QIcon icon(const std::string& name) { return icon(name, tokens()); }

QIcon icon(const std::string& name, const Tokens& t) {
    static QMap<QString, QIcon> cache;
    const QString key = QString::fromStdString(name) + QLatin1Char('|') + t.text.name() + t.textStrong.name() + t.dim.name();
    const auto hit = cache.constFind(key);
    if (hit != cache.constEnd()) return *hit;
    if (!hasIcon(name)) return {};
    QIcon out;
    constexpr int kPx = 48;   // drawn large once; Qt scales it down for the 16 - 24 px buttons
    const QPixmap normal = render(name, t.text, kPx), strong = render(name, t.textStrong, kPx), dim = render(name, t.dim, kPx);
    out.addPixmap(normal, QIcon::Normal, QIcon::Off);
    out.addPixmap(strong, QIcon::Active, QIcon::Off);
    out.addPixmap(strong, QIcon::Normal, QIcon::On);
    out.addPixmap(strong, QIcon::Active, QIcon::On);
    out.addPixmap(dim, QIcon::Disabled, QIcon::Off);
    out.addPixmap(dim, QIcon::Disabled, QIcon::On);
    cache.insert(key, out);
    return out;
}

} // namespace native::ui
