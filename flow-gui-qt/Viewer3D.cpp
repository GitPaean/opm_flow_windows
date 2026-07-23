/*
  Viewer3D implementation. Part of the opm_flow_windows harness; GPL v3+
  (see repository LICENSE).
*/
#include "Viewer3D.h"

#include <opm/io/eclipse/EGrid.hpp>
#include <opm/io/eclipse/EInit.hpp>
#include <opm/io/eclipse/ERst.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
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
#include <tuple>

// ===========================================================================
// GridGLWidget
// ===========================================================================
namespace {

const char* kVert = R"(#version 130
in vec3 aPos;
in vec3 aNrm;
in vec3 aCol;
uniform mat4 uMvp;
uniform mat3 uNrm;
uniform float uZScale;
out vec3 vCol;
out float vLight;
void main() {
    vec3 p = vec3(aPos.x, aPos.y, aPos.z * uZScale);
    gl_Position = uMvp * vec4(p, 1.0);
    vec3 n = normalize(uNrm * aNrm);
    vLight = 0.25 + 0.75 * abs(n.z);
    vCol = aCol;
}
)";

const char* kFrag = R"(#version 130
in vec3 vCol;
in float vLight;
uniform bool uFlat;
out vec4 frag;
void main() {
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
    meshDirty_ = colorDirty_ = true;
    resetCamera();
}

void GridGLWidget::setCellValues(const std::vector<float>& v, const QString& legendTitle)
{
    legendTitle_ = legendTitle;
    if (int(v.size()) != cellCount_ || cellCount_ == 0) {
        hasValues_ = false;
        std::fill(col_.begin(), col_.end(), 0.6f);
    } else {
        vmin_ = *std::min_element(v.begin(), v.end());
        vmax_ = *std::max_element(v.begin(), v.end());
        const float span = (vmax_ > vmin_) ? (vmax_ - vmin_) : 1.0f;
        hasValues_ = true;
        for (int c = 0; c < cellCount_; ++c) {
            float r, g, b;
            jet((v[c] - vmin_) / span, r, g, b);
            float* dst = &col_[size_t(c) * 36 * 3];
            for (int k = 0; k < 36; ++k) { dst[0]=r; dst[1]=g; dst[2]=b; dst += 3; }
        }
    }
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
    zscale_ = s;
    update();
}

void GridGLWidget::resetCamera()
{
    const QVector3D d = bboxMax_ - bboxMin_;
    dist_ = std::max(1.0f, d.length()) * 1.8f;
    // like looking at a mountain from afar: from a distance, somewhat to the
    // side and moderately above (~22 degrees elevation)
    yaw_ = -50.f; pitch_ = 22.f;
    panOffset_ = QVector3D();
    update();
}

void GridGLWidget::initializeGL()
{
    initializeOpenGLFunctions();
    glClearColor(0.96f, 0.97f, 0.98f, 1.0f);
    prog_ = std::make_unique<QOpenGLShaderProgram>();
    prog_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVert);
    prog_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag);
    prog_->bindAttributeLocation("aPos", 0);
    prog_->bindAttributeLocation("aNrm", 1);
    prog_->bindAttributeLocation("aCol", 2);
    prog_->link();
    vboPos_.create(); vboNrm_.create(); vboCol_.create();
    glReady_ = true;
}

