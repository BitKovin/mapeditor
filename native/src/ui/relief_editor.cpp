// ui/relief_editor -- see relief_editor.h.
#include "ui/relief_editor.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QPushButton>
#include <QScrollArea>
#include <QTransform>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <set>

#include "tools/transform.h"
#include "ui/main_window.h"

namespace native::ui {

namespace rl = native::tools::relief;
namespace mesh = native::tools::mesh;
namespace tl = native::tools;
using render::Vec3;

namespace {

constexpr double kPi = 3.14159265358979323846;

Vec3 rv(const core::Vec3& v) { return {v.x, v.y, v.z}; }
scene::V3 asV3(const Vec3& v) { return {v.x, v.y, v.z}; }
QString qs(const std::string& s) { return QString::fromUtf8(s.c_str()); }

// One repeat of the texture in metres; 1 x 1 when the size is not known (no game folder).
std::pair<double, double> tileSize(MainWindow& w, const std::string& texture) {
    const auto info = w.textureInfo();
    if (info)
        if (const core::texplan::TexInfo* i = info(texture)) {
            const auto n = core::texplan::naturalSize(*i);
            if (n[0] > 1e-6 && n[1] > 1e-6) return {n[0], n[1]};
        }
    return {1, 1};
}

rl::ReliefSession makeSession(MainWindow& w, const std::string& texture) {
    const auto size = tileSize(w, texture);
    const nlohmann::json props = w.textureProps(texture);
    std::shared_ptr<const rl::Relief> relief;
    if (props.is_object() && props.contains("relief") && !props["relief"].is_null()) relief = core::texplan::parseRelief(props["relief"]);
    return rl::ReliefSession(relief.get(), size.first, size.second);
}

// The camera's view-projection for a canvas of w x h pixels: a point to canvas pixels (y down) and depth.
struct Projector {
    render::Mat4 m;
    double W, H;
    Projector(render::OrbitCamera& cam, double w, double h) : m(cam.viewProj(w / std::max(1.0, h))), W(w), H(h) {}
    std::optional<mesh::Screen> operator()(double x, double y, double z) const {
        const double cx = m[0] * x + m[4] * y + m[8] * z + m[12];
        const double cy = m[1] * x + m[5] * y + m[9] * z + m[13];
        const double cz = m[2] * x + m[6] * y + m[10] * z + m[14];
        const double cw = m[3] * x + m[7] * y + m[11] * z + m[15];
        if (cw <= 1e-6) return std::nullopt;
        return mesh::Screen{(cx / cw * 0.5 + 0.5) * W, (cy / cw * 0.5 + 0.5) * H, cz / cw};
    }
};

QPushButton* makeButton(QWidget* parent, const QString& text, const QString& name, const QString& tip = {}) {
    auto* b = new QPushButton(text, parent);
    b->setObjectName(name);
    b->setAutoDefault(false);
    b->setDefault(false);
    b->setFocusPolicy(Qt::NoFocus);
    if (!tip.isEmpty()) b->setToolTip(tip);
    return b;
}

// The affine map from the texture picture (pixels) onto the triangle's screen points: A = S * P^-1 where P / S are the edge matrices at the picture / on the screen.
bool pictureMap(const QPointF* s, const float* const* v, const QSize& img, QTransform& out) {
    double px[3], py[3];
    for (int k = 0; k < 3; ++k) {
        px[k] = static_cast<double>(v[k][6]) * img.width();
        py[k] = static_cast<double>(v[k][7]) * img.height();
    }
    const double p00 = px[1] - px[0], p01 = px[2] - px[0], p10 = py[1] - py[0], p11 = py[2] - py[0];
    const double det = p00 * p11 - p01 * p10;
    if (std::fabs(det) < 1e-9) return false;
    const double i00 = p11 / det, i01 = -p01 / det, i10 = -p10 / det, i11 = p00 / det;
    const double s00 = s[1].x() - s[0].x(), s01 = s[2].x() - s[0].x(), s10 = s[1].y() - s[0].y(), s11 = s[2].y() - s[0].y();
    const double a00 = s00 * i00 + s01 * i10, a01 = s00 * i01 + s01 * i11, a10 = s10 * i00 + s11 * i10, a11 = s10 * i01 + s11 * i11;
    const double dx = s[0].x() - (a00 * px[0] + a01 * py[0]);
    const double dy = s[0].y() - (a10 * px[0] + a11 * py[0]);
    out = QTransform(a00, a10, a01, a11, dx, dy);
    return true;
}

const char* modeName(mesh::SelMode m) { return m == mesh::SelMode::Vertex ? "Vertices" : m == mesh::SelMode::Edge ? "Edges" : "Faces"; }

} // namespace

// ============================================================================================================================================
// ReliefView
// ============================================================================================================================================

ReliefView::ReliefView(ReliefEditor& editor, QWidget* parent) : QWidget(parent), ed_(editor) {
    setObjectName(QStringLiteral("ReliefView"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(360, 280);
}

void ReliefView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(30, 32, 36));
    const rl::ReliefSession& s = ed_.session();
    const core::Brush& b = s.brush();
    const double tw = s.width(), th = s.height();
    const Projector proj(ed_.camera(), width(), height());
    const auto pt = [&](double x, double y, double z) -> std::optional<QPointF> {
        const auto sc = proj(x, y, z);
        if (!sc) return std::nullopt;
        return QPointF(sc->x, sc->y);
    };
    const auto line = [&](double x0, double y0, double z0, double x1, double y1, double z1) {
        const auto a = pt(x0, y0, z0);
        const auto c = pt(x1, y1, z1);
        if (a && c) p.drawLine(*a, *c);
    };
    const render::Vec3 eye = ed_.camera().eye();

    // the surface the texture lies on, and its grid
    {
        QPolygonF quad;
        const double cx[4] = {0, tw, tw, 0}, cy[4] = {0, 0, th, th};
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            const auto q = pt(cx[i], cy[i], 0);
            if (!q) ok = false;
            else quad << *q;
        }
        if (ok) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(52, 56, 64));
            p.drawPolygon(quad);
        }
        if (ed_.snapOn() && ed_.snapStep() > 1e-6 && tw / ed_.snapStep() < 160 && th / ed_.snapStep() < 160) {
            p.setPen(QPen(QColor(255, 255, 255, 22), 1));
            for (double x = ed_.snapStep(); x < tw - 1e-9; x += ed_.snapStep()) line(x, 0, 0, x, th, 0);
            for (double y = ed_.snapStep(); y < th - 1e-9; y += ed_.snapStep()) line(0, y, 0, tw, y, 0);
        }
    }

    const std::vector<float> tri = rl::reliefTriangles(b, tw, th);
    const bool textured = ed_.showTexture() && !ed_.image().isNull();
    const QBrush base = textured ? QBrush(ed_.image()) : QBrush();
    const auto drawTriangles = [&](double ox, double oy) {
        p.setPen(Qt::NoPen);
        for (size_t i = 0; i + 24 <= tri.size(); i += 24) {
            const float* v[3] = {&tri[i], &tri[i + 8], &tri[i + 16]};
            QPointF sp[3];
            bool ok = true;
            for (int k = 0; k < 3 && ok; ++k) {
                const auto q = pt(v[k][0] + ox, v[k][1] + oy, v[k][2]);
                if (!q) ok = false;
                else sp[k] = *q;
            }
            if (!ok) continue;
            QPolygonF poly;
            poly << sp[0] << sp[1] << sp[2];
            const Vec3 n{v[0][3], v[0][4], v[0][5]};
            const Vec3 toEye = render::normalize(Vec3{eye.x - (v[0][0] + ox), eye.y - (v[0][1] + oy), eye.z - v[0][2]});
            const double ndl = render::dot(n, toEye);
            const double shade = 0.45 + 0.55 * std::min(1.0, std::fabs(ndl));
            QTransform map;
            if (textured && pictureMap(sp, v, ed_.image().size(), map)) {
                QBrush tb = base;
                tb.setTransform(map);
                p.setBrush(tb);
            } else {
                p.setBrush(QColor(150, 156, 166));
            }
            p.drawPolygon(poly);
            p.setBrush(QColor(0, 0, 0, static_cast<int>((1 - shade) * 210)));
            p.drawPolygon(poly);
            if (ndl < 0) {   // looks into the surface
                p.setBrush(QColor(210, 40, 40, 110));
                p.drawPolygon(poly);
            }
        }
    };
    if (ed_.showGhosts()) {   // the neighbouring repeats, dimmed: what the seams will look like
        p.setOpacity(0.35);
        for (int gy = -1; gy <= 1; ++gy)
            for (int gx = -1; gx <= 1; ++gx)
                if (gx != 0 || gy != 0) drawTriangles(gx * tw, gy * th);
        p.setOpacity(1);
    }
    drawTriangles(0, 0);

    // the tile border
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(120, 170, 230), 1.2, Qt::DashLine));
    line(0, 0, 0, tw, 0, 0);
    line(tw, 0, 0, tw, th, 0);
    line(tw, th, 0, 0, th, 0);
    line(0, th, 0, 0, 0, 0);

    const mesh::MeshSel& sel = s.sel();
    // selected faces
    if (s.mode() == mesh::SelMode::Face) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 160, 40, 85));
        for (uint32_t id : sel.faces) {
            const core::BrushFace* f = mesh::faceById(b, id);
            if (!f) continue;
            QPolygonF poly;
            bool ok = true;
            for (uint32_t vi : f->loop) {
                const core::Vec3& c = b.vertices[vi];
                const auto q = pt(c.x, c.y, c.z);
                if (!q) ok = false;
                else poly << *q;
            }
            if (ok) p.drawPolygon(poly);
        }
    }
    // edges, each once
    std::set<mesh::Edge> all;
    for (const core::BrushFace& f : b.faces)
        for (size_t i = 0; i < f.loop.size(); ++i) all.insert(mesh::edgeKey(f.loop[i], f.loop[(i + 1) % f.loop.size()]));
    const std::set<mesh::Edge> picked(sel.edges.begin(), sel.edges.end());
    for (const mesh::Edge& e : all) {
        const core::Vec3 &a = b.vertices[e.a], &c = b.vertices[e.b];
        const bool on = picked.count(e) > 0;
        p.setPen(on ? QPen(QColor(255, 170, 40), 2.6) : QPen(QColor(235, 238, 245, 210), 1.1));
        line(a.x, a.y, a.z, c.x, c.y, c.z);
    }
    // the loop cut that would be made
    p.setPen(QPen(QColor(255, 80, 220), 2.4));
    for (const auto& r : ed_.loopOverlay()) {
        const core::Vec3 &a = b.vertices[r.first], &c = b.vertices[r.second];
        line(a.x, a.y, a.z, c.x, c.y, c.z);
    }
    // vertices
    const std::set<uint32_t> chosen(sel.verts.begin(), sel.verts.end());
    p.setPen(Qt::NoPen);
    for (uint32_t i = 0; i < b.vertices.size(); ++i) {
        const core::Vec3& c = b.vertices[i];
        const auto q = pt(c.x, c.y, c.z);
        if (!q) continue;
        const bool on = chosen.count(i) > 0;
        p.setBrush(on ? QColor(255, 170, 40) : QColor(240, 242, 248));
        const double r = on ? 4.5 : 3.2;
        p.drawRect(QRectF(q->x() - r, q->y() - r, 2 * r, 2 * r));
    }
    // the rubber band
    if (box_) {
        p.setBrush(QColor(255, 255, 255, 28));
        p.setPen(QPen(QColor(255, 255, 255, 200), 1, Qt::DashLine));
        p.drawRect(*box_);
    }
    p.setPen(QColor(200, 205, 215));
    p.drawText(QRectF(8, 6, width() - 16, 20), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("%1 · %2 × %3 m").arg(QString::fromLatin1(modeName(s.mode()))).arg(tw, 0, 'g', 4).arg(th, 0, 'g', 4));
}

