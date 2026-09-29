/*
  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  Viewer3D implementation. Part of the opm_flow_windows harness; GPL v3+
  (see repository LICENSE).
*/
#include "Viewer3D.h"

#include "FlowLayout.h"

#include "GuiPaths.h"

#include "CasePath.h"

#include "DeckRegions.h"

#include <opm/io/eclipse/EGrid.hpp>
#include <opm/io/eclipse/EInit.hpp>
#include <opm/io/eclipse/ERst.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QPainter>
#include <QProxyStyle>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSurfaceFormat>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <map>
#include <set>
#include <tuple>

// ===========================================================================
// GridGLWidget
// ===========================================================================
namespace {

// Qt's default is that a click on a slider's groove pages *towards* the click
// (by pageStep) instead of going there. For a report-step bar that is useless -
// clicking a moment in the run should show that moment. This style hint is the
// supported way to ask for it, and it also lets a press anywhere on the groove
// grab the handle so the drag continues from there.
class AbsoluteSeekStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;
    int styleHint(StyleHint hint, const QStyleOption* opt, const QWidget* w,
                  QStyleHintReturn* ret) const override
    {
        if (hint == SH_Slider_AbsoluteSetButtons) return Qt::LeftButton;
        return QProxyStyle::styleHint(hint, opt, w, ret);
    }
};

const char* kVertBody = R"(
in vec3 aPos;
in vec3 aNrm;
in vec3 aCol;
uniform mat4 uMvp;
uniform mat3 uNrm;
uniform float uZScale;
out vec3 vCol;
out float vLight;
// Shading on a colour-mapped grid is a compromise: it multiplies the colour,
// so every bit of it is a lie about the value the colour stands for. None at
// all is worse - the faces of a cell come out identical and the block reads as
// a flat silhouette, with no steps, no faults, no layering - so the aim is the
// least that still shows the shape.
//
// uNrm puts the normal in EYE space, so the light travels with the camera and
// no face can be turned away from it. It used to be the grid's own space with
// the light straight down: a vertical face scored abs(n.z) = 0 and came out at
// a quarter brightness, a quarter of the way to black at every angle there is,
// and orbiting could not help because the light was welded to the grid.
//
// Slightly off the view axis rather than a pure headlight: dead-on, the three
// visible faces of a block all score about the same and it flattens out. Up
// and to the left is the convention, and it keeps them apart.
//
// The range is 0.55 to 1.00. A quarter of the way to black, as it was, buried
// the sides; 0.80 kept the colours honest but flattened a folded surface into
// a wash, with the relief that tells a fault from a fold gone with it. This
// sits between the two, and where exact colour matters more than shape the
// shading box turns it off outright.
void main() {
    vec3 p = vec3(aPos.x, aPos.y, aPos.z * uZScale);
    gl_Position = uMvp * vec4(p, 1.0);
    vec3 n = normalize(uNrm * aNrm);
    vec3 L = normalize(vec3(-0.30, 0.45, 1.0));
    vLight = 0.55 + 0.45 * abs(dot(n, L));
    vCol = aCol;
}
)";

const char* kFragBody = R"(
in vec3 vCol;
in float vLight;
uniform bool uFlat;
out vec4 frag;
void main() {
    if (vCol.r < 0.0) discard;      // a cell outside the region filter
    frag = uFlat ? vec4(vCol, 1.0) : vec4(vCol * vLight, 1.0);
}
)";

// simple jet-style colormap, t in [0,1]
void jet(float t, float& r, float& g, float& b)
{
    t = std::clamp(t, 0.0f, 1.0f);
    r = std::clamp(1.5f - std::fabs(4.0f * t - 3.0f), 0.f, 1.f);
    g = std::clamp(1.5f - std::fabs(4.0f * t - 2.0f), 0.f, 1.f);
    b = std::clamp(1.5f - std::fabs(4.0f * t - 1.0f), 0.f, 1.f);
}

} // namespace

GridGLWidget::GridGLWidget(QWidget* parent)
    : QOpenGLWidget(parent)
    , vboPos_(QOpenGLBuffer::VertexBuffer)
    , vboNrm_(QOpenGLBuffer::VertexBuffer)
    , vboCol_(QOpenGLBuffer::VertexBuffer)
{
    QSurfaceFormat fmt = format();
    fmt.setSamples(4);
    fmt.setDepthBufferSize(24);
    setFormat(fmt);
    setFocusPolicy(Qt::ClickFocus);
}

GridGLWidget::~GridGLWidget()
{
    makeCurrent();
    vboPos_.destroy(); vboNrm_.destroy(); vboCol_.destroy();
    prog_.reset();
    doneCurrent();
}

void GridGLWidget::setMesh(std::vector<float> pos, std::vector<float> nrm, int cellCount)
{
    pos_ = std::move(pos);
    nrm_ = std::move(nrm);
    cellCount_ = cellCount;
    vertCount_ = int(pos_.size() / 3);
    col_.assign(pos_.size(), 0.6f);          // grey until values arrive
    hasValues_ = false;
    values_.clear(); regions_.clear(); mask_.clear();
    categorical_ = false;
    regionLegend_.clear();
    shownCells_ = cellCount;

    // bounding box for the camera
    bboxMin_ = QVector3D( 1e30f,  1e30f,  1e30f);
    bboxMax_ = QVector3D(-1e30f, -1e30f, -1e30f);
    for (size_t i = 0; i + 2 < pos_.size(); i += 3) {
        bboxMin_.setX(std::min(bboxMin_.x(), pos_[i]));
        bboxMax_.setX(std::max(bboxMax_.x(), pos_[i]));
        bboxMin_.setY(std::min(bboxMin_.y(), pos_[i+1]));
        bboxMax_.setY(std::max(bboxMax_.y(), pos_[i+1]));
        bboxMin_.setZ(std::min(bboxMin_.z(), pos_[i+2]));
        bboxMax_.setZ(std::max(bboxMax_.z(), pos_[i+2]));
    }
    // An empty mesh (the last case was removed) leaves the sentinels above
    // untouched; collapse them so resetCamera() below sees a sane box.
    if (pos_.empty()) bboxMin_ = bboxMax_ = QVector3D();
    meshDirty_ = colorDirty_ = true;

    // Default orientation: put the model's long horizontal axis across the
    // screen. Principal xy axis of the mesh via the covariance of the vertex
    // x/y coordinates (cheap single pass; vertices weight cells evenly).
    homeYaw_ = -50.f;                        // oblique fallback (squarish grids)
    const size_t nv = pos_.size() / 3;
    if (nv > 0) {
        double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
        for (size_t i = 0; i + 2 < pos_.size(); i += 3) {
            const double x = pos_[i], y = pos_[i + 1];
            sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y;
        }
        const double n   = double(nv);
        const double cxx = sxx / n - (sx / n) * (sx / n);
        const double cyy = syy / n - (sy / n) * (sy / n);
        const double cxy = sxy / n - (sx / n) * (sy / n);
        const double tr  = cxx + cyy;
        const double det = std::sqrt((cxx - cyy) * (cxx - cyy) + 4.0 * cxy * cxy);
        const double lmax = 0.5 * (tr + det), lmin = 0.5 * (tr - det);
        if (lmin <= 0.0 || lmax > 1.5 * lmin) {   // clearly elongated
            // azimuth of the major axis; yaw = -azimuth maps it to screen-x
            const double az = 0.5 * std::atan2(2.0 * cxy, cxx - cyy);
            homeYaw_ = float(-az * (180.0 / 3.14159265358979323846));
        }
    }
    resetCamera();
}

void GridGLWidget::setCellValues(const std::vector<float>& v, const QString& legendTitle)
{
    legendTitle_ = legendTitle;
    values_ = v;
    regions_.clear();
    categorical_ = false;
    recolor();
}

void GridGLWidget::setCellRegions(const std::vector<int>& r, const QString& legendTitle)
{
    legendTitle_ = legendTitle;
    regions_ = r;
    values_.clear();
    categorical_ = true;
    recolor();
}

void GridGLWidget::setCellMask(std::vector<char> visible)
{
    mask_ = std::move(visible);
    recolor();
}

void GridGLWidget::setValueRange(float lo, float hi, const QString& note, bool redraw)
{
    fixedLo_ = lo; fixedHi_ = hi;
    rangeNote_ = hi > lo ? note : QString();
    if (redraw) recolor();
}

bool GridGLWidget::valueRange(float& lo, float& hi) const
{
    if (!hasValues_ || categorical_) return false;
    lo = vmin_; hi = vmax_;
    return true;
}

QColor GridGLWidget::regionColor(int region)
{
    // Okabe-Ito, then the rest of Tol's muted set: thirteen that stay apart
    // for the common colour blindnesses, which covers most decks. Past that,
    // hues a golden angle apart, so consecutive numbers still differ.
    static const QColor kBase[] = {
        QColor(0x00, 0x72, 0xB2), QColor(0xE6, 0x9F, 0x00), QColor(0x00, 0x9E, 0x73),
        QColor(0xCC, 0x79, 0xA7), QColor(0x56, 0xB4, 0xE9), QColor(0xD5, 0x5E, 0x00),
        QColor(0xF0, 0xE4, 0x42), QColor(0x33, 0x22, 0x88), QColor(0x88, 0x22, 0x55),
        QColor(0x44, 0xAA, 0x99), QColor(0x99, 0x99, 0x33), QColor(0xAA, 0x44, 0x99),
        QColor(0xCC, 0x66, 0x77),
    };
    constexpr int kBaseCount = int(sizeof(kBase) / sizeof(kBase[0]));
    if (region <= 0) return QColor(0x40, 0x40, 0x40);
    if (region <= kBaseCount) return kBase[region - 1];
    const double hue = std::fmod(double(region) * 137.508, 360.0);
    return QColor::fromHsvF(hue / 360.0, region % 2 ? 0.75 : 0.55, region % 3 ? 0.85 : 0.65);
}

