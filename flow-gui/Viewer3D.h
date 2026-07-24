/*
  Viewer3D - 3D visualization of simulation results: the corner-point grid
  colored by a selectable static (INIT) or dynamic (UNRST) cell property,
  well trajectories from the restart well/connection arrays, and report-step
  animation for the dynamic properties.

  Rendering is a self-contained QOpenGLWidget (no external engine), which
  keeps the build dependency-free on both Windows and Linux.

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QColor>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QPoint>
#include <QString>
#include <QVector>
#include <QWidget>

#include <memory>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QRadioButton;
class QSlider;
class QTimer;

namespace Opm { namespace EclIO { class EGrid; class EInit; class ERst; } }

// ---------------------------------------------------------------------------
// The OpenGL canvas: cell mesh with per-cell scalar coloring, well polylines,
// orbit/pan/zoom camera, vertical exaggeration, and a QPainter overlay for
// the color legend and well names.
class GridGLWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
public:
    explicit GridGLWidget(QWidget* parent = nullptr);
    ~GridGLWidget() override;

    struct WellPath {
        QString              name;
        QVector<QVector3D>   points;    // center-relative coordinates
        QColor               color { 0x55, 0x00, 0x00 };
    };

    // Geometry: 36 vertices per cell (6 faces x 2 triangles), center-relative.
    void setMesh(std::vector<float> pos, std::vector<float> nrm, int cellCount);
    // Per-cell values -> jet colormap; empty vector = uniform grey.
    void setCellValues(const std::vector<float>& v, const QString& legendTitle);
    void setWells(const QVector<WellPath>& wells);
    void setZScale(double s);
    void setStepText(const QString& t) { stepText_ = t; update(); }
    void resetCamera();

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void wheelEvent(QWheelEvent* ev) override;

private:
    void uploadColors();
    QMatrix4x4 mvp() const;

    std::unique_ptr<QOpenGLShaderProgram> prog_;
    QOpenGLBuffer vboPos_, vboNrm_, vboCol_;
    bool  glReady_ = false;
    bool  meshDirty_ = false, colorDirty_ = false;

    std::vector<float> pos_, nrm_, col_;
    int   cellCount_ = 0;
    int   vertCount_ = 0;
    QVector<WellPath> wells_;

    // scalar/legend state
    QString legendTitle_, stepText_;
    float   vmin_ = 0.f, vmax_ = 0.f;
    bool    hasValues_ = false;

    // camera
    QVector3D bboxMin_, bboxMax_;
    float  dist_ = 1.f;
    float  yaw_ = -35.f, pitch_ = 25.f;
    float  homeYaw_ = -50.f;      // default yaw: long grid axis across the screen
    QVector3D panOffset_;
    double zscale_ = 3.0;
    QPoint lastMouse_;
};

// ---------------------------------------------------------------------------
// The tab: case selection, property choice (static / dynamic), report-step
// slider + play, Z exaggeration, and the GL canvas.
class Viewer3DWidget : public QWidget
{
public:
    explicit Viewer3DWidget(QWidget* parent = nullptr);
    ~Viewer3DWidget() override;

    // Register a case (same currency as the Results tab: the SMSPEC path;
    // the EGRID/INIT/UNRST siblings are derived from it).
    void addCase(const QString& label, const QString& smspecPath);

    // The job writing this case just finished: (re)open it if it is the
    // selected one - it was registered at job start, possibly before the
    // EGRID existed, and the final restart steps only exist now.
    void caseFinished(const QString& smspecPath);

protected:
    // The selected case's files may have appeared while the tab was hidden.
    void showEvent(QShowEvent* ev) override;

private:
    struct CaseFiles {
        QString label, egrid, init, unrst;
    };

    QComboBox*      caseBox_   = nullptr;
    QRadioButton*   staticSel_ = nullptr;
    QRadioButton*   dynSel_    = nullptr;
    QComboBox*      staticBox_ = nullptr;
    QComboBox*      dynBox_    = nullptr;
    QCheckBox*      wellsChk_  = nullptr;
    QDoubleSpinBox* zscale_    = nullptr;
    QSlider*        stepSlider_ = nullptr;
    QPushButton*    playBtn_   = nullptr;
    QLabel*         stepLabel_ = nullptr;
    QLabel*         status_    = nullptr;
    GridGLWidget*   gl_        = nullptr;
    QTimer*         playTimer_ = nullptr;

    QVector<CaseFiles> cases_;
    std::unique_ptr<Opm::EclIO::EGrid> grid_;
    std::unique_ptr<Opm::EclIO::EInit> init_;
    std::unique_ptr<Opm::EclIO::ERst>  rst_;
    std::vector<int>   steps_;          // restart report step numbers
    std::vector<int>   cellGlob_;       // active index -> global index
    double cx_ = 0, cy_ = 0, cz_ = 0;   // mesh center offset (double precision)

    void openCase(int idx);
    void buildMesh();
    void populateProperties();
    void showProperty();
    void showWells();
    void stepChanged(int sliderPos);
    void setStatus(const QString& s);
};