void ReliefView::mousePressEvent(QMouseEvent* e) {
    setFocus();
    last_ = boxStart_ = e->position();
    ed_.setMouse(e->position());
    drag_ = e->button();
    box_.reset();
    if (ed_.modalActive()) {
        if (e->button() == Qt::LeftButton) ed_.modalConfirm();
        else if (e->button() == Qt::RightButton) ed_.modalCancel();
        drag_ = Qt::NoButton;
        return;
    }
    if (e->button() == Qt::LeftButton && ed_.loopCutArmed()) {
        ed_.hover(e->position().x(), e->position().y());
        if (ed_.loopHover_) ed_.begin(tl::ModalKind::LoopCut, ed_.loopHover_);
        drag_ = Qt::NoButton;
    }
}

void ReliefView::mouseMoveEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    ed_.setMouse(pos);
    if (ed_.modalActive()) {
        ed_.modalMove(pos.x(), pos.y());
        return;
    }
    if (drag_ == Qt::RightButton) {
        ed_.orbit(pos.x() - last_.x(), pos.y() - last_.y());
    } else if (drag_ == Qt::MiddleButton) {
        ed_.pan(pos.x() - last_.x(), pos.y() - last_.y());
    } else if (drag_ == Qt::LeftButton) {
        if ((pos - boxStart_).manhattanLength() > 4 || box_) setBox(QRectF(boxStart_, pos).normalized());
    } else {
        ed_.hover(pos.x(), pos.y());
    }
    if (drag_ != Qt::LeftButton) last_ = pos;
    update();
}