void GridGLWidget::recolor()
{
    hasValues_ = false;
    regionLegend_.clear();
    const bool masked = cellCount_ > 0 && int(mask_.size()) == cellCount_;
    const auto shown = [&](int c) { return !masked || mask_[size_t(c)] != 0; };
    const auto paint = [this](int c, float r, float g, float b) {
        float* dst = &col_[size_t(c) * 36 * 3];
        for (int k = 0; k < 36; ++k) { dst[0] = r; dst[1] = g; dst[2] = b; dst += 3; }
    };
    shownCells_ = 0;
    for (int c = 0; c < cellCount_; ++c) if (shown(c)) ++shownCells_;

    if (categorical_ && int(regions_.size()) == cellCount_ && cellCount_ > 0) {
        std::map<int, int> count;
        for (int c = 0; c < cellCount_; ++c) if (shown(c)) ++count[regions_[size_t(c)]];
        for (const auto& [reg, n] : count) regionLegend_.push_back({ reg, n });
        for (int c = 0; c < cellCount_; ++c) {
            const QColor q = regionColor(regions_[size_t(c)]);
            paint(c, float(q.redF()), float(q.greenF()), float(q.blueF()));
        }
        hasValues_ = !regionLegend_.isEmpty();
    } else if (!categorical_ && int(values_.size()) == cellCount_ && cellCount_ > 0) {
        bool any = false;
        for (int c = 0; c < cellCount_; ++c) {
            if (!shown(c)) continue;
            const float v = values_[size_t(c)];
            if (!any) { vmin_ = vmax_ = v; any = true; }
            else      { vmin_ = std::min(vmin_, v); vmax_ = std::max(vmax_, v); }
        }
        if (fixedHi_ > fixedLo_) { vmin_ = fixedLo_; vmax_ = fixedHi_; }
        const float span = (vmax_ > vmin_) ? (vmax_ - vmin_) : 1.0f;
        for (int c = 0; c < cellCount_; ++c) {
            float r, g, b;
            jet((values_[size_t(c)] - vmin_) / span, r, g, b);
            paint(c, r, g, b);
        }
        hasValues_ = any;
    } else {
        std::fill(col_.begin(), col_.end(), 0.6f);
    }
    if (masked)
        for (int c = 0; c < cellCount_; ++c) if (!shown(c)) paint(c, -1.f, -1.f, -1.f);
    colorDirty_ = true;
    update();
}

void GridGLWidget::setWells(const QVector<WellPath>& wells)
{
    wells_ = wells;
    update();
}

void GridGLWidget::setZScale(double s)
{
    // The camera stands off by the model's size, and exaggerating Z changes
    // that size - so hold the standoff in proportion, or turning the dial up
    // walks the camera into the grid it is looking at.
    if (zscale_ > 0.0 && s > 0.0 && vertCount_ > 0) {
        const QVector3D d = bboxMax_ - bboxMin_;
        const auto diag = [&d](double z) {
            return QVector3D(d.x(), d.y(), d.z() * float(z)).length();
        };
        const float before = diag(zscale_), after = diag(s);
        if (before > 1e-6f) dist_ *= after / before;
    }
    zscale_ = s;
    update();
}

void GridGLWidget::resetCamera()
{
    // Measured on the grid AS DRAWN: the shader stretches Z by uZScale, so a
    // standoff taken from the raw box puts the camera inside anything with a
    // thin footprint and an exaggerated depth - a 1x1x20 column at 10x lands
    // the eye well inside the rock.
    const QVector3D d = bboxMax_ - bboxMin_;
    const QVector3D drawn(d.x(), d.y(), d.z() * float(zscale_));
    dist_ = std::max(1.0f, drawn.length()) * 1.3f;
    // Like looking at a mountain from afar: the long axis of the grid across
    // the screen (homeYaw_, from the mesh PCA), seen from the side and a
    // little above. In this camera parameterization negative pitch views
    // from above (the depth axis then points downwards on screen, as the
    // orientation gizmo shows).
    yaw_ = homeYaw_; pitch_ = -18.f;
    panOffset_ = QVector3D();
    update();
}

void GridGLWidget::setStandardView(StdView v)
{
    // pitch 0 looks straight down on the model, -90 is a pure side view and
    // +/-180 is from underneath (see mvp(): the camera orbits in the Y-Z
    // plane before the yaw spin).
    yaw_ = homeYaw_;
    switch (v) {
        case StdView::Home:   pitch_ =  -18.f; break;
        case StdView::Top:    pitch_ =    0.f; break;
        case StdView::Bottom: pitch_ =  180.f; break;
        case StdView::Side:   pitch_ =  -90.f; break;
    }
    update();
}

void GridGLWidget::initializeGL()
{
    initializeOpenGLFunctions();
    glClearColor(0.96f, 0.97f, 0.98f, 1.0f);
    prog_ = std::make_unique<QOpenGLShaderProgram>();
    // Which GLSL to speak is the context's choice, not ours. On Wayland the
    // NVIDIA driver hands Qt an OpenGL ES context, and ES rejects "#version
    // 130" outright - and has no default precision for a float in a fragment
    // shader, so it must be stated. The bodies are already in/out style, which
    // GLSL 1.30 and ES 3.00 both accept, so only the preamble differs.
    const bool es = context() && context()->isOpenGLES();
    const QString head = es
        ? QStringLiteral("#version 300 es\nprecision highp float;\n")
        : QStringLiteral("#version 130\n");
    prog_->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                   head + QLatin1String(kVertBody));
    prog_->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                   head + QLatin1String(kFragBody));
    prog_->bindAttributeLocation("aPos", 0);
    prog_->bindAttributeLocation("aNrm", 1);
    prog_->bindAttributeLocation("aCol", 2);
    // A viewport with nothing in it is the one failure this widget used to
    // report by staying empty. Keep the reason and draw it instead.
    if (!prog_->link()) {
        const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char* v = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        shaderErr_ = QStringLiteral("the graphics driver refused the shaders\n%1\n%2\n\n%3")
                         .arg(QString::fromLatin1(r ? r : "?"),
                              QString::fromLatin1(v ? v : "?"),
                              prog_->log().trimmed());
    }
    vboPos_.create(); vboNrm_.create(); vboCol_.create();
    glReady_ = true;
}

QMatrix4x4 GridGLWidget::viewMatrix() const
{
    const QVector3D center = 0.5f * (bboxMin_ + bboxMax_);
    const QVector3D scaledCenter(center.x(), center.y(), center.z() * float(zscale_));

    QMatrix4x4 view;
    view.translate(0, 0, -dist_);
    view.rotate(pitch_, 1, 0, 0);
    view.rotate(yaw_,   0, 0, 1);
    view.translate(-scaledCenter - panOffset_);
    return view;
}

QMatrix4x4 GridGLWidget::mvp() const
{
    const QMatrix4x4 view = viewMatrix();
    QMatrix4x4 proj;
    const float aspect = height() > 0 ? float(width()) / float(height()) : 1.0f;
    proj.perspective(40.0f, aspect, dist_ * 0.01f, dist_ * 20.0f);
    return proj * view;
}

