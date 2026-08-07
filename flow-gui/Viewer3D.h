/*
  Viewer3D - 3D visualization of simulation results: the corner-point grid
  colored by a selectable static (INIT) or dynamic (UNRST) cell property,
  well trajectories from the restart well/connection arrays, and report-step
  animation for the dynamic properties.

  Rendering is a self-contained QOpenGLWidget (no external engine), which
  keeps the build dependency-free on both Windows and Linux.

  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

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
class QJsonObject;
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
    // Off draws every face at its colour-bar colour exactly, which is what
    // you want to read a value off; on trades a little of that for shape.
    void setShaded(bool on) { shaded_ = on; update(); }
    void resetCamera();

    // Standard viewpoints. Only the camera angles change - zoom and pan are
    // kept, so flipping under the model stays on the region being inspected.
    enum class StdView { Home, Top, Bottom, Side };
    void setStandardView(StdView v);

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
    // The camera on its own. paintGL() needs it to put the surface normals in
    // eye space, so the shading can follow the camera rather than the grid.
    QMatrix4x4 viewMatrix() const;

    std::unique_ptr<QOpenGLShaderProgram> prog_;
    QOpenGLBuffer vboPos_, vboNrm_, vboCol_;
    bool  shaded_ = true;
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

    // The case a simulation is currently writing, as its SMSPEC path; empty
    // when nothing is running. While a case is being written this tab leaves
    // its restart file alone - see openCase() for why.
    void setRunningCase(const QString& smspecPath);

    // Follow a rename made in the Summary Plots tab (same case identity).
    void renameCase(const QString& smspecPath, const QString& label);

    // Drop a case from the list - the Summary Plots tab dropped the same one,
    // or the Remove button here. The files on disk are left alone.
    void removeCase(const QString& smspecPath);

    // Follow the order the Summary Plots tab's list was put in, so the two
    // lists read the same way round. Cases this tab does not have are simply
    // not there; ones it has that the list does not mention keep their place
    // at the end.
    void reorderCases(const QStringList& smspecPaths);

    // Session state: which case is shown, with which property, and how. The
    // case itself is only opened once the tab is looked at, so a restored
    // choice waits in pending* below until then.
    QJsonObject uiState() const;
    void restoreUiState(const QJsonObject& state);

protected:
    // The selected case's files may have appeared while the tab was hidden.
    void showEvent(QShowEvent* ev) override;

private:
    struct CaseFiles {
        QString label, egrid, init, unrst;
    };

    // Should this case's restart file be left alone right now? True only when a
    // run is writing it AND it sits where a reader can disturb that writer.
    bool restartIsBusy(const CaseFiles& cf) const;

    // Re-read the restart file of the case a run is writing, so its report
    // steps appear while it runs rather than only when it finishes. Driven by
    // followTimer_, and cheap: the grid and mesh are left as they are.
    void followRunningCase();

    // Fill the dynamic (restart) property list from the current reader.
    void populateDynamicProperties();

    QComboBox*      caseBox_   = nullptr;
    QRadioButton*   staticSel_ = nullptr;
    QRadioButton*   dynSel_    = nullptr;
    QComboBox*      staticBox_ = nullptr;
    QComboBox*      dynBox_    = nullptr;
    QCheckBox*      wellsChk_  = nullptr;
    QCheckBox*      shadingChk_ = nullptr;   // relief, at the cost of exact colour
    QDoubleSpinBox* zscale_    = nullptr;
    QSlider*        stepSlider_ = nullptr;
    QPushButton*    playBtn_   = nullptr;
    QLabel*         stepLabel_ = nullptr;
    QLabel*         status_    = nullptr;
    GridGLWidget*   gl_        = nullptr;
    QTimer*         playTimer_ = nullptr;
    QTimer*         followTimer_ = nullptr;  // ticks while a run writes the shown case

    QVector<CaseFiles> cases_;
    std::unique_ptr<Opm::EclIO::EGrid> grid_;
    std::unique_ptr<Opm::EclIO::EInit> init_;
    std::unique_ptr<Opm::EclIO::ERst>  rst_;
    std::vector<int>   steps_;          // restart report step numbers
    // The .EGRID key of the case a simulation is writing right now, empty when
    // none is. Kept in the same currency the case list uses so it can be
    // compared with flowgui::sameCasePath().
    QString            runningEgrid_;
    // The case index whose restart read has already been retried once, so a
    // file that stays unreadable does not spin. -1 = nothing retried yet.
    int                rstRetryIdx_ = -1;
    // Size the restart file had when it was last indexed while following a
    // run; a tick that finds it unchanged has nothing to do. -1 = not seen.
    qint64             lastUnrstSize_ = -1;
    // Bytes the restart reader has been asked for since it last dropped its
    // cache; see kRstCacheBudget in the .cpp.
    qint64             rstBytes_ = 0;
    std::vector<int>   cellGlob_;       // active index -> global index
    double cx_ = 0, cy_ = 0, cz_ = 0;   // mesh center offset (double precision)

    // Opening a case reads the grid and builds the whole mesh, so it waits
    // until the tab is actually shown: pendingCase_ is the case to open then,
    // and pendingProp_/pendingStep_ the restored choice to apply once its
    // property boxes have been filled.
    int     pendingCase_ = -1;
    QString pendingProp_;
    bool    pendingDynamic_ = false;
    int     pendingStep_ = -1;
    bool    havePending_ = false;

    void openCase(int idx);
    void removeCaseAt(int idx);
    void clearView();
    void buildMesh();
    void populateProperties();
    void showProperty();
    void showWells();
    void stepChanged(int sliderPos);
    void setStatus(const QString& s);
};
