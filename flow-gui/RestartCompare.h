/*
  RestartCompare - do two runs agree, and if not, where and when?

  A UNRST holds a value per active cell per property per report step, which is
  far too much to look at directly. Each (property, step) is reduced to a
  handful of numbers - the largest absolute difference, the RMS, how many cells
  fall outside tolerance, and the pore-volume weighted mean of each run - off
  one streaming pass that never holds more than two arrays at a time.

  Steps are paired by the DATE they were written at, not by their position in
  the file and not by SEQNUM. Two runs of the same field need not report at the
  same times: a restart, a different TUNING, or adaptive stepping all shift
  them, and comparing the n-th step of one against the n-th of the other would
  then be comparing different moments and calling the result a divergence.
  INTEHEAD carries the calendar date of each step, as integers, so dates pair
  exactly and there is no tolerance to argue about.

  What counts as a difference is not invented here: it is the test compareECL
  applies, so a green result means the same thing as a green regression test.
  See diffIsSignificant().

  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <atomic>
#include <cmath>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QThread;
class QTimer;
class QChart;
class QChartView;
class QTabWidget;

namespace flowgui {

// Absolute and relative, both, exactly as compareECL takes them.
struct DiffTol {
    double abs = 1e-4;
    double rel = 1e-4;
};

// compareECL's test, from ECLRegressionTest::deviationsForCell(): a pair of
// values differs when the absolute deviation is over tolerance AND either the
// relative deviation is too, or there is no relative deviation to speak of
// because one of the values is zero. Needing BOTH is what stops a large value
// tripping on floating-point noise, and the zero case is what stops a value
// appearing out of nothing from being excused for lack of a ratio.
inline bool diffIsSignificant(double a, double b, const DiffTol& tol)
{
    if (a == 0.0 && b == 0.0) return false;
    const double dAbs = std::abs(a - b);
    const double denom = std::max(std::abs(a), std::abs(b));
    const bool relUndefined = (a == 0.0 || b == 0.0);
    const double dRel = relUndefined ? -1.0 : dAbs / denom;
    return dAbs > tol.abs && (dRel > tol.rel || relUndefined);
}

// One property at one report DATE, reduced.
struct StepDiff {
    QDateTime when;
    int    seqA = -1, seqB = -1;   // the two files' own step numbers, as labels
    double maxAbs   = 0.0;
    double rms      = 0.0;
    int    nBad     = 0;
    int    worstCell = -1;
    double aWorst = 0.0, bWorst = 0.0;
    // The quantity itself, averaged over the field: pore-volume weighted when
    // a PORV was found, else a plain cell mean. Weighted is the one that means
    // something physically - an unweighted mean of PRESSURE is not the average
    // reservoir pressure, it is the average of a set of numbers.
    double aggA = 0.0, aggB = 0.0;
};

struct KeywordDiff {
    QString           keyword;
    QVector<StepDiff> steps;
    QDateTime firstBad;            // invalid when it never differs
    long   totalBad       = 0;
    double maxAbsOverall  = 0.0;
    bool   clean() const { return totalBad == 0; }
};

struct CompareResult {
    bool    ran = false;
    bool    cancelled = false;
    QString problem;

    // Paired by date. What only one side has is reported, not intersected away.
    QVector<QDateTime> times;
    QVector<QDateTime> timesOnlyInA, timesOnlyInB;
    QDateTime endA, endB;          // last report date of each run
    QStringList  kwOnlyInA, kwOnlyInB;
    QStringList  kwSkipped;        // present, but not one value per cell
    QString      gridNote;
    bool         porvWeighted = false;

    QVector<KeywordDiff> keywords;

    bool    identical() const;
    bool    sameEnd() const;       // both runs reached the same date
    QString verdict() const;
};

CompareResult compareRestarts(const QString& smspecA, const QString& smspecB,
                              const DiffTol& tol,
                              std::atomic<int>* progress = nullptr,
                              std::atomic<bool>* cancel = nullptr);

// ---------------------------------------------------------------------------
// Properties down, time across, colour for how far apart the two runs are.
// The overview: with a dozen or more properties a line per property is a
// tangle, and the question it has to answer first is WHICH property and WHEN,
// not by how much.
class DivergenceHeatmap : public QWidget
{
    Q_OBJECT
public:
    explicit DivergenceHeatmap(QWidget* parent = nullptr);
    void setResult(const CompareResult* r, int metric, bool onlyDiffering);

signals:
    void cellPicked(const QString& keyword, const QDateTime& when);

protected:
    void paintEvent(QPaintEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    bool event(QEvent* ev) override;          // tooltips
    QSize minimumSizeHint() const override;

private:
    int rowAt(int y) const;
    int colAt(int x) const;
    double valueAt(int row, int col) const;

    const CompareResult* r_ = nullptr;
    int  metric_ = 0;
    bool onlyDiffering_ = true;
    QVector<int> rows_;      // indices into r_->keywords, in display order
    double vmax_ = 0.0;
    int    labelW_ = 90;
};

// ---------------------------------------------------------------------------
class RestartComparePanel : public QWidget
{
    Q_OBJECT
public:
    explicit RestartComparePanel(QWidget* parent = nullptr);
    ~RestartComparePanel() override;

    void addCase(const QString& label, const QString& smspecPath);
    void renameCase(const QString& smspecPath, const QString& label);
    void removeCase(const QString& smspecPath);
    void reorderCases(const QStringList& smspecPaths);

    struct CaseEntry { QString label, smspec; };

signals:
    // "Add case..." picked a file. As in the 3D tab, it is handed to whoever
    // owns the case list rather than added here, so it is deduped and named
    // once and consistently across the tabs. Wired in FlowGuiWindow.
    void openCaseRequested(const QString& smspecPath);

private:
    void startCompare();
    void finishCompare();
    void showResult();
    void replot();
    void showKeywordDetail(const QString& keyword);
    void showStepDetail(const QDateTime& when);
    void refreshDetail();
    void syncCombos();
    void pickFromHeatmap(const QString& keyword, const QDateTime& when);

    QComboBox*      caseA_ = nullptr;
    QComboBox*      caseB_ = nullptr;
    QDoubleSpinBox* absTol_ = nullptr;
    QDoubleSpinBox* relTol_ = nullptr;
    QComboBox*      metric_ = nullptr;
    QCheckBox*      onlyBad_ = nullptr;
    QPushButton*    runBtn_ = nullptr;
    QPushButton*    addBtn_ = nullptr;
    QProgressBar*   bar_    = nullptr;
    QLabel*         verdict_ = nullptr;
    QLabel*         note_    = nullptr;
    QTabWidget*     views_  = nullptr;
    DivergenceHeatmap* heat_ = nullptr;
    QTableWidget*   table_  = nullptr;
    QComboBox*      detailMode_ = nullptr;
    QComboBox*      detailPick_ = nullptr;
    QLabel*         detailInfo_ = nullptr;
    QTableWidget*   detail_ = nullptr;
    QChart*         chart_  = nullptr;
    QChartView*     chartView_ = nullptr;
    QTimer*         poll_   = nullptr;
    QThread*        worker_ = nullptr;

    QVector<CaseEntry> cases_;
    CompareResult      result_;
    std::atomic<int>   progress_{0};
    std::atomic<bool>  cancel_{false};
};

} // namespace flowgui