void GridGLWidget::paintGL()
{
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!prog_ || vertCount_ == 0) return;

    if (meshDirty_) {
        vboPos_.bind(); vboPos_.allocate(pos_.data(), int(pos_.size() * sizeof(float)));
        vboNrm_.bind(); vboNrm_.allocate(nrm_.data(), int(nrm_.size() * sizeof(float)));
        meshDirty_ = false;
    }
    if (colorDirty_) {
        vboCol_.bind(); vboCol_.allocate(col_.data(), int(col_.size() * sizeof(float)));
        colorDirty_ = false;
    }

    const QMatrix4x4 m = mvp();
    // Normals in EYE space, so the light can sit with the camera. Built from
    // view*model rather than model alone: the z-scale has to be in it (it is
    // what makes a squashed cell's sides face where they do), and the camera
    // has to be in it, or the light stays welded to the grid - see kVert.
    QMatrix4x4 model; model.scale(1.f, 1.f, float(zscale_));
    const QMatrix3x3 nrmM = (viewMatrix() * model).normalMatrix();

    prog_->bind();
    prog_->setUniformValue("uMvp", m);
    prog_->setUniformValue("uNrm", nrmM);
    prog_->setUniformValue("uZScale", float(zscale_));
    // Shading off reuses the same unlit path the wells are drawn with, so a
    // face comes out at exactly the colour the bar gives its value.
    prog_->setUniformValue("uFlat", !shaded_);

    prog_->enableAttributeArray(0);
    vboPos_.bind(); prog_->setAttributeBuffer(0, GL_FLOAT, 0, 3);
    prog_->enableAttributeArray(1);
    vboNrm_.bind(); prog_->setAttributeBuffer(1, GL_FLOAT, 0, 3);
    prog_->enableAttributeArray(2);
    vboCol_.bind(); prog_->setAttributeBuffer(2, GL_FLOAT, 0, 3);
    glDrawArrays(GL_TRIANGLES, 0, vertCount_);
    prog_->disableAttributeArray(0);
    prog_->disableAttributeArray(1);
    prog_->disableAttributeArray(2);

    // wells: immediate small buffers each frame (a handful of polylines)
    if (!wells_.isEmpty()) {
        prog_->setUniformValue("uFlat", true);
        glLineWidth(3.0f);
        glDisable(GL_DEPTH_TEST);       // draw on top so paths stay visible
        prog_->disableAttributeArray(1);
        prog_->disableAttributeArray(2);
        prog_->setAttributeValue(1, QVector3D(0, 0, 1));
        vboPos_.release();
        for (const WellPath& w : wells_) {
            if (w.points.size() < 2) continue;
            prog_->setAttributeValue(2, QVector3D(float(w.color.redF()),
                                                  float(w.color.greenF()),
                                                  float(w.color.blueF())));
            std::vector<float> tmp;
            tmp.reserve(size_t(w.points.size()) * 3);
            for (const auto& p : w.points) { tmp.push_back(p.x()); tmp.push_back(p.y()); tmp.push_back(p.z()); }
            prog_->enableAttributeArray(0);
            prog_->setAttributeArray(0, GL_FLOAT, tmp.data(), 3);
            glDrawArrays(GL_LINE_STRIP, 0, w.points.size());
            prog_->disableAttributeArray(0);
        }
        glEnable(GL_DEPTH_TEST);
    }
    prog_->release();

    // ---- 2D overlay: legend, step text, well names -------------------------
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    // Said in the viewport, because that is where the grid is missing from.
    if (!shaderErr_.isEmpty()) {
        p.setPen(QColor(0xb3, 0x1f, 0x1f));
        p.drawText(QRect(16, 16, std::max(200, width() - 32), height() - 32),
                   Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, shaderErr_);
        return;
    }

    if (cellCount_ > 0 && shownCells_ == 0) {
        p.setPen(QColor(0x55, 0x5b, 0x61));
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("no cell is in the chosen regions"));
    }

    if (hasValues_ && categorical_) {
        // A swatch per region, with how many of the cells drawn it holds - the
        // distribution, which a colour on its own does not give.
        const int x = 14, y = 40, row = 16;
        p.setPen(Qt::black);
        p.drawText(x, y - 10, legendTitle_);
        const int fit  = std::max(1, (height() - y - 60) / row);
        const int n    = int(regionLegend_.size());
        const int rows = n > fit ? fit - 1 : n;
        for (int i = 0; i < rows; ++i) {
            const auto& e = regionLegend_[i];
            const QRect sw(x, y + i * row, 18, 12);
            p.fillRect(sw, regionColor(e.region));
            p.drawRect(sw);
            p.drawText(x + 24, y + i * row + 11,
                       QStringLiteral("%1   %2 cell%3").arg(e.region).arg(e.cells)
                           .arg(e.cells == 1 ? QString() : QStringLiteral("s")));
        }
        if (rows < n)
            p.drawText(x + 24, y + rows * row + 11,
                       QStringLiteral("+ %1 more").arg(n - rows));
    } else if (hasValues_) {
        const int x = 14, y = 40, w = 18, h = 180;
        QLinearGradient gr(x, y + h, x, y);
        for (int i = 0; i <= 10; ++i) {
            float r, g, b; jet(i / 10.0f, r, g, b);
            gr.setColorAt(i / 10.0, QColor::fromRgbF(r, g, b));
        }
        p.fillRect(x, y, w, h, gr);
        p.setPen(Qt::black);
        p.drawRect(x, y, w, h);
        // A constant field has no range to label, and printing the same number
        // at both ends reads as a bug in the legend rather than as a fact about
        // the data. Say the value once, and say that it does not vary.
        if (vmax_ > vmin_) {
            p.drawText(x + w + 6, y + 12, QString::number(vmax_, 'g', 5));
            p.drawText(x + w + 6, y + h,  QString::number(vmin_, 'g', 5));
        } else {
            p.drawText(x + w + 6, y + h / 2,
                       QStringLiteral("%1 (uniform)").arg(vmax_, 0, 'g', 5));
        }
        p.drawText(x, y - 10, rangeNote_.isEmpty()
                                  ? legendTitle_
                                  : QStringLiteral("%1  (%2)").arg(legendTitle_, rangeNote_));
    }
    if (!stepText_.isEmpty()) {
        p.setPen(Qt::black);
        p.drawText(14, height() - 14, stepText_);
    }

    // ---- well type legend (top right) --------------------------------------
    if (!wells_.isEmpty()) {
        struct { QColor c; const char* t; } entries[] = {
            { QColor(0x1e, 0x8a, 0x3c), "producer" },
            { QColor(0x1f, 0x4f, 0xc0), "water inj" },
            { QColor(0xc0, 0x29, 0x1e), "gas inj" },
        };
        int ly = 22;
        for (const auto& e : entries) {
            p.setPen(QPen(e.c, 3));
            p.drawLine(width() - 92, ly - 4, width() - 72, ly - 4);
            p.setPen(Qt::black);
            p.drawText(width() - 66, ly, QLatin1String(e.t));
            ly += 17;
        }
    }

    // ---- orientation gizmo (bottom right): X east, Y north, Z depth -------
    if (vertCount_ > 0) {
        QMatrix4x4 rot;
        rot.rotate(pitch_, 1, 0, 0);
        rot.rotate(yaw_,   0, 0, 1);
        const struct { QVector3D axis; QColor color; const char* name; } axes[] = {
            { { 1, 0, 0 },  QColor(0xc0, 0x39, 0x2b), "X" },
            { { 0, 1, 0 },  QColor(0x27, 0x8a, 0x3c), "Y" },
            { { 0, 0, -1 }, QColor(0x2b, 0x50, 0xc0), "Z" },   // depth (down)
        };
        const QPointF origin(width() - 58.0, height() - 46.0);
        const float L = 30.0f;
        QFont f = p.font(); f.setBold(true); p.setFont(f);
        for (const auto& a : axes) {
            const QVector3D v = rot.map(a.axis);
            const QPointF tip(origin.x() + v.x() * L, origin.y() - v.y() * L);
            QPen pen(a.color, 2);
            p.setPen(pen);
            p.drawLine(origin, tip);
            // nudge the label past the tip so it does not sit on the line
            const QPointF lbl(origin.x() + v.x() * (L + 11) - 4,
                              origin.y() - v.y() * (L + 11) + 4);
            p.drawText(lbl, QLatin1String(a.name));
        }
    }
    if (!wells_.isEmpty()) {
        // Project each well's top point with the same transform the shader
        // uses (z is scaled by the vertical exaggeration before the MVP).
        const QMatrix4x4 m2 = mvp();
        for (const WellPath& w : wells_) {
            if (w.points.isEmpty()) continue;
            const QVector3D& t = w.points.front();
            const QVector4D clip =
                m2 * QVector4D(t.x(), t.y(), t.z() * float(zscale_), 1.0f);
            if (clip.w() <= 0) continue;
            const float sx = (clip.x() / clip.w() * 0.5f + 0.5f) * width();
            const float sy = (1.0f - (clip.y() / clip.w() * 0.5f + 0.5f)) * height();
            p.setPen(w.color.darker(120));
            p.drawText(QPointF(sx + 4, sy - 4), w.name);
        }
    }
}

void GridGLWidget::resizeGL(int, int) {}

void GridGLWidget::mousePressEvent(QMouseEvent* ev) { lastMouse_ = ev->pos(); }

void GridGLWidget::mouseMoveEvent(QMouseEvent* ev)
{
    const QPoint d = ev->pos() - lastMouse_;
    lastMouse_ = ev->pos();
    if (ev->buttons() & Qt::LeftButton) {
        yaw_   += d.x() * 0.4f;
        pitch_ += d.y() * 0.4f;
        // Full orbit, no clamp at the poles: keep dragging past the side view
        // to end up under the model and look at the base of the reservoir.
        // The camera is a rotation composition (no lookAt), so there is no
        // up-vector degeneracy to avoid - only the numbers need wrapping.
        if (pitch_ >  180.f) pitch_ -= 360.f;
        if (pitch_ < -180.f) pitch_ += 360.f;
    } else if (ev->buttons() & (Qt::MiddleButton | Qt::RightButton)) {
        // pan in view plane, scaled with distance
        const float s = dist_ * 0.0015f;
        QMatrix4x4 rot;
        rot.rotate(-yaw_, 0, 0, 1);
        rot.rotate(-pitch_, 1, 0, 0);
        panOffset_ += rot.map(QVector3D(-d.x() * s, d.y() * s, 0));
    }
    update();
}

void GridGLWidget::wheelEvent(QWheelEvent* ev)
{
    const float f = std::pow(1.0015f, -ev->angleDelta().y());
    dist_ = std::clamp(dist_ * f, 1.0f, 1e9f);
    update();
}

