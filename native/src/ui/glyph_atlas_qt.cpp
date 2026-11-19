// ui/glyph_atlas_qt -- see glyph_atlas_qt.h.
#include "ui/glyph_atlas_qt.h"

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace native::ui {

std::shared_ptr<const render::GlyphAtlas> makeGlyphAtlas(double scale) {
    using namespace render;
    scale = std::max(1.0, scale);
    QFont font(QStringLiteral("sans-serif"));
    font.setPixelSize(static_cast<int>(std::lround(12 * scale)));
    font.setWeight(QFont::DemiBold);
    const QFontMetricsF fm(font);
    const double margin = std::ceil(3 * scale);
    const double outline = std::max(2.0, 3 * scale);
    double maxAdvance = 0;
    for (int c = kFirstGlyph; c <= kLastGlyph; ++c) maxAdvance = std::max(maxAdvance, fm.horizontalAdvance(QChar(c)));
    const int cellW = static_cast<int>(std::ceil(maxAdvance + 2 * margin));
    const int cellH = static_cast<int>(std::ceil(fm.height() + 2 * margin));
    const int cols = 16, rows = (kLastGlyph - kFirstGlyph + cols) / cols;
    auto atlas = std::make_shared<GlyphAtlas>();
    atlas->width = cols * cellW;
    atlas->height = rows * cellH;
    atlas->lineHeight = static_cast<float>(cellH);
    atlas->margin = static_cast<float>(margin);

    // two layers: the glyph alone (R) and the glyph with its outline (G)
    QImage fill(atlas->width, atlas->height, QImage::Format_ARGB32_Premultiplied), halo(atlas->width, atlas->height, QImage::Format_ARGB32_Premultiplied);
    fill.fill(Qt::transparent);
    halo.fill(Qt::transparent);
    QPainter pf(&fill), ph(&halo);
    pf.setRenderHint(QPainter::Antialiasing);
    ph.setRenderHint(QPainter::Antialiasing);
    QPen stroke(Qt::white, outline);
    stroke.setJoinStyle(Qt::RoundJoin);
    for (int c = kFirstGlyph; c <= kLastGlyph; ++c) {
        const int i = c - kFirstGlyph;
        const int cx = (i % cols) * cellW, cy = (i / cols) * cellH;
        Glyph& g = atlas->glyphs[i];
        g.u0 = static_cast<float>(cx) / atlas->width;
        g.v0 = static_cast<float>(cy) / atlas->height;
        g.u1 = static_cast<float>(cx + cellW) / atlas->width;
        g.v1 = static_cast<float>(cy + cellH) / atlas->height;
        g.w = static_cast<float>(cellW);
        g.h = static_cast<float>(cellH);
        g.advance = static_cast<float>(fm.horizontalAdvance(QChar(c)));
        if (c == ' ') continue;
        QPainterPath path;
        path.addText(QPointF(cx + margin, cy + margin + fm.ascent()), font, QString(QChar(c)));
        ph.strokePath(path, stroke);
        ph.fillPath(path, Qt::white);
        pf.fillPath(path, Qt::white);
    }
    pf.end();
    ph.end();
    atlas->rgba.assign(static_cast<size_t>(atlas->width) * atlas->height * 4, 0);
    for (int y = 0; y < atlas->height; ++y)
        for (int x = 0; x < atlas->width; ++x) {
            uint8_t* px = &atlas->rgba[(static_cast<size_t>(y) * atlas->width + x) * 4];
            px[0] = static_cast<uint8_t>(qAlpha(fill.pixel(x, y)));
            px[1] = static_cast<uint8_t>(qAlpha(halo.pixel(x, y)));
            px[2] = 0;
            px[3] = 255;
        }
    return atlas;
}

} // namespace native::ui