void ReliefView::mouseReleaseEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    if (drag_ == Qt::LeftButton && e->button() == Qt::LeftButton) {
        const bool shift = e->modifiers() & Qt::ShiftModifier;
        if (box_) {
            const QRectF r = *box_;
            setBox(std::nullopt);
            ed_.boxSelect(r.left(), r.top(), r.right(), r.bottom(), shift);
        } else {
            ed_.click(pos.x(), pos.y(), shift);
        }
    }
    if (e->button() == drag_) drag_ = Qt::NoButton;
}

void ReliefView::wheelEvent(QWheelEvent* e) {
    const double dy = -e->angleDelta().y();
    if (ed_.modalActive()) ed_.modalWheel(dy);
    else ed_.zoom(dy);
    e->accept();
}

void ReliefView::keyPressEvent(QKeyEvent* e) {
    if (ed_.key(e->key(), e->modifiers(), e->text())) e->accept();
    else e->ignore();
}

// ============================================================================================================================================
// ReliefEditor
// ============================================================================================================================================

ReliefEditor::ReliefEditor(MainWindow& window, const std::string& texture) : QDialog(&window), win_(window), texture_(texture), session_(makeSession(window, texture)) {
    setObjectName(QStringLiteral("ReliefEditor"));
    resize(1120, 720);
    image_ = win_.loadTextureImage(texture_);
    if (!image_.isNull() && std::max(image_.width(), image_.height()) > 512)
        image_ = image_.scaled(512, 512, Qt::KeepAspectRatio, Qt::FastTransformation);
    startJson_ = rl::reliefToJson(session_.toRelief()).dump();
    cam_.nearPlane = 0.01;
    cam_.yaw = 0.55;
    cam_.pitch = 0.3;
    build();
    frameTile();
    refresh();
    say("LMB select (drag = box, Shift = add) · RMB orbit · MMB pan · wheel zoom · G R S move / rotate / scale · E extrude · I inset · Ctrl+B bevel · Ctrl+R loop cut");
}

ReliefEditor::~ReliefEditor() = default;

QWidget* ReliefEditor::part(const QString& name) const { return findChild<QWidget*>(name); }

QSize ReliefEditor::viewSize() const {
    QSize s = view_ ? view_->size() : QSize();
    if (s.width() < 50 || s.height() < 50) s = view_ ? view_->sizeHint() : QSize(720, 560);
    return s;
}

Vec3 ReliefEditor::viewForward() { return rayAt(viewSize().width() / 2.0, viewSize().height() / 2.0).dir; }

std::optional<mesh::Screen> ReliefEditor::project(const core::Vec3& p) {
    const QSize s = viewSize();
    return Projector(cam_, s.width(), s.height())(p.x, p.y, p.z);
}

render::Ray ReliefEditor::rayAt(double px, double py) {
    const QSize s = viewSize();
    return cam_.rayFromNdc((px / s.width()) * 2 - 1, 1 - (py / s.height()) * 2, static_cast<double>(s.width()) / s.height());
}

void ReliefEditor::frameTile() {
    const QSize s = viewSize();
    const double w = session_.width(), h = session_.height();
    cam_.pivot = {w / 2, h / 2, 0};
    const double aspect = static_cast<double>(s.width()) / s.height();
    cam_.distance = render::clampd(std::max(h / 2, (w / 2) / std::max(0.3, aspect)) / std::tan(cam_.fovY / 2) * 1.35, 0.3, 1000);
    update3d();
}

void ReliefEditor::setView(double yaw, double pitch) {
    cam_.yaw = yaw;
    cam_.pitch = render::clampd(pitch, -1.5, 1.5);
    frameTile();
}

void ReliefEditor::pan(double dx, double dy) {
    cam_.pan(dx, dy, viewSize().height());
    update3d();
}

void ReliefEditor::update3d() {
    if (view_) view_->update();
}

std::string ReliefEditor::say(const std::string& s) {
    status_ = qs(s);
    if (hud_) hud_->setText(status_);
    return s;
}

// ---- panel --------------------------------------------------------------------------------------------------------------------------------

namespace {
struct Btn {
    const char* name;
    const char* text;
    const char* tip;
    std::function<void()> run;
};
} // namespace