// ===========================================================================
// Viewer3DWidget
// ===========================================================================
Viewer3DWidget::Viewer3DWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* top = new QVBoxLayout(this);

    // --- case + property row ------------------------------------------------
    // Wrapping (see FlowLayout): as one fixed line this row alone asked for
    // more width than a laptop panel has.
    {
        FlowLayout* row = nullptr;
        top->addWidget(FlowLayout::host(&row));
        row->addWidget(new QLabel(QStringLiteral("Case:")));
        caseBox_ = new QComboBox;
        caseBox_->setMinimumWidth(240);
        row->addWidget(caseBox_);
        auto* bopen = new QPushButton(QStringLiteral("Open EGRID..."));
        bopen->setToolTip(QStringLiteral(
            "open one or more grids; each joins the case list shared with the "
            "Summary Plots and Compare tabs"));
        row->addWidget(bopen);
        auto* bremove = new QPushButton(QStringLiteral("Remove"));
        bremove->setToolTip(QStringLiteral(
            "drop the selected case from the list - the run's files are left alone"));
        row->addWidget(bremove);

        staticSel_ = new QRadioButton(QStringLiteral("static:"));
        staticBox_ = new QComboBox; staticBox_->setMinimumWidth(110);
        dynSel_    = new QRadioButton(QStringLiteral("dynamic:"));
        dynBox_    = new QComboBox; dynBox_->setMinimumWidth(110);
        staticSel_->setChecked(true);
        row->addWidget(staticSel_); row->addWidget(staticBox_);
        row->addWidget(dynSel_);    row->addWidget(dynBox_);

        // Draw only the cells of some regions, whatever is being coloured: the
        // saturation in one equilibration region, the pressure in one PVT
        // region. The region arrays are the INIT file's integer cell arrays.
        regionBox_ = new QComboBox;
        regionBox_->addItem(QStringLiteral("all cells"));
        regionBox_->setToolTip(QStringLiteral(
            "draw only the cells in some regions of this array; the property "
            "shown is coloured over those cells alone"));
        regionNums_ = new QLineEdit;
        regionNums_->setMaximumWidth(120);
        regionNums_->setEnabled(false);
        regionNums_->setToolTip(QStringLiteral(
            "the region numbers to draw, e.g. \"2\" or \"1, 3-5\"; empty draws "
            "every region. Applied on Enter."));
        regionInfo_ = new QLabel;
        regionInfo_->setStyleSheet(QStringLiteral("color:#555b61;"));
        row->addWidget(new QLabel(QStringLiteral("regions:")));
        row->addWidget(regionBox_);
        row->addWidget(regionNums_);
        row->addWidget(regionInfo_);

        // A bar that refits every step makes a run look busiest where it is
        // quietest: 138.47 to 138.48 fills the whole scale.
        fixRange_ = new QCheckBox(QStringLiteral("fix range"));
        fixRange_->setToolTip(QStringLiteral(
            "hold the colour bar still while stepping through the run: by "
            "default at the property's min and max over the whole simulation, "
            "or at the ends typed beside it"));
        rangeMin_ = new QLineEdit;
        rangeMax_ = new QLineEdit;
        for (auto* e : { rangeMin_, rangeMax_ }) {
            e->setMaximumWidth(90);
            e->setEnabled(false);
            e->setToolTip(QStringLiteral(
                "with fix range on: this end of the colour bar. Empty takes the "
                "whole simulation's value, shown greyed. Remembered per property."));
            connect(e, &QLineEdit::editingFinished, this, [this] {
                const QString name = dynSel_->isChecked() ? dynBox_->currentText()
                                                          : staticBox_->currentText();
                if (name.isEmpty()) return;
                rangeTexts_[name] = { rangeMin_->text().trimmed(), rangeMax_->text().trimmed() };
                showProperty();
            });
        }
        row->addWidget(fixRange_);
        row->addWidget(rangeMin_);
        row->addWidget(new QLabel(QStringLiteral("to")));
        row->addWidget(rangeMax_);

        wellsChk_ = new QCheckBox(QStringLiteral("wells"));
        wellsChk_->setChecked(true);
        row->addWidget(wellsChk_);

        // Shading is a trade, not an improvement: it multiplies the cell's
        // colour, so a shaded face no longer reads true against the colour
        // bar. Worth it to see the shape of a folded or faulted surface,
        // not worth it when the question is what value a cell holds - so it
        // is a switch rather than a decision made for the user.
        shadingChk_ = new QCheckBox(QStringLiteral("shading"));
        shadingChk_->setChecked(true);
        shadingChk_->setToolTip(QStringLiteral(
            "light the grid so its shape reads.\n\n"
            "Off, every face is drawn at exactly the colour the bar gives its "
            "value - which is what you want when reading values off the "
            "screen, at the cost of the relief that separates a fault from a "
            "fold."));
        row->addWidget(shadingChk_);

        // Following a run costs a re-index of the restart file every few
        // seconds, and for a big model that is not free. On by default, because
        // a view that quietly stops at the step it opened on is the more
        // surprising of the two; off for anyone who would rather spend the
        // machine on the simulation.
        autoRef_ = new QCheckBox(QStringLiteral("auto-refresh (10 s)"));
        autoRef_->setChecked(true);
        autoRef_->setToolTip(QStringLiteral(
            "while a run is writing this case, re-read its restart file so new "
            "report steps appear as they are produced. Does nothing when no run "
            "is going: the results are then already whole.\n\n"
            "Each refresh re-indexes the restart file, which on a large model "
            "takes a moment - turn this off to leave the machine to the run. "
            "The view is reloaded in full when the run finishes either way."));
        row->addWidget(autoRef_);

        row->addWidget(new QLabel(QStringLiteral("Z x")));
        zscale_ = new QDoubleSpinBox;
        zscale_->setRange(0.1, 50.0);
        zscale_->setValue(3.0);
        zscale_->setSingleStep(0.5);
        row->addWidget(zscale_);
        // Standard viewpoints - dragging can reach all of them (the orbit is
        // unrestricted), this is just the quick way to the useful ones.
        auto* viewBox = new QComboBox;
        viewBox->addItem(QStringLiteral("View..."));
        viewBox->addItem(QStringLiteral("Home"));
        viewBox->addItem(QStringLiteral("Top (map)"));
        viewBox->addItem(QStringLiteral("Bottom (base)"));
        viewBox->addItem(QStringLiteral("Side"));
        viewBox->setToolTip(QStringLiteral(
            "jump to a standard viewpoint, keeping the current zoom and pan"));
        row->addWidget(viewBox);
        connect(viewBox, &QComboBox::currentIndexChanged, this, [this, viewBox](int i) {
            switch (i) {
                case 1: gl_->setStandardView(GridGLWidget::StdView::Home);   break;
                case 2: gl_->setStandardView(GridGLWidget::StdView::Top);    break;
                case 3: gl_->setStandardView(GridGLWidget::StdView::Bottom); break;
                case 4: gl_->setStandardView(GridGLWidget::StdView::Side);   break;
                default: return;
            }
            viewBox->setCurrentIndex(0);   // a menu, not a persistent state
        });
        auto* bview = new QPushButton(QStringLiteral("Reset view"));
        bview->setToolTip(QStringLiteral("back to the framed default view (also resets zoom and pan)"));
        row->addWidget(bview);
        connect(bview, &QPushButton::clicked, this, [this] { gl_->resetCamera(); });

        connect(bopen, &QPushButton::clicked, this, [this] {
            const int cur = caseBox_->currentIndex();
            const QStringList files = QFileDialog::getOpenFileNames(
                this, QStringLiteral("Open grid files"),
                flowgui::startDir(QStringLiteral("grid"),
                                  cur >= 0 && cur < cases_.size() ? cases_[cur].egrid
                                                                  : QString()),
                QStringLiteral("Eclipse grid (*.EGRID);;All files (*)"));
            if (files.isEmpty()) return;
            flowgui::rememberDir(QStringLiteral("grid"), files.first());
            // Handed to whoever owns the case list rather than added here: two
            // grids with the same file name in different folders would both
            // come out called the same thing, and this tab has no way to tell
            // them apart. They come back through addCase(), tagged.
            for (const QString& f : files) {
                QString base = f; base.chop(6);   // ".EGRID"
                emit openCaseRequested(base + QStringLiteral(".SMSPEC"));
            }
            QString first = files.first(); first.chop(6);
            for (int i = 0; i < caseBox_->count(); ++i)
                if (flowgui::sameCasePath(caseBox_->itemData(i).toString(),
                                          first + QStringLiteral(".EGRID"))) {
                    caseBox_->setCurrentIndex(i);
                    break;
                }
        });
        connect(bremove, &QPushButton::clicked, this,
                [this] { removeCaseAt(caseBox_->currentIndex()); });
        connect(caseBox_, &QComboBox::currentIndexChanged, this,
                [this](int i) { openCase(i); syncCaseTip(); });
        auto onProp = [this] { showProperty(); };
        connect(staticBox_, &QComboBox::currentIndexChanged, this, onProp);
        connect(dynBox_,    &QComboBox::currentIndexChanged, this, onProp);
        connect(staticSel_, &QRadioButton::toggled, this, onProp);
        connect(wellsChk_, &QCheckBox::toggled, this, [this](bool) { showWells(); });
        // showProperty() after: a fixed range is taken over the cells drawn.
        connect(regionBox_, &QComboBox::currentIndexChanged, this, [this](int i) {
            regionChoice_ = i > 0 ? regionBox_->currentText() : QString();
            applyRegionFilter();
            showProperty();
        });
        connect(regionNums_, &QLineEdit::editingFinished, this, [this] {
            applyRegionFilter();
            showProperty();
        });
        connect(fixRange_, &QCheckBox::toggled, this, [this](bool) { showProperty(); });
        connect(autoRef_, &QCheckBox::toggled, this, [this](bool on) {
            if (!followTimer_) return;
            // Only meaningful while a run is writing the case; setRunningCase()
            // starts the timer when one begins.
            if (on && !runningEgrid_.isEmpty()) { followTimer_->start(); followRunningCase(); }
            else                                 followTimer_->stop();
        });
        connect(shadingChk_, &QCheckBox::toggled, this,
                [this](bool on) { if (gl_) gl_->setShaded(on); });
        connect(zscale_, &QDoubleSpinBox::valueChanged, this,
                [this](double v) { gl_->setZScale(v); });
    }

    // --- animation row --------------------------------------------------------
    {
        auto* row = new QHBoxLayout;
        auto* rewindBtn = new QPushButton(QStringLiteral("|<< Rewind"));
        rewindBtn->setToolTip(QStringLiteral("stop and go back to the first report step"));
        playBtn_ = new QPushButton(QStringLiteral("Play"));
        playBtn_->setCheckable(true);
        stepSlider_ = new QSlider(Qt::Horizontal);
        stepSlider_->setEnabled(false);
        // click the bar to jump to that report step (see AbsoluteSeekStyle);
        // the style is parented to the slider, which owns it from here on.
        {
            auto* seek = new AbsoluteSeekStyle;
            seek->setParent(stepSlider_);
            stepSlider_->setStyle(seek);
        }
        stepSlider_->setToolTip(QStringLiteral(
            "drag, or click anywhere on the bar, to show that report step"));
        stepLabel_ = new QLabel(QStringLiteral("-"));
        stepLabel_->setMinimumWidth(160);
        row->addWidget(rewindBtn);
        row->addWidget(playBtn_);
        row->addWidget(stepSlider_, 1);
        row->addWidget(stepLabel_);
        top->addLayout(row);

        // Picks up report steps while a run writes them. 10 s, matching the
        // summary tab: each tick re-indexes the restart file, so a longer
        // interval both costs the run less and gives a half-written file more
        // room to be finished before anyone looks at it. Nobody watches a
        // reservoir simulation step by step.
        followTimer_ = new QTimer(this);
        followTimer_->setInterval(10000);
        connect(followTimer_, &QTimer::timeout, this, [this] { followRunningCase(); });

        playTimer_ = new QTimer(this);
        playTimer_->setInterval(600);
        connect(playTimer_, &QTimer::timeout, this, [this] {
            if (stepSlider_->maximum() <= 0) return;
            int v = stepSlider_->value() + 1;
            if (v > stepSlider_->maximum()) v = 0;
            stepSlider_->setValue(v);
        });
        connect(playBtn_, &QPushButton::toggled, this, [this](bool on) {
            dynSel_->setChecked(true);
            playBtn_->setText(on ? QStringLiteral("Pause") : QStringLiteral("Play"));
            if (on) {
                // pressing Play at the end starts over from the beginning
                if (stepSlider_->value() >= stepSlider_->maximum())
                    stepSlider_->setValue(0);
                playTimer_->start();
            } else {
                playTimer_->stop();
            }
        });
        connect(rewindBtn, &QPushButton::clicked, this, [this] {
            playBtn_->setChecked(false);          // stops the timer, text -> Play
            stepSlider_->setValue(0);
        });
        // Moving the bar is a request to see that moment, but stepChanged()
        // only draws when a dynamic property is on show - so with a static one
        // selected the bar would move and the view never change. Switch over
        // the way Play does. Only user input gets here (sliderPressed and
        // actionTriggered are not emitted by setValue()), so restoring a
        // session or opening a case still keeps the static choice it was given.
        auto toDynamic = [this] {
            if (dynSel_->isChecked() || dynBox_->count() == 0) return;
            dynSel_->setChecked(true);   // toggled -> showProperty() -> stepChanged()
        };
        connect(stepSlider_, &QSlider::sliderPressed, this, toDynamic);
        connect(stepSlider_, &QSlider::actionTriggered, this,
                [toDynamic](int) { toDynamic(); });
        connect(stepSlider_, &QSlider::valueChanged, this,
                [this](int v) { stepChanged(v); });
    }

    gl_ = new GridGLWidget;
    top->addWidget(gl_, 1);

    status_ = new QLabel(QStringLiteral(
        "run a job or open an EGRID - left-drag orbits, right-drag pans, wheel zooms"));
    top->addWidget(status_);
}