QMatrix4x4 GridGLWidget::mvp() const
{
    const QVector3D center = 0.5f * (bboxMin_ + bboxMax_);
    const QVector3D scaledCenter(center.x(), center.y(), center.z() * float(zscale_));

    QMatrix4x4 view;
    view.translate(0, 0, -dist_);
    view.rotate(pitch_, 1, 0, 0);
    view.rotate(yaw_,   0, 0, 1);
    view.translate(-scaledCenter - panOffset_);

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
    // normal matrix for the z-scaled model (uniform in x/y): inverse-transpose
    QMatrix4x4 model; model.scale(1.f, 1.f, float(zscale_));
    const QMatrix3x3 nrmM = model.normalMatrix();

    prog_->bind();
    prog_->setUniformValue("uMvp", m);
    prog_->setUniformValue("uNrm", nrmM);
    prog_->setUniformValue("uZScale", float(zscale_));
    prog_->setUniformValue("uFlat", false);

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

    if (hasValues_) {
        const int x = 14, y = 40, w = 18, h = 180;
        QLinearGradient gr(x, y + h, x, y);
        for (int i = 0; i <= 10; ++i) {
            float r, g, b; jet(i / 10.0f, r, g, b);
            gr.setColorAt(i / 10.0, QColor::fromRgbF(r, g, b));
        }
        p.fillRect(x, y, w, h, gr);
        p.setPen(Qt::black);
        p.drawRect(x, y, w, h);
        p.drawText(x + w + 6, y + 12,      QString::number(vmax_, 'g', 5));
        p.drawText(x + w + 6, y + h,       QString::number(vmin_, 'g', 5));
        p.drawText(x, y - 10, legendTitle_);
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
        pitch_ = std::clamp(pitch_, -89.f, 89.f);
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
    {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("Case:")));
        caseBox_ = new QComboBox;
        caseBox_->setMinimumWidth(240);
        row->addWidget(caseBox_, 1);
        auto* bopen = new QPushButton(QStringLiteral("Open EGRID..."));
        row->addWidget(bopen);
        row->addSpacing(12);

        staticSel_ = new QRadioButton(QStringLiteral("static:"));
        staticBox_ = new QComboBox; staticBox_->setMinimumWidth(110);
        dynSel_    = new QRadioButton(QStringLiteral("dynamic:"));
        dynBox_    = new QComboBox; dynBox_->setMinimumWidth(110);
        staticSel_->setChecked(true);
        row->addWidget(staticSel_); row->addWidget(staticBox_);
        row->addWidget(dynSel_);    row->addWidget(dynBox_);

        wellsChk_ = new QCheckBox(QStringLiteral("wells"));
        wellsChk_->setChecked(true);
        row->addWidget(wellsChk_);

        row->addWidget(new QLabel(QStringLiteral("Z x")));
        zscale_ = new QDoubleSpinBox;
        zscale_->setRange(0.1, 50.0);
        zscale_->setValue(3.0);
        zscale_->setSingleStep(0.5);
        row->addWidget(zscale_);
        auto* bview = new QPushButton(QStringLiteral("Reset view"));
        row->addWidget(bview);
        connect(bview, &QPushButton::clicked, this, [this] { gl_->resetCamera(); });
        top->addLayout(row);

        connect(bopen, &QPushButton::clicked, this, [this] {
            const QString f = QFileDialog::getOpenFileName(
                this, QStringLiteral("Open grid file"), QString(),
                QStringLiteral("Eclipse grid (*.EGRID);;All files (*)"));
            if (f.isEmpty()) return;
            QString base = f; base.chop(6);   // ".EGRID"
            addCase(QFileInfo(f).completeBaseName(), base + QStringLiteral(".SMSPEC"));
            caseBox_->setCurrentIndex(caseBox_->count() - 1);
        });
        connect(caseBox_, &QComboBox::currentIndexChanged, this,
                [this](int i) { openCase(i); });
        auto onProp = [this] { showProperty(); };
        connect(staticBox_, &QComboBox::currentIndexChanged, this, onProp);
        connect(dynBox_,    &QComboBox::currentIndexChanged, this, onProp);
        connect(staticSel_, &QRadioButton::toggled, this, onProp);
        connect(wellsChk_, &QCheckBox::toggled, this, [this](bool) { showWells(); });
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
        stepLabel_ = new QLabel(QStringLiteral("-"));
        stepLabel_->setMinimumWidth(160);
        row->addWidget(rewindBtn);
        row->addWidget(playBtn_);
        row->addWidget(stepSlider_, 1);
        row->addWidget(stepLabel_);
        top->addLayout(row);

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

void Viewer3DWidget::addCase(const QString& label, const QString& smspecPath)
{
    QString base = smspecPath;
    if (base.endsWith(QStringLiteral(".SMSPEC"), Qt::CaseInsensitive)) base.chop(7);
    const CaseFiles cf{ label,
                        base + QStringLiteral(".EGRID"),
                        base + QStringLiteral(".INIT"),
                        base + QStringLiteral(".UNRST") };
    for (const auto& c : cases_)
        if (c.egrid == cf.egrid) return;
    cases_.push_back(cf);
    caseBox_->addItem(label, cf.egrid);
    if (caseBox_->count() == 1) caseBox_->setCurrentIndex(0);
}

void Viewer3DWidget::openCase(int idx)
{
    grid_.reset(); init_.reset(); rst_.reset();
    steps_.clear(); cellGlob_.clear();
    staticBox_->clear(); dynBox_->clear();
    stepSlider_->setEnabled(false);
    stepSlider_->setRange(0, 0);
    gl_->setWells({});
    gl_->setStepText(QString());
    if (idx < 0 || idx >= cases_.size()) return;
    const CaseFiles& cf = cases_[idx];

    if (!QFileInfo::exists(cf.egrid)) {
        setStatus(QStringLiteral("no EGRID yet at %1").arg(cf.egrid));
        return;
    }
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
    try {
        if (QFileInfo::exists(cf.unrst)) {
            rst_ = std::make_unique<Opm::EclIO::ERst>(cf.unrst.toStdString());
            steps_ = rst_->listOfReportStepNumbers();
        }
    } catch (...) { rst_.reset(); steps_.clear(); }

    populateProperties();
    if (!steps_.empty()) {
        stepSlider_->setEnabled(true);
        stepSlider_->setRange(0, int(steps_.size()) - 1);
        stepSlider_->setValue(int(steps_.size()) - 1);
    }
    const auto d = grid_->dimension();
    setStatus(QStringLiteral("%1: %2x%3x%4, %5 active cells, %6 static, %7 dynamic, %8 report steps")
        .arg(cf.label).arg(d[0]).arg(d[1]).arg(d[2]).arg(grid_->activeCells())
        .arg(staticBox_->count()).arg(dynBox_->count()).arg(steps_.size()));
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
        for (const auto& [name, typ, size] : init_->list_arrays())
            if ((typ == Opm::EclIO::REAL || typ == Opm::EclIO::DOUB) &&
                size == grid_->activeCells())
                staticBox_->addItem(QString::fromStdString(name));
        const int poro = staticBox_->findText(QStringLiteral("PORO"));
        if (poro >= 0) staticBox_->setCurrentIndex(poro);
    }
    if (rst_ && grid_ && !steps_.empty()) {
        for (const auto& [name, typ, size] : rst_->listOfRstArrays(steps_.back()))
            if (typ == Opm::EclIO::REAL && size == grid_->activeCells())
                dynBox_->addItem(QString::fromStdString(name));
        // SOIL is usually not stored in the restart; offer it synthesized
        // from the stored saturations (SOIL = 1 - SWAT - SGAS).
        if (dynBox_->findText(QStringLiteral("SOIL")) < 0 &&
            (rst_->hasArray("SWAT", steps_.back()) ||
             rst_->hasArray("SGAS", steps_.back())))
            dynBox_->addItem(QStringLiteral("SOIL"));
        const int p = dynBox_->findText(QStringLiteral("PRESSURE"));
        if (p >= 0) dynBox_->setCurrentIndex(p);
    }
    staticBox_->blockSignals(false);
    dynBox_->blockSignals(false);
}