void ReliefEditor::build() {
    building_ = true;
    auto* root = new QHBoxLayout(this);
    auto* left = new QVBoxLayout;
    root->addLayout(left, 1);
    view_ = new ReliefView(*this, this);
    left->addWidget(view_, 1);
    hud_ = new QLabel(this);
    hud_->setObjectName(QStringLiteral("lbl:hud"));
    hud_->setWordWrap(true);
    hud_->setMinimumHeight(34);
    left->addWidget(hud_);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFixedWidth(300);
    scroll->setFrameShape(QFrame::NoFrame);
    panel_ = new QWidget;
    auto* col = new QVBoxLayout(panel_);
    scroll->setWidget(panel_);
    root->addWidget(scroll);

    const auto heading = [&](const QString& t) {
        auto* l = new QLabel(t, panel_);
        QFont f = l->font();
        f.setBold(true);
        l->setFont(f);
        col->addSpacing(4);
        col->addWidget(l);
    };
    const auto grid = [&](const std::vector<Btn>& items, int cols, bool checkable = false) {
        auto* g = new QGridLayout;
        g->setSpacing(3);
        int i = 0;
        for (const Btn& b : items) {
            QPushButton* btn = makeButton(panel_, QString::fromUtf8(b.text), QString::fromUtf8(b.name), QString::fromUtf8(b.tip));
            btn->setCheckable(checkable);
            QObject::connect(btn, &QPushButton::clicked, this, [fn = b.run] { fn(); });
            g->addWidget(btn, i / cols, i % cols);
            ++i;
        }
        col->addLayout(g);
    };
    const auto guarded = [this](std::function<std::string()> fn) {
        return [this, fn] {
            if (modal_) {
                say("Finish the running tool first (Enter confirms, Esc cancels)");
                return;
            }
            run(fn());
        };
    };

    heading(QStringLiteral("Select"));
    grid({{"btn:mode1", "Vertices (1)", "Pick vertices", [this] { setMode(mesh::SelMode::Vertex); }},
          {"btn:mode2", "Edges (2)", "Pick edges", [this] { setMode(mesh::SelMode::Edge); }},
          {"btn:mode3", "Faces (3)", "Pick faces", [this] { setMode(mesh::SelMode::Face); }}},
         3, true);
    grid({{"btn:all", "All (A)", "Select everything", [this] { session_.selectAll(); changed(); }},
          {"btn:none", "None (Alt+A)", "Clear the selection", [this] { session_.selectNone(); changed(); }},
          {"btn:invert", "Invert (Ctrl+I)", "Invert the selection", [this] { session_.invert(); changed(); }},
          {"btn:grow", "Grow (+)", "Add the neighbours of the selection", [this] { session_.grow(); changed(); }},
          {"btn:shrink", "Shrink (-)", "Drop the outer ring of the selection", [this] { session_.shrink(); changed(); }}},
         3);

    heading(QStringLiteral("Tools"));
    grid({{"btn:grab", "Move (G)", "Moves the selection; X / Y / Z limit it to an axis, typed digits give metres", [this] { startModal(tl::ModalKind::Grab); }},
          {"btn:rotate", "Rotate (R)", "Rotates the selection about its centre; typed digits give degrees", [this] { startModal(tl::ModalKind::Rotate); }},
          {"btn:scale", "Scale (S)", "Scales the selection about its centre; typed digits give the factor", [this] { startModal(tl::ModalKind::Scale); }},
          {"btn:extrude", "Extrude (E)", "Pushes the selected faces out along their normal (faces mode)", [this] { startModal(tl::ModalKind::Extrude); }},
          {"btn:inset", "Inset (I)", "Insets the selected faces (faces mode)", [this] { startModal(tl::ModalKind::Inset); }},
          {"btn:bevel", "Bevel (Ctrl+B)", "Chamfers the selected edges; the wheel adds segments", [this] { startModal(tl::ModalKind::Bevel); }},
          {"btn:loopcut", "Loop cut (Ctrl+R)", "Hover an edge and click to cut a loop across the faces around it", [this] { armLoopCut(!loopArmed_); }},
          {"btn:merge", "Merge (M)", "Welds the selected vertices into one", guarded([this] { return session_.merge(); })},
          {"btn:dissolve", "Dissolve (X)", "Takes the selected elements out, joining the faces around them", guarded([this] { return session_.dissolve(); })},
          {"btn:fill", "Fill (F)", "Makes a face from the selected open edges", guarded([this] { return session_.fill(); })},
          {"btn:delete", "Delete faces", "Removes the selected faces (the mesh stays open)", guarded([this] { return session_.deleteSelectedFaces(); })},
          {"btn:flip", "Flip (Alt+N)", "Turns the selected faces round", guarded([this] { return session_.flip(); })}},
         2);

    heading(QStringLiteral("Depth of the selection (m)"));
    auto* depthRow = new QHBoxLayout;
    depth_ = new QDoubleSpinBox(panel_);
    depth_->setObjectName(QStringLiteral("spin:depth"));
    depth_->setRange(-rl::kMaxDepth, rl::kMaxDepth);
    depth_->setDecimals(3);
    depth_->setSingleStep(0.05);
    depth_->setToolTip(QStringLiteral("Negative sinks into the surface, positive stands out"));
    depthRow->addWidget(depth_, 1);
    QPushButton* setD = makeButton(panel_, QStringLiteral("Set"), QStringLiteral("btn:depth"), QStringLiteral("Puts every selected vertex at this depth"));
    QObject::connect(setD, &QPushButton::clicked, this, [this] { setDepth(depth_->value()); });
    QPushButton* flat = makeButton(panel_, QStringLiteral("Flatten"), QStringLiteral("btn:flatten"), QStringLiteral("Puts every selected vertex back on the surface"));
    QObject::connect(flat, &QPushButton::clicked, this, [this] { setDepth(0); });
    depthRow->addWidget(setD);
    depthRow->addWidget(flat);
    col->addLayout(depthRow);

    heading(QStringLiteral("Start from"));
    std::vector<Btn> pre;
    static std::vector<std::string> names;   // the button object names must outlive the Btn list
    names.clear();
    for (const rl::PresetInfo& pi : rl::presets()) names.push_back(std::string("btn:preset:") + pi.key);
    size_t pn = 0;
    for (const rl::PresetInfo& pi : rl::presets()) {
        const rl::Preset id = pi.id;
        pre.push_back({names[pn++].c_str(), pi.label, pi.hint, [this, id] { loadPreset(id); }});
    }
    grid(pre, 2);

    heading(QStringLiteral("View"));
    grid({{"btn:view:front", "Front", "Looking straight at the surface", [this] { setView(0, 0); }},
          {"btn:view:side", "Side", "From the side: shows the depth", [this] { setView(kPi / 2, 0); }},
          {"btn:view:top", "Top", "From above", [this] { setView(0, 1.5); }},
          {"btn:view:3d", "3D", "A three-quarter view", [this] { setView(0.55, 0.3); }},
          {"btn:view:frame", "Frame (Home)", "Fits the tile to the window", [this] { frameTile(); }}},
         3);

    heading(QStringLiteral("Display"));
    snapCheck_ = new QCheckBox(QStringLiteral("Snap to grid"), panel_);
    snapCheck_->setObjectName(QStringLiteral("chk:snap"));
    snapCheck_->setChecked(snapOn_);
    texCheck_ = new QCheckBox(QStringLiteral("Show the texture"), panel_);
    texCheck_->setObjectName(QStringLiteral("chk:texture"));
    texCheck_->setChecked(showTexture_);
    ghostCheck_ = new QCheckBox(QStringLiteral("Show the neighbouring repeats"), panel_);
    ghostCheck_->setObjectName(QStringLiteral("chk:ghosts"));
    ghostCheck_->setChecked(showGhosts_);
    col->addWidget(snapCheck_);
    auto* stepRow = new QHBoxLayout;
    stepRow->addWidget(new QLabel(QStringLiteral("Grid step (m)"), panel_));
    stepBox_ = new QDoubleSpinBox(panel_);
    stepBox_->setObjectName(QStringLiteral("spin:step"));
    stepBox_->setRange(0.005, 2);
    stepBox_->setDecimals(3);
    stepBox_->setSingleStep(0.01);
    stepBox_->setValue(snapStep_);
    stepRow->addWidget(stepBox_, 1);
    col->addLayout(stepRow);
    col->addWidget(texCheck_);
    col->addWidget(ghostCheck_);
    QObject::connect(snapCheck_, &QCheckBox::toggled, this, [this](bool on) { snapOn_ = on; update3d(); });
    QObject::connect(texCheck_, &QCheckBox::toggled, this, [this](bool on) { showTexture_ = on; update3d(); });
    QObject::connect(ghostCheck_, &QCheckBox::toggled, this, [this](bool on) { showGhosts_ = on; update3d(); });
    QObject::connect(stepBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) { snapStep_ = v; update3d(); });

    heading(QStringLiteral("Mesh"));
    info_ = new QLabel(panel_);
    info_->setObjectName(QStringLiteral("lbl:info"));
    info_->setWordWrap(true);
    col->addWidget(info_);
    warnings_ = new QWidget(panel_);
    warnings_->setObjectName(QStringLiteral("warnings"));
    auto* wl = new QVBoxLayout(warnings_);
    wl->setContentsMargins(0, 0, 0, 0);
    col->addWidget(warnings_);
    col->addStretch(1);

    state_ = new QLabel(panel_);
    state_->setObjectName(QStringLiteral("lbl:state"));
    col->addWidget(state_);
    auto* foot = new QGridLayout;
    undo_ = makeButton(panel_, QStringLiteral("Undo"), QStringLiteral("btn:undo"), QStringLiteral("Ctrl+Z"));
    redo_ = makeButton(panel_, QStringLiteral("Redo"), QStringLiteral("btn:redo"), QStringLiteral("Ctrl+Y"));
    QPushButton* remove = makeButton(panel_, QStringLiteral("Remove relief"), QStringLiteral("btn:remove"), QStringLiteral("The texture goes back to flat and the window closes"));
    QPushButton* cancel = makeButton(panel_, QStringLiteral("Cancel"), QStringLiteral("btn:cancel"));
    QPushButton* apply = makeButton(panel_, QStringLiteral("Apply"), QStringLiteral("btn:apply"), QStringLiteral("Saves the relief with the texture and closes"));
    QObject::connect(undo_, &QPushButton::clicked, this, [this] { undo(); });
    QObject::connect(redo_, &QPushButton::clicked, this, [this] { redo(); });
    QObject::connect(remove, &QPushButton::clicked, this, [this] { removeRelief(); });
    QObject::connect(cancel, &QPushButton::clicked, this, [this] { reject(); });
    QObject::connect(apply, &QPushButton::clicked, this, [this] { applyAndClose(); });
    foot->addWidget(undo_, 0, 0);
    foot->addWidget(redo_, 0, 1);
    foot->addWidget(remove, 1, 0, 1, 2);
    foot->addWidget(cancel, 2, 0);
    foot->addWidget(apply, 2, 1);
    col->addLayout(foot);
    building_ = false;
}