Viewer3DWidget::~Viewer3DWidget() = default;

void Viewer3DWidget::setStatus(const QString& s) { status_->setText(s); }

// A case is handed to this tab as its SMSPEC path and the grid files are its
// siblings. Normalized (see CasePath.h) so that one run is one entry however
// the path was spelled - a custom output directory arrives in native
// separators, a file dialog or a project file in '/'.
static QString caseBaseOf(const QString& smspecPath)
{
    QString base = flowgui::normalizeCasePath(smspecPath);
    if (base.endsWith(QStringLiteral(".SMSPEC"), Qt::CaseInsensitive)) base.chop(7);
    return base;
}

void Viewer3DWidget::addCase(const QString& label, const QString& smspecPath)
{
    const QString base = caseBaseOf(smspecPath);
    const CaseFiles cf{ label,
                        base + QStringLiteral(".EGRID"),
                        base + QStringLiteral(".INIT"),
                        base + QStringLiteral(".UNRST") };
    for (const auto& c : cases_)
        if (flowgui::sameCasePath(c.egrid, cf.egrid)) return;
    cases_.push_back(cf);
    caseBox_->addItem(label, cf.egrid);
    // The path on the tooltip: two runs of one deck can only differ by folder,
    // and the label is a tag at best.
    caseBox_->setItemData(caseBox_->count() - 1, cf.egrid, Qt::ToolTipRole);
    if (caseBox_->count() == 1) caseBox_->setCurrentIndex(0);
    syncCaseTip();
}

// A closed combo shows its OWN tooltip, never the current item's, so the path
// has to be copied onto the widget every time the selection moves. Without
// this, hovering the box says nothing until you open it.
void Viewer3DWidget::syncCaseTip()
{
    const int i = caseBox_->currentIndex();
    caseBox_->setToolTip(
        i >= 0 && i < cases_.size()
            ? QDir::toNativeSeparators(cases_[i].egrid)
            : QStringLiteral("the case being viewed"));
}

void Viewer3DWidget::renameCase(const QString& smspecPath, const QString& label)
{
    const QString egrid = caseBaseOf(smspecPath) + QStringLiteral(".EGRID");
    for (int i = 0; i < cases_.size(); ++i)
        if (flowgui::sameCasePath(cases_[i].egrid, egrid)) {
            cases_[i].label = label;
            if (i < caseBox_->count()) caseBox_->setItemText(i, label);
            return;
        }
}

void Viewer3DWidget::removeCase(const QString& smspecPath)
{
    const QString egrid = caseBaseOf(smspecPath) + QStringLiteral(".EGRID");
    for (int i = 0; i < cases_.size(); ++i)
        if (flowgui::sameCasePath(cases_[i].egrid, egrid)) { removeCaseAt(i); return; }
}

void Viewer3DWidget::reorderCases(const QStringList& smspecPaths)
{
    if (cases_.isEmpty()) return;
    const QString shown = caseBox_->currentIndex() >= 0
                              ? cases_[caseBox_->currentIndex()].egrid : QString();

    QVector<CaseFiles> sorted;
    sorted.reserve(cases_.size());
    QVector<bool> taken(cases_.size(), false);
    for (const QString& p : smspecPaths) {
        const QString egrid = caseBaseOf(p) + QStringLiteral(".EGRID");
        for (int i = 0; i < cases_.size(); ++i)
            if (!taken[i] && flowgui::sameCasePath(cases_[i].egrid, egrid)) {
                sorted.append(cases_[i]);
                taken[i] = true;
                break;
            }
    }
    for (int i = 0; i < cases_.size(); ++i)      // anything not mentioned
        if (!taken[i]) sorted.append(cases_[i]);
    bool changed = false;                        // CaseFiles has no operator==
    for (int i = 0; i < sorted.size() && !changed; ++i)
        changed = sorted[i].egrid != cases_[i].egrid;
    if (!changed) return;
    cases_ = sorted;

    // Rebuild the box without reopening anything: the case on screen stays on
    // screen, at its new place in the list.
    const QSignalBlocker block(caseBox_);
    caseBox_->clear();
    int current = -1;
    for (int i = 0; i < cases_.size(); ++i) {
        caseBox_->addItem(cases_[i].label, cases_[i].egrid);
        caseBox_->setItemData(i, cases_[i].egrid, Qt::ToolTipRole);
        if (!shown.isEmpty() && flowgui::sameCasePath(cases_[i].egrid, shown)) current = i;
    }
    caseBox_->setCurrentIndex(current);
    syncCaseTip();
}

// The list is what grows over a session - every job that runs adds to it - so
// dropping an entry only forgets it here; nothing on disk is touched.
void Viewer3DWidget::removeCaseAt(int idx)
{
    if (idx < 0 || idx >= cases_.size()) {
        setStatus(QStringLiteral("no case selected to remove"));
        return;
    }
    const bool wasShown = (idx == caseBox_->currentIndex());
    const QString gone  = cases_[idx].label;

    cases_.remove(idx);
    {   // choose the replacement below rather than let removeItem() pick one
        const QSignalBlocker block(caseBox_);
        caseBox_->removeItem(idx);
    }
    // Indices shift down over the hole; a case still waiting to be opened
    // moves with them, or goes away with the entry it referred to.
    if (pendingCase_ == idx)     pendingCase_ = -1;
    else if (pendingCase_ > idx) --pendingCase_;

    if (cases_.isEmpty()) {
        clearView();
        setStatus(QStringLiteral("removed %1 - no cases left").arg(gone));
        return;
    }
    // Only the shown case needs a successor: removing any other one leaves the
    // combo on the same entry (its index already shifted with cases_).
    if (wasShown) {
        const int next = std::min(idx, int(cases_.size()) - 1);
        {
            const QSignalBlocker block(caseBox_);
            caseBox_->setCurrentIndex(next);
        }
        openCase(next);
    }
    setStatus(QStringLiteral("removed %1").arg(gone));
}

// Nothing left to show: drop the readers and blank the canvas, so the tab does
// not keep drawing a case that is no longer in the list.
void Viewer3DWidget::clearView()
{
    if (playBtn_->isChecked()) playBtn_->setChecked(false);   // also stops the timer
    openCase(-1);            // resets readers, property boxes, slider and wells
    gl_->setMesh({}, {}, 0);
    pendingCase_ = -1;
    havePending_ = false;
}

void Viewer3DWidget::caseFinished(const QString& smspecPath)
{
    const QString egrid = caseBaseOf(smspecPath) + QStringLiteral(".EGRID");
    const int idx = caseBox_->currentIndex();
    // The run is over, so a read that failed against a half-written file
    // deserves a fresh attempt rather than being held to its one retry.
    rstRetryIdx_ = -1;
    if (idx >= 0 && idx < cases_.size() &&
        flowgui::sameCasePath(cases_[idx].egrid, egrid))
        openCase(idx);
}

void Viewer3DWidget::setRunningCase(const QString& smspecPath)
{
    const QString egrid = smspecPath.isEmpty()
                              ? QString()
                              : caseBaseOf(smspecPath) + QStringLiteral(".EGRID");
    if (egrid == runningEgrid_) return;

    runningEgrid_ = egrid;
    rstRetryIdx_ = -1;

    // Follow the run only while one is going on. Clearing does not reopen the
    // case here - the caller follows a finished job with caseFinished(), and
    // doing it in both places would rebuild the mesh twice.
    if (egrid.isEmpty()) { if (followTimer_) followTimer_->stop(); return; }
    // Tick for as long as the run lasts, whichever case is on screen: the user
    // may switch to the running one at any point, and followRunningCase() is
    // the thing that decides whether there is anything to do. Unless the user
    // has turned following off, in which case the view waits for the run to end.
    if (followTimer_ && (!autoRef_ || autoRef_->isChecked())) followTimer_->start();

    const int idx = caseBox_ ? caseBox_->currentIndex() : -1;
    if (idx < 0 || idx >= cases_.size()) return;
    if (!flowgui::sameCasePath(cases_[idx].egrid, egrid)) return;

    // A run has started on the case being shown, and it is about to overwrite
    // the files this tab has open. Reopen it now, unconditionally: whatever is
    // on screen belongs to the PREVIOUS run, and leaving it up while a new one
    // writes is worse than showing nothing - it looks like the new results.
    // openCase() drops every reader and reloads whatever exists so far, which
    // early in a run may be nothing at all.
    openCase(idx);
    if (followTimer_) followTimer_->start();
}

void Viewer3DWidget::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    // A case picked while the tab was hidden (a restored session, or a job
    // that finished on another tab) is opened now, when there is finally
    // something to draw.
    const int idx = pendingCase_ >= 0 ? pendingCase_ : caseBox_->currentIndex();
    pendingCase_ = -1;
    if (!grid_ && idx >= 0 && idx < cases_.size() &&
        QFileInfo::exists(cases_[idx].egrid))
        openCase(idx);
}