void Viewer3DWidget::showProperty()
{
    if (!grid_) return;
    try {
        if (staticSel_->isChecked()) {
            const QString name = staticBox_->currentText();
            if (name.isEmpty() || !init_) { gl_->setCellValues({}, QString()); return; }
            const std::string n = name.toStdString();
            std::vector<float> v;
            // INIT arrays may be REAL or DOUB
            try { v = init_->getInitData<float>(n); }
            catch (...) {
                const auto& d = init_->getInitData<double>(n);
                v.assign(d.begin(), d.end());
            }
            gl_->setCellValues(v, name);
            gl_->setStepText(QString());
        } else {
            stepChanged(stepSlider_->value());
        }
    } catch (const std::exception& e) {
        setStatus(QStringLiteral("property load failed: %1")
                      .arg(QString::fromLocal8Bit(e.what())));
    }
}

void Viewer3DWidget::stepChanged(int sliderPos)
{
    if (!grid_ || !rst_ || steps_.empty() || !dynSel_->isChecked()) return;
    const int step = steps_[std::clamp(sliderPos, 0, int(steps_.size()) - 1)];
    const QString name = dynBox_->currentText();
    if (name.isEmpty()) return;
    try {
        std::vector<float> v;
        const std::string n = name.toStdString();
        if (rst_->hasArray(n, step)) {
            v = rst_->getRestartData<float>(n, step);
        } else if (name == QLatin1String("SOIL")) {
            // synthesized oil saturation: 1 - SWAT - SGAS, with a phase
            // that is not stored treated as absent (two-phase runs)
            v.assign(size_t(grid_->activeCells()), 1.0f);
            for (const char* sat : { "SWAT", "SGAS" }) {
                if (!rst_->hasArray(sat, step)) continue;
                const auto& s = rst_->getRestartData<float>(sat, step);
                const size_t nn = std::min(v.size(), s.size());
                for (size_t i = 0; i < nn; ++i) v[i] -= s[i];
            }
            for (float& x : v) x = std::clamp(x, 0.0f, 1.0f);
        } else {
            return;   // property not available at this step
        }
        gl_->setCellValues(v, name);

        // date from INTEHEAD when plausible
        QString when;
        const auto& ih = rst_->getRestartData<int>("INTEHEAD", step);
        if (ih.size() > 66 && ih[66] >= 1900 && ih[66] <= 2300)
            when = QStringLiteral("  %1-%2-%3")
                .arg(ih[66], 4, 10, QLatin1Char('0'))
                .arg(ih[65], 2, 10, QLatin1Char('0'))
                .arg(ih[64], 2, 10, QLatin1Char('0'));
        gl_->setStepText(QStringLiteral("report step %1/%2 (SEQNUM %3)%4")
            .arg(sliderPos + 1).arg(steps_.size()).arg(step).arg(when));
        stepLabel_->setText(QStringLiteral("step %1/%2%3")
            .arg(sliderPos + 1).arg(steps_.size()).arg(when));
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