void ReliefEditor::refresh() {
    if (building_ || !view_) return;
    const mesh::SelMode m = session_.mode();
    const struct {
        const char* name;
        mesh::SelMode mode;
    } modes[] = {{"btn:mode1", mesh::SelMode::Vertex}, {"btn:mode2", mesh::SelMode::Edge}, {"btn:mode3", mesh::SelMode::Face}};
    for (const auto& e : modes)
        if (auto* b = qobject_cast<QPushButton*>(part(QString::fromLatin1(e.name)))) b->setChecked(m == e.mode);
    undo_->setEnabled(session_.canUndo() && !modal_);
    redo_->setEnabled(session_.canRedo() && !modal_);
    const rl::ReliefReport rep = report();
    info_->setText(QStringLiteral("%1 vertices · %2 faces · %3 triangles\ndepth %4 … +%5 m")
                       .arg(rep.vertices)
                       .arg(rep.faces)
                       .arg(rep.triangles)
                       .arg(rep.minZ, 0, 'f', 3)
                       .arg(rep.maxZ, 0, 'f', 3));
    // the problems of the mesh, one line each
    if (auto* lay = warnings_->layout()) {
        while (QLayoutItem* it = lay->takeAt(0)) {
            delete it->widget();
            delete it;
        }
        for (const std::string& w : rep.warnings) {
            auto* l = new QLabel(QStringLiteral("⚠ ") + qs(w), warnings_);
            l->setWordWrap(true);
            l->setStyleSheet(QStringLiteral("color: #e0a030;"));
            lay->addWidget(l);
        }
    }
    if (!depth_->hasFocus()) {
        depth_->blockSignals(true);
        depth_->setValue(session_.hasSelection() ? session_.pivot().z : 0);
        depth_->blockSignals(false);
    }
    state_->setText(dirty() ? QStringLiteral("Unsaved changes") : QStringLiteral("Saved"));
    setWindowTitle(QStringLiteral("Relief: %1%2").arg(qs(texture_), dirty() ? QStringLiteral(" *") : QString()));
    view_->update();
}

rl::ReliefReport ReliefEditor::report() const { return rl::reliefReport(session_.brush(), session_.width(), session_.height()); }

bool ReliefEditor::dirty() const { return rl::reliefToJson(session_.toRelief()).dump() != startJson_; }

void ReliefEditor::changed() { refresh(); }

void ReliefEditor::run(const std::string& err) {
    if (!err.empty()) say(err);
    changed();
}

// ---- picking ---------------------------------------------------------------------------------------------------------------------------------