// ---------------------------------------------------------------------------
QJsonObject Viewer3DWidget::uiState() const
{
    QJsonObject o;
    const int idx = caseBox_ ? caseBox_->currentIndex() : -1;
    if (idx >= 0 && idx < cases_.size())
        o[QStringLiteral("case")] = QDir::fromNativeSeparators(cases_[idx].egrid);
    const bool dyn = dynSel_ && dynSel_->isChecked();
    o[QStringLiteral("dynamic")]  = dyn;
    if (auto* box = dyn ? dynBox_ : staticBox_)
        o[QStringLiteral("property")] = box->currentText();
    if (wellsChk_)   o[QStringLiteral("wells")]  = wellsChk_->isChecked();
    if (shadingChk_) o[QStringLiteral("shading")] = shadingChk_->isChecked();
    if (autoRef_)    o[QStringLiteral("autoRefresh")] = autoRef_->isChecked();
    if (zscale_)     o[QStringLiteral("zscale")] = zscale_->value();
    o[QStringLiteral("regionFilter")] = regionChoice_;
    if (regionNums_) o[QStringLiteral("regionNumbers")] = regionNums_->text();
    if (fixRange_)   o[QStringLiteral("fixRange")] = fixRange_->isChecked();
    QJsonObject ranges;
    for (auto it = rangeTexts_.constBegin(); it != rangeTexts_.constEnd(); ++it)
        if (!it->first.isEmpty() || !it->second.isEmpty())
            ranges[it.key()] = QJsonArray{ it->first, it->second };
    o[QStringLiteral("ranges")] = ranges;
    if (stepSlider_ && stepSlider_->isEnabled())
        o[QStringLiteral("step")] = stepSlider_->value();
    return o;
}

void Viewer3DWidget::restoreUiState(const QJsonObject& state)
{
    if (state.isEmpty()) return;
    if (wellsChk_ && state.contains(QStringLiteral("wells")))
        wellsChk_->setChecked(state.value(QStringLiteral("wells")).toBool(true));
    if (shadingChk_ && state.contains(QStringLiteral("shading")))
        shadingChk_->setChecked(state.value(QStringLiteral("shading")).toBool(true));
    if (zscale_ && state.contains(QStringLiteral("zscale")))
        zscale_->setValue(state.value(QStringLiteral("zscale")).toDouble(3.0));
    if (autoRef_ && state.contains(QStringLiteral("autoRefresh")))
        autoRef_->setChecked(state.value(QStringLiteral("autoRefresh")).toBool(true));
    // Applied when a case is opened and its region arrays are known.
    regionChoice_ = state.value(QStringLiteral("regionFilter")).toString();
    if (regionNums_) regionNums_->setText(state.value(QStringLiteral("regionNumbers")).toString());
    if (fixRange_) {
        const QSignalBlocker block(fixRange_);
        fixRange_->setChecked(state.value(QStringLiteral("fixRange")).toBool(false));
    }
    rangeTexts_.clear();
    const QJsonObject ranges = state.value(QStringLiteral("ranges")).toObject();
    for (auto it = ranges.constBegin(); it != ranges.constEnd(); ++it) {
        const QJsonArray a = it.value().toArray();
        rangeTexts_[it.key()] = { a.at(0).toString(), a.at(1).toString() };
    }

    // The case list is mirrored from the Summary Plots tab, so by now it holds
    // the restored cases; select ours without opening it (see showEvent).
    // Compared, not spelled: a project file written by an older version holds
    // whatever separators that session happened to use.
    const QString egrid = state.value(QStringLiteral("case")).toString();
    if (!egrid.isEmpty() && caseBox_) {
        for (int i = 0; i < cases_.size(); ++i) {
            if (!flowgui::sameCasePath(cases_[i].egrid, egrid)) continue;
            const QSignalBlocker block(caseBox_);
            caseBox_->setCurrentIndex(i);
            pendingCase_ = i;
            break;
        }
    }
    pendingDynamic_ = state.value(QStringLiteral("dynamic")).toBool(false);
    pendingProp_    = state.value(QStringLiteral("property")).toString();
    pendingStep_    = state.value(QStringLiteral("step")).toInt(-1);
    havePending_    = !pendingProp_.isEmpty() || pendingStep_ >= 0;
}

void Viewer3DWidget::openCase(int idx)
{
    grid_.reset(); init_.reset(); rst_.reset();
    rstBytes_ = 0;                      // the old reader's cache went with it
    lastUnrstSize_ = -1;                // ... and what it had been indexed at
    steps_.clear(); cellGlob_.clear();
    staticBox_->clear(); dynBox_->clear();
    {
        const QSignalBlocker block(regionBox_);
        while (regionBox_->count() > 1) regionBox_->removeItem(1);
    }
    regionInfo_->clear();
    nStatic_ = nRegion_ = 0;
    runRangeKey_.clear();
    stepSlider_->setEnabled(false);
    stepSlider_->setRange(0, 0);
    gl_->setWells({});
    gl_->setStepText(QString());
    if (idx < 0 || idx >= cases_.size()) return;
    // Reading the grid and building the mesh is the expensive part of this
    // tab; with the tab hidden there is nothing to show for it, so hold the
    // case until showEvent asks for it.
    if (!isVisible()) { pendingCase_ = idx; return; }
    const CaseFiles& cf = cases_[idx];

    if (!QFileInfo::exists(cf.egrid)) {
        setStatus(QStringLiteral("no EGRID yet at %1").arg(cf.egrid));
        return;
    }
    // opm-common resolves the file against the working directory, so a
    // deleted one would make every grid unreadable (see GuiPaths.h).
    flowgui::ensureWorkingDirectory();
    try {
        grid_ = std::make_unique<Opm::EclIO::EGrid>(cf.egrid.toStdString());
        buildMesh();
    } catch (const std::exception& e) {
        setStatus(QStringLiteral("failed to load grid: %1").arg(
            QString::fromLocal8Bit(e.what())));
        grid_.reset();
        return;
    }
    try { if (QFileInfo::exists(cf.init))  init_ = std::make_unique<Opm::EclIO::EInit>(cf.init.toStdString()); }
    catch (...) { init_.reset(); }

    // What the status line says about the restart file, when there is anything
    // worth saying: it is either being written, or it could not be read.
    QString rstNote;
    try {
        if (QFileInfo::exists(cf.unrst)) {
            rst_ = std::make_unique<Opm::EclIO::ERst>(cf.unrst.toStdString());
            steps_ = rst_->listOfReportStepNumbers();
        }
    } catch (const std::exception& e) {
        // Do not swallow this: dropping every dynamic property with no
        // reason given looks like the case simply has none.
        rst_.reset();
        steps_.clear();
        rstNote = QStringLiteral(" - restart file unreadable (%1)")
                      .arg(QString::fromLocal8Bit(e.what()));
        // A restart file that was being written usually becomes readable a
        // moment later, so try once - and only once per case, so a file
        // that stays bad does not spin.
        if (rstRetryIdx_ != idx) {
            rstRetryIdx_ = idx;
            QTimer::singleShot(1500, this, [this, idx] {
                if (caseBox_->currentIndex() == idx && !rst_) openCase(idx);
            });
        }
    }

    populateProperties();
    applyRegionFilter();
    if (!steps_.empty()) {
        stepSlider_->setEnabled(true);
        stepSlider_->setRange(0, int(steps_.size()) - 1);
        stepSlider_->setValue(int(steps_.size()) - 1);
    }
    // A restored session chose its property before this grid was open; the
    // boxes only exist now, so apply the choice here - before showProperty()
    // below draws whatever the defaults left selected.
    if (havePending_) {
        havePending_ = false;
        if (QRadioButton* r = pendingDynamic_ ? dynSel_ : staticSel_) r->setChecked(true);
        if (QComboBox* b = pendingDynamic_ ? dynBox_ : staticBox_) {
            const int i = b->findText(pendingProp_);
            if (i >= 0) b->setCurrentIndex(i);
        }
        if (pendingStep_ >= 0 && stepSlider_->isEnabled()
            && pendingStep_ <= stepSlider_->maximum())
            stepSlider_->setValue(pendingStep_);
    }
    const auto d = grid_->dimension();
    setStatus(QStringLiteral("%1: %2x%3x%4, %5 active cells, %6 static, %7 region, "
                             "%8 dynamic, %9 report steps%10")
        .arg(cf.label).arg(d[0]).arg(d[1]).arg(d[2]).arg(grid_->activeCells())
        .arg(nStatic_).arg(nRegion_).arg(dynBox_->count()).arg(steps_.size()).arg(rstNote));
    showProperty();
    showWells();
}

void Viewer3DWidget::buildMesh()
{
    const int nact = grid_->activeCells();
    cellGlob_.resize(nact);
    // face corner order for the 8 Eclipse corners (0-3 top, 4-7 bottom;
    // within a layer: 0=(i,j) 1=(i+1,j) 2=(i,j+1) 3=(i+1,j+1))
    static const int F[6][4] = {
        {0, 1, 3, 2},   // top
        {4, 5, 7, 6},   // bottom
        {0, 1, 5, 4},   // front  (j)
        {2, 3, 7, 6},   // back   (j+1)
        {0, 2, 6, 4},   // left   (i)
        {1, 3, 7, 5},   // right  (i+1)
    };

    // first pass: cell centers for the offset (double precision)
    cx_ = cy_ = cz_ = 0.0;
    std::array<double, 8> X, Y, Z;
    for (int a = 0; a < nact; ++a) {
        const auto ijk = grid_->ijk_from_active_index(a);
        cellGlob_[a] = grid_->global_index(ijk[0], ijk[1], ijk[2]);
    }
    grid_->getCellCorners(cellGlob_[0], X, Y, Z);
    cx_ = X[0]; cy_ = Y[0]; cz_ = Z[0];

    std::vector<float> pos, nrm;
    pos.reserve(size_t(nact) * 36 * 3);
    nrm.reserve(size_t(nact) * 36 * 3);

    for (int a = 0; a < nact; ++a) {
        grid_->getCellCorners(cellGlob_[a], X, Y, Z);
        float px[8], py[8], pz[8];
        for (int c = 0; c < 8; ++c) {
            px[c] = float(X[c] - cx_);
            py[c] = float(Y[c] - cy_);
            pz[c] = float(-(Z[c] - cz_));       // depth -> up-negative z
        }
        for (const auto& f : F) {
            // two triangles: 0-1-2, 0-2-3 of the quad
            const int tri[6] = { f[0], f[1], f[2], f[0], f[2], f[3] };
            // face normal from first three points
            const float ux = px[f[1]] - px[f[0]], uy = py[f[1]] - py[f[0]], uz = pz[f[1]] - pz[f[0]];
            const float vx = px[f[2]] - px[f[0]], vy = py[f[2]] - py[f[0]], vz = pz[f[2]] - pz[f[0]];
            float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz2 = ux * vy - uy * vx;
            const float len = std::sqrt(nx * nx + ny * ny + nz2 * nz2);
            if (len > 1e-12f) { nx /= len; ny /= len; nz2 /= len; }
            for (int t = 0; t < 6; ++t) {
                pos.push_back(px[tri[t]]); pos.push_back(py[tri[t]]); pos.push_back(pz[tri[t]]);
                nrm.push_back(nx); nrm.push_back(ny); nrm.push_back(nz2);
            }
        }
    }
    gl_->setMesh(std::move(pos), std::move(nrm), nact);
    gl_->setZScale(zscale_->value());
}