void ReliefEditor::click(double px, double py, bool shift) {
    if (modal_) return;
    const core::Brush& b = session_.brush();
    const mesh::SelMode mode = session_.mode();
    const mesh::Project proj = [this](const core::Vec3& p) { return project(p); };
    mesh::MeshSel add;
    if (mode == mesh::SelMode::Vertex) {
        if (const auto v = mesh::pickVertex(b, proj, px, py, 10)) add = mesh::selFromVerts(b, {*v});
    } else if (mode == mesh::SelMode::Edge) {
        if (const auto e = mesh::pickEdge(b, proj, px, py, 8)) add = mesh::selFromEdges(b, {*e});
    } else {
        const render::Ray r = rayAt(px, py);
        if (const auto id = mesh::pickFaceId(b, mesh::Ray{core::Vec3(r.origin.x, r.origin.y, r.origin.z), core::Vec3(r.dir.x, r.dir.y, r.dir.z)})) add = mesh::selFromFaces(b, {*id});
    }
    if (add.empty()) {
        if (!shift) session_.selectNone();
        changed();
        return;
    }
    session_.setSel(shift ? mesh::combineSel(b, session_.sel(), add, mode, true) : add);
    changed();
}

void ReliefEditor::boxSelect(double x0, double y0, double x1, double y1, bool shift) {
    if (modal_) return;
    const core::Brush& b = session_.brush();
    const mesh::Project proj = [this](const core::Vec3& p) { return project(p); };
    const mesh::MeshSel box = mesh::boxSelect(b, proj, session_.mode(), std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1));
    session_.setSel(shift ? mesh::combineSel(b, session_.sel(), box, session_.mode(), false) : box);
    changed();
}

void ReliefEditor::hover(double px, double py) {
    mouse_ = QPointF(px, py);
    if (!loopArmed_ || modal_) return;
    const mesh::Project proj = [this](const core::Vec3& p) { return project(p); };
    loopHover_ = mesh::pickEdge(session_.brush(), proj, px, py, 14);
    update3d();
}

void ReliefEditor::armLoopCut(bool on) {
    if (modal_) return;
    loopArmed_ = on;
    loopHover_.reset();
    if (on) {
        say("Loop cut: hover an edge and click; Esc cancels");
        hover(mouse_.x(), mouse_.y());
        if (loopHover_ && view_ && view_->underMouse()) begin(tl::ModalKind::LoopCut, loopHover_);
    } else {
        say("");
    }
    update3d();
}

std::vector<std::pair<uint32_t, uint32_t>> ReliefEditor::loopOverlay() const {
    if (!loopHover_ || modal_) return {};
    const auto ring = mesh::edgeRing(session_.brush(), *loopHover_);
    if (!ring) return {};
    return ring->ring;
}

// ---- tools -----------------------------------------------------------------------------------------------------------------------------------

void ReliefEditor::setMode(mesh::SelMode m) {
    if (modal_) return;
    session_.setMode(m);
    changed();
}

void ReliefEditor::loadPreset(rl::Preset p) {
    if (modal_) return;
    session_.load(rl::buildPreset(p, session_.width(), session_.height()));
    session_.selectNone();
    say("");
    changed();
}

void ReliefEditor::setDepth(double z) {
    if (modal_) return;
    run(session_.setDepth(z));
}

void ReliefEditor::undo() {
    if (modal_) return;
    session_.undo();
    changed();
}

void ReliefEditor::redo() {
    if (modal_) return;
    session_.redo();
    changed();
}

bool ReliefEditor::startModal(tl::ModalKind kind) {
    if (kind != tl::ModalKind::LoopCut) return begin(kind, std::nullopt);
    std::optional<mesh::Edge> edge = loopHover_;
    if (!edge) {
        const mesh::Project proj = [this](const core::Vec3& p) { return project(p); };
        edge = mesh::pickEdge(session_.brush(), proj, mouse_.x(), mouse_.y(), 14);
    }
    return begin(kind, edge);
}

bool ReliefEditor::begin(tl::ModalKind kind, std::optional<mesh::Edge> edge) {
    if (modal_ || session_.inModal()) return false;
    const core::Brush& b = session_.brush();
    const mesh::MeshSel sel = session_.sel();
    const auto hint = [this](const char* t) {
        say(t);
        return false;
    };
    if (kind != tl::ModalKind::LoopCut && sel.verts.empty()) return hint("Select something first");
    Modal m;
    m.kind = kind;
    const core::Vec3 c = session_.pivot();
    m.pivot = rv(c);
    const Vec3 fwd = viewForward();
    m.normal = std::fabs(fwd.z) > 0.25 ? Vec3{0, 0, 1} : fwd;   // the plane the pointer slides the selection on: the surface, unless it is seen edge-on
    m.mouse0 = {mouse_.x(), mouse_.y()};
    const render::Ray ray = rayAt(mouse_.x(), mouse_.y());
    m.startPoint = tl::planePoint(ray, m.pivot, m.normal).value_or(m.pivot);
    const QSize vs = viewSize();
    if (const auto sp = project(c)) m.screenPivot = {sp->x, sp->y};
    else m.screenPivot = {vs.width() / 2.0, vs.height() / 2.0};
    m.mpp = tl::metresPerPixelAt(cam_.distanceTo(m.pivot), cam_.fovY, vs.height());
    switch (kind) {
        case tl::ModalKind::Grab:
        case tl::ModalKind::Rotate:
        case tl::ModalKind::Scale: break;
        case tl::ModalKind::Extrude:
        case tl::ModalKind::Inset: {
            if (session_.mode() != mesh::SelMode::Face || sel.faces.empty())
                return hint(kind == tl::ModalKind::Extrude ? "Extrude works on selected faces: press 3, select a face, then E" : "Inset works on selected faces: press 3, select a face, then I");
            m.faces = sel.faces;
            if (kind == tl::ModalKind::Extrude) {
                std::vector<const core::BrushFace*> faces;
                for (uint32_t id : sel.faces)
                    if (const core::BrushFace* f = mesh::faceById(b, id)) faces.push_back(f);
                const core::Vec3 n = mesh::averageNormal(b, faces);
                if (n.x == 0 && n.y == 0 && n.z == 0) return hint("These faces have no common direction to extrude along");
                m.dir = render::normalize(rv(n));
                m.t0 = tl::lineDistance(ray, m.pivot, m.dir).value_or(0);
            }
            break;
        }
        case tl::ModalKind::Bevel:
            if (sel.edges.empty()) return hint("Bevel works on edges (or faces): press 2 or 3, select, then Ctrl+B");
            m.edges = sel.edges;
            m.segments = 1;
            break;
        case tl::ModalKind::LoopCut: {
            if (!edge) return hint("Loop cut needs an edge: hover one, then Ctrl+R");
            const auto ring = mesh::edgeRing(b, *edge);
            if (!ring || ring->ring.empty()) return hint("No loop passes through this edge (it needs quad faces around it)");
            const core::Vec3 p = b.vertices[ring->ring[0].first], q = b.vertices[ring->ring[0].second];
            const Vec3 d{q.x - p.x, q.y - p.y, q.z - p.z};
            m.edge = *edge;
            m.railFrom = rv(p);
            m.railLength = render::length(d);
            m.railDir = render::normalize(d);
            m.fraction = 0.5;
            m.value = 0.5;
            m.pivot = {(p.x + q.x) / 2, (p.y + q.y) / 2, (p.z + q.z) / 2};
            break;
        }
    }
    session_.beginModal();
    modal_ = m;
    loopArmed_ = false;
    loopHover_.reset();
    modalApply();
    return true;
}

void ReliefEditor::modalApply() {
    if (!modal_) return;
    Modal& m = *modal_;
    const std::optional<double> typed = tl::typedValue(m.typed);
    const render::Ray ray = rayAt(mouse_.x(), mouse_.y());
    const tl::Pixel from = m.mouse0, to{mouse_.x(), mouse_.y()};
    const double st = step();
    const Vec3 fwd = viewForward();
    rl::ReliefOp op;
    switch (m.kind) {
        case tl::ModalKind::Grab: {
            Vec3 d{0, 0, 0};
            if (typed) d = tl::axisDir(m.axis < 0 ? 0 : m.axis) * *typed;
            else d = tl::grabDelta(ray, m.startPoint, m.pivot, m.normal, m.axis, st);
            m.value = render::length(d);
            op = [d](const core::Brush& b, const mesh::MeshSel& s) {
                return mesh::transformVerts(b, s.verts, [d](const core::Vec3& p, uint32_t) { return core::Vec3(p.x + d.x, p.y + d.y, p.z + d.z); });
            };
            break;
        }
        case tl::ModalKind::Rotate: {
            const Vec3 toward = fwd * -1.0;
            const Vec3 axisVec = m.axis >= 0 ? tl::axisDir(m.axis) : (std::fabs(fwd.z) > 0.25 ? Vec3{0, 0, 1} : toward);
            const double angle = typed ? *typed * kPi / 180.0 : tl::rotateAngle(m.screenPivot, from, to, render::dot(axisVec, toward) >= 0, snapOn_ ? 15.0 * kPi / 180.0 : 0);
            m.value = angle;
            const tl::Affine map = tl::rotateAbout(asV3(m.pivot), asV3(axisVec), angle);
            op = [map](const core::Brush& b, const mesh::MeshSel& s) {
                return mesh::transformVerts(b, s.verts, [map](const core::Vec3& p, uint32_t) {
                    const scene::V3 q = map({p.x, p.y, p.z});
                    return core::Vec3(q[0], q[1], q[2]);
                });
            };
            break;
        }
        case tl::ModalKind::Scale: {
            const double f = typed ? *typed : tl::scaleFactor(m.screenPivot, from, to, snapOn_ ? 0.1 : 0);
            m.value = f;
            const tl::Affine map = tl::scaleAbout(asV3(m.pivot), asV3(tl::scaleVector(f == 0 ? 1e-6 : f, m.axis)));
            op = [map](const core::Brush& b, const mesh::MeshSel& s) {
                return mesh::transformVerts(b, s.verts, [map](const core::Vec3& p, uint32_t) {
                    const scene::V3 q = map({p.x, p.y, p.z});
                    return core::Vec3(q[0], q[1], q[2]);
                });
            };
            break;
        }
        case tl::ModalKind::Extrude: {
            double dist = m.value;
            if (typed) {
                dist = *typed;
            } else if (std::fabs(render::dot(fwd, m.dir)) > 0.85) {   // looking along the direction: the pointer going up / right pushes outwards
                const double raw = ((to.x - from.x) - (to.y - from.y)) * m.mpp * 0.7071;
                dist = st > 0 ? std::round(raw / st) * st : raw;
            } else if (const auto along = tl::lineDistance(ray, m.pivot, m.dir)) {
                dist = st > 0 ? std::round((*along - m.t0) / st) * st : *along - m.t0;
            }
            m.value = dist;
            const std::vector<uint32_t> faces = m.faces;
            op = [faces, dist](const core::Brush& b, const mesh::MeshSel&) { return mesh::extrudeFaces(b, faces, dist); };
            break;
        }
        case tl::ModalKind::Inset: {
            const double dist = typed ? *typed : tl::widthFromMouse(from, to, m.mpp, st > 0 ? st / 2 : 0);
            m.value = dist;
            const std::vector<uint32_t> faces = m.faces;
            op = [faces, dist](const core::Brush& b, const mesh::MeshSel&) { return mesh::insetFaces(b, faces, dist); };
            break;
        }
        case tl::ModalKind::Bevel: {
            const double dist = typed ? *typed : tl::widthFromMouse(from, to, m.mpp, st > 0 ? st / 2 : 0);
            m.value = dist;
            const std::vector<mesh::Edge> edges = m.edges;
            const int segments = m.segments;
            op = [edges, dist, segments](const core::Brush& b, const mesh::MeshSel& s) { return mesh::bevelEdges(b, s.edges.empty() ? edges : s.edges, dist, segments); };
            break;
        }
        case tl::ModalKind::LoopCut: {
            double frac = m.fraction;
            if (typed) frac = *typed;
            else if (const auto along = tl::lineDistance(ray, m.railFrom, m.railDir); along && m.railLength > 1e-9) frac = *along / m.railLength;
            m.fraction = std::min(0.999, std::max(0.001, frac));
            m.value = m.fraction;
            const mesh::Edge edge = m.edge;
            const double f = m.fraction;
            op = [edge, f](const core::Brush& b, const mesh::MeshSel&) { return mesh::loopCut(b, edge, f); };
            break;
        }
    }
    const std::string err = session_.modalUpdate(op);
    say(err.empty() ? tl::describeModal(m.kind, m.value, m.axis, m.typed, m.kind == tl::ModalKind::Bevel ? m.segments : -1) : err);
    refresh();
}

void ReliefEditor::modalMove(double px, double py) {
    mouse_ = QPointF(px, py);
    if (modal_) modalApply();
}