void Viewer3DWidget::populateProperties()
{
    staticBox_->blockSignals(true);
    dynBox_->blockSignals(true);
    if (init_ && grid_) {
        QStringList regions;
        for (const auto& [name, typ, size] : init_->list_arrays()) {
            if (size != grid_->activeCells()) continue;
            if (typ == Opm::EclIO::REAL || typ == Opm::EclIO::DOUB) {
                staticBox_->addItem(QString::fromStdString(name));
                ++nStatic_;
            } else if (typ == Opm::EclIO::INTE) {
                regions << QString::fromStdString(name);
            }
        }
        // Region arrays after the rest, and marked, so showProperty() draws
        // them a colour per region rather than on a scale.
        if (!regions.isEmpty() && nStatic_ > 0) staticBox_->insertSeparator(staticBox_->count());
        for (const QString& r : regions) {
            staticBox_->addItem(r, true);
            const QSignalBlocker block(regionBox_);
            regionBox_->addItem(r);
        }
        nRegion_ = int(regions.size());
        const int poro = staticBox_->findText(QStringLiteral("PORO"));
        if (poro >= 0) staticBox_->setCurrentIndex(poro);
        const QSignalBlocker block(regionBox_);
        const int keep = regionChoice_.isEmpty() ? -1 : regionBox_->findText(regionChoice_);
        regionBox_->setCurrentIndex(std::max(0, keep));
    }
    populateDynamicProperties();
    staticBox_->blockSignals(false);
    dynBox_->blockSignals(false);
}

// The dynamic list on its own, so a run that has just written its first report
// step can fill it without disturbing the static list or the mesh.
void Viewer3DWidget::populateDynamicProperties()
{
    const QSignalBlocker block(dynBox_);
    const QString had = dynBox_->currentText();
    dynBox_->clear();
    if (!rst_ || !grid_ || steps_.empty()) return;

    for (const auto& [name, typ, size] : rst_->listOfRstArrays(steps_.back()))
        if (typ == Opm::EclIO::REAL && size == grid_->activeCells())
            dynBox_->addItem(QString::fromStdString(name));
    // SOIL is usually not stored in the restart; offer it synthesized
    // from the stored saturations (SOIL = 1 - SWAT - SGAS), inserted
    // next to its sibling saturations rather than at the end.
    if (dynBox_->findText(QStringLiteral("SOIL")) < 0 &&
        (rst_->hasArray("SWAT", steps_.back()) ||
         rst_->hasArray("SGAS", steps_.back()))) {
        int at = dynBox_->findText(QStringLiteral("SWAT"));
        if (at < 0) at = dynBox_->findText(QStringLiteral("SGAS"));
        if (at < 0) dynBox_->addItem(QStringLiteral("SOIL"));
        else        dynBox_->insertItem(at + 1, QStringLiteral("SOIL"));
    }
    // Keep what the user was looking at when this is a refresh mid-run;
    // otherwise start at PRESSURE.
    int keep = had.isEmpty() ? -1 : dynBox_->findText(had);
    if (keep < 0) keep = dynBox_->findText(QStringLiteral("PRESSURE"));
    if (keep >= 0) dynBox_->setCurrentIndex(keep);
}

// Re-read the restart file of a case a run is writing, so report steps appear
// as they are produced instead of only when the run ends. Deliberately cheap:
// the grid and the mesh do not change during a run, so only the restart reader
// and the things that depend on it are touched.
void Viewer3DWidget::followRunningCase()
{
    const int idx = caseBox_ ? caseBox_->currentIndex() : -1;
    if (idx < 0 || idx >= cases_.size()) return;
    const CaseFiles& cf = cases_[idx];
    // Only the case being written, and only while it is being written.
    if (runningEgrid_.isEmpty() || !flowgui::sameCasePath(cf.egrid, runningEgrid_))
        return;
    if (!isVisible()) return;           // nothing to show for the work

    // The grid only exists once flow has written it. Until then there is
    // nothing to draw, and openCase() is the way in - it also builds the mesh.
    if (!grid_) {
        if (QFileInfo::exists(cf.egrid)) openCase(idx);
        return;
    }
    // Re-indexing a restart file means walking its array headers, and Norne's
    // runs to hundreds of megabytes - too much to repeat on the UI thread for
    // nothing. Its size only changes when flow has written more, so the ticks
    // between report steps cost a stat and no more.
    const QFileInfo ui(cf.unrst);
    if (!ui.exists()) return;
    const qint64 size = ui.size();
    if (size == lastUnrstSize_) return;
    lastUnrstSize_ = size;

    const std::size_t had = steps_.size();
    // Following the run means staying at the newest step; a user who has
    // parked on an earlier one is left there.
    const bool atEnd = !stepSlider_->isEnabled() ||
                       stepSlider_->value() == stepSlider_->maximum();
    try {
        auto rst   = std::make_unique<Opm::EclIO::ERst>(cf.unrst.toStdString());
        auto steps = rst->listOfReportStepNumbers();
        if (steps.size() <= had) return;          // nothing new yet
        rst_   = std::move(rst);
        steps_ = std::move(steps);
        rstBytes_ = 0;                            // the old reader's cache went with it
    } catch (...) {
        // A restart file caught mid-write; the next tick will find it whole.
        return;
    }

    if (dynBox_->count() == 0) populateDynamicProperties();
    stepSlider_->setEnabled(!steps_.empty());
    stepSlider_->setRange(0, int(steps_.size()) - 1);
    if (atEnd) stepSlider_->setValue(int(steps_.size()) - 1);   // draws the new step
    else       showProperty();
    // The report step reached, which is what says how far the run has got; the
    // number of steps in the file does not, since a restarted run's first
    // report step is wherever it resumed.
    setStatus(QStringLiteral("%1: running - report step %2 (%3 steps written)")
                  .arg(cf.label).arg(steps_.back()).arg(steps_.size()));
}

void Viewer3DWidget::showProperty()
{
    if (!grid_) return;
    try {
        if (staticSel_->isChecked()) {
            const QString name = staticBox_->currentText();
            if (name.isEmpty() || !init_) { gl_->setCellValues({}, QString()); return; }
            const std::string n = name.toStdString();
            if (staticBox_->currentData().toBool()) {
                gl_->setCellRegions(regionArray(name), name);
                syncRangeBoxes(QString());       // regions have no scale to fix
                gl_->setStepText(QString());
                return;
            }
            std::vector<float> v;
            // INIT arrays may be REAL or DOUB
            try { v = init_->getInitData<float>(n); }
            catch (...) {
                const auto& d = init_->getInitData<double>(n);
                v.assign(d.begin(), d.end());
            }
            applyRange(name, false);
            gl_->setCellValues(v, name);
            syncRangeBoxes(name);
            gl_->setStepText(QString());
        } else {
            stepChanged(stepSlider_->value());
        }
    } catch (const std::exception& e) {
        setStatus(QStringLiteral("property load failed: %1")
                      .arg(QString::fromLocal8Bit(e.what())));
    }
}

std::vector<int> Viewer3DWidget::regionArray(const QString& name) const
{
    if (!init_ || !grid_ || name.isEmpty()) return {};
    try {
        const std::string n = name.toStdString();
        if (!init_->hasKey(n)) return {};
        std::vector<int> r = init_->getInitData<int>(n);
        if (int(r.size()) == grid_->activeCells()) return r;
    } catch (...) {}
    return {};
}

void Viewer3DWidget::applyRegionFilter()
{
    const QString name = regionBox_->currentIndex() > 0 ? regionBox_->currentText() : QString();
    regionNums_->setEnabled(!name.isEmpty());
    regionInfo_->clear();
    const std::vector<int> r = regionArray(name);
    if (r.empty()) {
        regionNums_->setPlaceholderText(QString());
        cellMask_.clear();
        maskKey_.clear();
        gl_->setCellMask({});
        return;
    }
    const std::set<int> present(r.begin(), r.end());
    regionNums_->setPlaceholderText(QStringLiteral("all: %1")
        .arg(flowgui::regionNumbersText(QList<int>(present.begin(), present.end()))));

    // "1, 3-5": numbers and ranges, by comma or space. Empty keeps them all.
    std::set<int> want;
    QStringList bad;
    for (const QString& tok : regionNums_->text().split(
             QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts)) {
        const QStringList ab = tok.split(QLatin1Char('-'));
        bool ok1 = false, ok2 = false;
        const int a = ab.value(0).toInt(&ok1);
        const int b = ab.size() == 2 ? ab.value(1).toInt(&ok2) : a;
        if (!ok1 || (ab.size() == 2 && !ok2) || ab.size() > 2) { bad << tok; continue; }
        for (int k = std::min(a, b); k <= std::max(a, b); ++k) want.insert(k);
    }
    if (want.empty()) want = present;

    std::vector<char> mask(r.size());
    int shown = 0;
    for (std::size_t c = 0; c < r.size(); ++c)
        if ((mask[c] = char(want.count(r[c]) ? 1 : 0))) ++shown;
    cellMask_ = mask;
    maskKey_  = name + QLatin1Char('|') + regionNums_->text();
    gl_->setCellMask(std::move(mask));
    regionInfo_->setText(bad.isEmpty()
        ? QStringLiteral("%1 of %2 cells").arg(shown).arg(r.size())
        : QStringLiteral("cannot read %1").arg(bad.join(QStringLiteral(", "))));
}

// opm-common's EclFile caches every array it hands out and offers no per-array
// eviction, so stepping through a long run accumulates them: one dynamic
// property is 4 bytes per active cell per step, which on a multi-million-cell
// model reaches gigabytes over a full sweep. Past this much, drop the reader's
// cache with clearData(); it keeps the file index, so the only cost is
// re-reading a step that is visited again.
constexpr qint64 kRstCacheBudget = 256ll * 1024 * 1024;