void ReliefEditor::modalKey(const std::string& typed) {
    if (!modal_) return;
    if (const auto next = tl::typeInto(modal_->typed, typed)) {
        modal_->typed = *next;
        modalApply();
    }
}

void ReliefEditor::modalAxis(int axis) {
    if (!modal_) return;
    const tl::ModalKind k = modal_->kind;
    if (k != tl::ModalKind::Grab && k != tl::ModalKind::Rotate && k != tl::ModalKind::Scale) return;
    modal_->axis = modal_->axis == axis ? -1 : axis;
    modalApply();
}

void ReliefEditor::modalWheel(double dy) {
    if (!modal_ || modal_->kind != tl::ModalKind::Bevel) return;
    modal_->segments = tl::stepSegments(modal_->segments, dy, mesh::kMaxBevelSegments);
    modalApply();
}

void ReliefEditor::endModal(bool confirm) {
    if (!modal_) return;
    session_.endModal(confirm);
    modal_.reset();
    say(confirm ? "" : "Cancelled");
    changed();
}

void ReliefEditor::modalConfirm() { endModal(true); }
void ReliefEditor::modalCancel() { endModal(false); }

// ---- keys ------------------------------------------------------------------------------------------------------------------------------------

bool ReliefEditor::key(int k, Qt::KeyboardModifiers mods, const QString&) {
    const bool ctrl = mods & Qt::ControlModifier, alt = mods & Qt::AltModifier, shift = mods & Qt::ShiftModifier;
    if (modal_) {
        if (k == Qt::Key_Return || k == Qt::Key_Enter) modalConfirm();
        else if (k == Qt::Key_Escape) modalCancel();
        else if (k == Qt::Key_X) modalAxis(0);
        else if (k == Qt::Key_Y) modalAxis(1);
        else if (k == Qt::Key_Z) modalAxis(2);
        else if (k >= Qt::Key_0 && k <= Qt::Key_9) modalKey(std::string(1, static_cast<char>('0' + (k - Qt::Key_0))));
        else if (k == Qt::Key_Period || k == Qt::Key_Comma) modalKey(".");
        else if (k == Qt::Key_Minus) modalKey("-");
        else if (k == Qt::Key_Backspace) modalKey("Backspace");
        return true;
    }
    if (k == Qt::Key_Escape && loopArmed_) {
        armLoopCut(false);
        return true;
    }
    if (ctrl) {
        switch (k) {
            case Qt::Key_Z: shift ? redo() : undo(); return true;
            case Qt::Key_Y: redo(); return true;
            case Qt::Key_I: session_.invert(); changed(); return true;
            case Qt::Key_B: startModal(tl::ModalKind::Bevel); return true;
            case Qt::Key_R: armLoopCut(!loopArmed_); return true;
            default: return false;
        }
    }
    if (alt) {
        if (k == Qt::Key_A) {
            session_.selectNone();
            changed();
            return true;
        }
        if (k == Qt::Key_N) {
            run(session_.flip());
            return true;
        }
        return false;
    }
    switch (k) {
        case Qt::Key_1: setMode(mesh::SelMode::Vertex); return true;
        case Qt::Key_2: setMode(mesh::SelMode::Edge); return true;
        case Qt::Key_3: setMode(mesh::SelMode::Face); return true;
        case Qt::Key_A: session_.selectAll(); changed(); return true;
        case Qt::Key_Plus:
        case Qt::Key_Equal: session_.grow(); changed(); return true;
        case Qt::Key_Minus: session_.shrink(); changed(); return true;
        case Qt::Key_M: run(session_.merge()); return true;
        case Qt::Key_X: run(session_.dissolve()); return true;
        case Qt::Key_Delete: run(session_.deleteSelectedFaces()); return true;
        case Qt::Key_F: run(session_.fill()); return true;
        case Qt::Key_G: startModal(tl::ModalKind::Grab); return true;
        case Qt::Key_R: startModal(tl::ModalKind::Rotate); return true;
        case Qt::Key_S: startModal(tl::ModalKind::Scale); return true;
        case Qt::Key_E: startModal(tl::ModalKind::Extrude); return true;
        case Qt::Key_I: startModal(tl::ModalKind::Inset); return true;
        case Qt::Key_Home: frameTile(); return true;
        default: return false;
    }
}

// ---- leaving ---------------------------------------------------------------------------------------------------------------------------------

std::string ReliefEditor::save() {
    if (modal_) endModal(true);
    const rl::Relief relief = session_.toRelief();
    nlohmann::json props = win_.textureProps(texture_);
    if (!props.is_object()) props = nlohmann::json::object();
    if (core::texplan::isFlatRelief(relief)) props["relief"] = nullptr;
    else props["relief"] = rl::reliefToJson(relief);
    const std::string err = win_.saveTextureProps(texture_, props);
    if (!err.empty()) {
        say("Not saved: " + err);
        return err;
    }
    startJson_ = rl::reliefToJson(relief).dump();
    say("Saved");
    refresh();
    emit saved();
    return {};
}

bool ReliefEditor::applyAndClose() {
    if (!save().empty()) return false;
    accept();
    return true;
}

void ReliefEditor::discard() {
    discarding_ = true;
    reject();
}

void ReliefEditor::removeRelief() {
    if (modal_) endModal(false);
    if (isVisible() && !core::texplan::isFlatRelief(session_.toRelief()) &&
        QMessageBox::question(this, QStringLiteral("Remove relief"), QStringLiteral("Remove the relief of this texture? It becomes flat again."), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    session_.reset();
    if (save().empty()) accept();
}

void ReliefEditor::reject() {
    if (modal_) endModal(false);
    if (dirty() && isVisible() && !discarding_) {
        QMessageBox box(this);
        box.setWindowTitle(QStringLiteral("Relief"));
        box.setText(QStringLiteral("The relief has changes that are not saved."));
        QPushButton* saveBtn = box.addButton(QStringLiteral("Save"), QMessageBox::AcceptRole);
        QPushButton* dropBtn = box.addButton(QStringLiteral("Discard"), QMessageBox::DestructiveRole);
        box.addButton(QStringLiteral("Keep editing"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == saveBtn) {
            if (!save().empty()) return;
        } else if (box.clickedButton() != dropBtn) {
            return;
        }
    }
    QDialog::reject();
}

} // namespace native::ui