std::vector<float> Viewer3DWidget::dynamicValues(const QString& name, int step)
{
    // Before anything is read: getRestartData() hands out references into the
    // reader, and those must not be alive across a clearData().
    if (rstBytes_ > kRstCacheBudget) {
        rst_->clearData();
        rstBytes_ = 0;
    }
    std::vector<float> v;
    const std::string n = name.toStdString();
    if (rst_->hasArray(n, step)) {
        v = rst_->getRestartData<float>(n, step);
        rstBytes_ += qint64(v.size()) * qint64(sizeof(float));
    } else if (name == QLatin1String("SOIL")) {
        // synthesized oil saturation: 1 - SWAT - SGAS, with a phase
        // that is not stored treated as absent (two-phase runs)
        v.assign(size_t(grid_->activeCells()), 1.0f);
        for (const char* sat : { "SWAT", "SGAS" }) {
            if (!rst_->hasArray(sat, step)) continue;
            const auto& s = rst_->getRestartData<float>(sat, step);
            rstBytes_ += qint64(s.size()) * qint64(sizeof(float));
            const size_t nn = std::min(v.size(), s.size());
            for (size_t i = 0; i < nn; ++i) v[i] -= s[i];
        }
        for (float& x : v) x = std::clamp(x, 0.0f, 1.0f);
    }
    return v;
}

bool Viewer3DWidget::runRange(const QString& name, bool dynamic, float& lo, float& hi)
{
    const QString key = QStringLiteral("%1|%2|%3|%4")
                            .arg(name).arg(dynamic).arg(maskKey_).arg(steps_.size());
    if (key != runRangeKey_) {
        runHas_ = false;
        const auto take = [this](const std::vector<float>& v) {
            for (std::size_t c = 0; c < v.size(); ++c) {
                if (!cellMask_.empty() && (c >= cellMask_.size() || !cellMask_[c])) continue;
                if (!runHas_) { runLo_ = runHi_ = v[c]; runHas_ = true; }
                else          { runLo_ = std::min(runLo_, v[c]); runHi_ = std::max(runHi_, v[c]); }
            }
        };
        try {
            if (dynamic && rst_) {
                QApplication::setOverrideCursor(Qt::WaitCursor);
                for (int s : steps_) take(dynamicValues(name, s));
                QApplication::restoreOverrideCursor();
            } else if (!dynamic && init_) {
                const std::string n = name.toStdString();
                std::vector<float> v;
                try { v = init_->getInitData<float>(n); }
                catch (...) {
                    const auto& d = init_->getInitData<double>(n);
                    v.assign(d.begin(), d.end());
                }
                take(v);
            }
        } catch (...) {
            if (dynamic) QApplication::restoreOverrideCursor();
            runHas_ = false;
        }
        runRangeKey_ = key;
    }
    lo = runLo_; hi = runHi_;
    return runHas_;
}

void Viewer3DWidget::applyRange(const QString& name, bool dynamic)
{
    if (!fixRange_->isChecked() || name.isEmpty()) {
        gl_->setValueRange(0.f, 0.f, QString(), false);
        return;
    }
    float lo = 0.f, hi = 0.f;
    const bool whole = runRange(name, dynamic, lo, hi);
    const auto typed = rangeTexts_.value(name);
    bool okLo = false, okHi = false;
    const float tLo = QString(typed.first).replace(QLatin1Char(','), QLatin1Char('.')).toFloat(&okLo);
    const float tHi = QString(typed.second).replace(QLatin1Char(','), QLatin1Char('.')).toFloat(&okHi);
    if (okLo) lo = tLo;
    if (okHi) hi = tHi;
    if (lo > hi) std::swap(lo, hi);
    if ((!whole && !(okLo && okHi)) || !(hi > lo)) {
        gl_->setValueRange(0.f, 0.f, QString(), false);
        return;
    }
    gl_->setValueRange(lo, hi, okLo || okHi ? QStringLiteral("fixed")
                                            : QStringLiteral("whole run"), false);
}

void Viewer3DWidget::syncRangeBoxes(const QString& name)
{
    const bool on = fixRange_->isChecked() && !name.isEmpty();
    rangeMin_->setEnabled(on);
    rangeMax_->setEnabled(on);
    float lo = 0.f, hi = 0.f;
    if (on) {
        const auto typed = rangeTexts_.value(name);
        rangeMin_->setText(typed.first);
        rangeMax_->setText(typed.second);
        if (runRange(name, dynSel_->isChecked(), lo, hi)) {
            rangeMin_->setPlaceholderText(QString::number(lo, 'g', 6));
            rangeMax_->setPlaceholderText(QString::number(hi, 'g', 6));
        }
    } else {
        // The range in effect, greyed, so fixing it starts from what is seen.
        rangeMin_->clear();
        rangeMax_->clear();
        const bool shown = !name.isEmpty() && gl_->valueRange(lo, hi);
        rangeMin_->setPlaceholderText(shown ? QString::number(lo, 'g', 6) : QString());
        rangeMax_->setPlaceholderText(shown ? QString::number(hi, 'g', 6) : QString());
    }
}

void Viewer3DWidget::stepChanged(int sliderPos)
{
    if (!grid_ || !rst_ || steps_.empty() || !dynSel_->isChecked()) return;
    const int step = steps_[std::clamp(sliderPos, 0, int(steps_.size()) - 1)];
    const QString name = dynBox_->currentText();
    if (name.isEmpty()) return;
    try {
        applyRange(name, true);
        const std::vector<float> v = dynamicValues(name, step);
        if (v.empty()) return;   // property not available at this step
        gl_->setCellValues(v, name);
        syncRangeBoxes(name);

        // date from INTEHEAD when plausible
        QString when;
        const auto& ih = rst_->getRestartData<int>("INTEHEAD", step);
        if (ih.size() > 66 && ih[66] >= 1900 && ih[66] <= 2300)
            when = QStringLiteral("  %1-%2-%3")
                .arg(ih[66], 4, 10, QLatin1Char('0'))
                .arg(ih[65], 2, 10, QLatin1Char('0'))
                .arg(ih[64], 2, 10, QLatin1Char('0'));
        // "step i/N" is the position in this file; "report step" is the
        // simulation's own number for it (SEQNUM in the restart file, but that
        // is the file format's word - the term of the trade is report step).
        // The two only coincide for a run that started from the beginning.
        gl_->setStepText(QStringLiteral("step %1/%2 (report step %3)%4")
            .arg(sliderPos + 1).arg(steps_.size()).arg(step).arg(when));
        stepLabel_->setText(QStringLiteral("step %1/%2 (report step %3)%4")
            .arg(sliderPos + 1).arg(steps_.size()).arg(step).arg(when));
        showWells();
    } catch (const std::exception& e) {
        setStatus(QStringLiteral("restart read failed: %1")
                      .arg(QString::fromLocal8Bit(e.what())));
    }
}

void Viewer3DWidget::showWells()
{
    if (!grid_ || !rst_ || steps_.empty() || !wellsChk_ || !wellsChk_->isChecked()) {
        gl_->setWells({});
        return;
    }
    const int step = steps_[std::clamp(stepSlider_->value(), 0, int(steps_.size()) - 1)];
    QVector<GridGLWidget::WellPath> wells;
    try {
        const auto& ih   = rst_->getRestartData<int>("INTEHEAD", step);
        const int nwells = ih[16], ncwmax = ih[17], niwelz = ih[24],
                  nzwelz = ih[27], niconz = ih[32];
        if (nwells <= 0) { gl_->setWells({}); return; }
        const auto& iwel = rst_->getRestartData<int>("IWEL", step);
        const auto& icon = rst_->getRestartData<int>("ICON", step);
        const auto& zwel = rst_->getRestartData<std::string>("ZWEL", step);

        std::array<double, 8> X, Y, Z;
        auto cellCenter = [&](int i, int j, int k) -> QVector3D {
            grid_->getCellCorners(grid_->global_index(i - 1, j - 1, k - 1), X, Y, Z);
            double sx = 0, sy = 0, sz = 0;
            for (int c = 0; c < 8; ++c) { sx += X[c]; sy += Y[c]; sz += Z[c]; }
            return QVector3D(float(sx / 8.0 - cx_), float(sy / 8.0 - cy_),
                             float(-(sz / 8.0 - cz_)));
        };

        for (int w = 0; w < nwells; ++w) {
            GridGLWidget::WellPath wp;
            wp.name = QString::fromStdString(zwel[size_t(w) * nzwelz]).trimmed();
            // IWEL well type (verified against Norne): 1 producer,
            // 2 oil injector, 3 water injector, 4 gas injector
            switch (iwel[size_t(w) * niwelz + 6]) {
            case 1:  wp.color = QColor(0x1e, 0x8a, 0x3c); break;   // producer: green
            case 2:  wp.color = QColor(0xd0, 0x7c, 0x1a); break;   // oil inj: orange
            case 3:  wp.color = QColor(0x1f, 0x4f, 0xc0); break;   // water inj: blue
            case 4:  wp.color = QColor(0xc0, 0x29, 0x1e); break;   // gas inj: red
            default: wp.color = QColor(0x55, 0x55, 0x55); break;   // unknown: grey
            }
            const int hi = iwel[size_t(w) * niwelz],
                      hj = iwel[size_t(w) * niwelz + 1];
            for (int c = 0; c < ncwmax; ++c) {
                const int* e = &icon[(size_t(w) * ncwmax + c) * niconz];
                if (e[1] > 0 && e[2] > 0 && e[3] > 0)
                    wp.points.push_back(cellCenter(e[1], e[2], e[3]));
            }
            if (!wp.points.isEmpty() && hi > 0 && hj > 0) {
                // stub from above the first connection to suggest the wellhead
                QVector3D top = wp.points.front();
                top.setZ(top.z() + (grid_->dimension()[2] > 1 ? 80.f : 20.f));
                wp.points.prepend(top);
            }
            if (wp.points.size() >= 2) wells.push_back(wp);
        }
    } catch (...) {
        wells.clear();
    }
    gl_->setWells(wells);
}
