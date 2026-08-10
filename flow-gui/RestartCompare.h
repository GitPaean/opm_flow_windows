/*
  RestartCompare - do two runs agree, and if not, from when?

  A UNRST holds a value per active cell per property per report step, which is
  far too much to look at directly: comparing two of them by eye is hopeless
  and comparing them cell by cell produces more output than anyone reads. So
  each (property, step) is reduced to three numbers - the largest absolute
  difference, the RMS, and how many cells fall outside tolerance - and those
  are plotted against the report step. That answers the two questions actually
  being asked, "are these the same" and "from where do they part", off one
  streaming pass and without holding a whole restart in memory.

  What counts as a difference is not invented here: it is the test compareECL
  applies, so a green result here means the same thing as a green regression
  test rather than something almost like it. See diffIsSignificant().

  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QThread;
class QTimer;
QT_BEGIN_NAMESPACE
namespace QtCharts { }
QT_END_NAMESPACE
class QChart;
class QChartView;

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
    // rel is undefined (compareECL leaves it at -1) when either value is zero
    const bool relUndefined = (a == 0.0 || b == 0.0);
    const double dRel = relUndefined ? -1.0 : dAbs / denom;
    return dAbs > tol.abs && (dRel > tol.rel || relUndefined);
}

// One property at one report step, reduced.
struct StepDiff {
    int    seqnum   = 0;
    double maxAbs   = 0.0;
    double rms      = 0.0;
    int    nBad     = 0;    // cells failing diffIsSignificant()
    int    worstCell = -1;
    double aWorst = 0.0, bWorst = 0.0;
};

struct KeywordDiff {
    QString           keyword;
    QVector<StepDiff> steps;
    int    firstBadSeqnum = -1;
    long   totalBad       = 0;
    double maxAbsOverall  = 0.0;
    bool   clean() const { return totalBad == 0; }
};

struct CompareResult {
    bool    ran = false;         // finished rather than failed or was cancelled
    bool    cancelled = false;
    QString problem;             // why it could not run, when !ran

    // What the two cases have in common, and what they do not. Reported
    // rather than silently intersected away: a step or a property missing
    // from one side is usually the most interesting thing about a comparison.
    QVector<int> steps;
    QVector<int> stepsOnlyInA, stepsOnlyInB;
    QStringList  kwOnlyInA, kwOnlyInB;
    QString      gridNote;       // dimensions/active cells, or why they clash

    QVector<KeywordDiff> keywords;

    bool    identical() const;
    QString verdict() const;     // the one line worth reading first
};

// Compare the UNRST siblings of two SMSPEC paths. Runs on the calling thread;
// `cancel` is polled between arrays and `progress` gets 0..100.
CompareResult compareRestarts(const QString& smspecA, const QString& smspecB,
                              const DiffTol& tol,
                              std::atomic<int>* progress = nullptr,
                              std::atomic<bool>* cancel = nullptr);

// ---------------------------------------------------------------------------
// The tab: pick two cases, pick tolerances, get the verdict, and drill into
// whichever property or report step the verdict points at.
//
// A tab and not a dialog. Asking whether two runs agree is a thing people come
// to this program to do, on the same footing as running one or plotting one,
// and a button at the end of a wrapping toolbar is where features go to be
// missed. It also mirrors the case list the way the 3D tab does, so a run that
// finishes while this is open can be compared without reopening anything.
class RestartComparePanel : public QWidget
{
    Q_OBJECT
public:
    explicit RestartComparePanel(QWidget* parent = nullptr);
    ~RestartComparePanel() override;

    // The same case currency as the other tabs: the SMSPEC path.
    void addCase(const QString& label, const QString& smspecPath);
    void renameCase(const QString& smspecPath, const QString& label);
    void removeCase(const QString& smspecPath);
    void reorderCases(const QStringList& smspecPaths);

    struct CaseEntry { QString label, smspec; };

private:
    void startCompare();
    void finishCompare();
    void showResult();
    void replot();
    void showKeywordDetail(const QString& keyword);
    void showStepDetail(int seqnum);
    void refreshDetail();
    void syncCombos();

    QComboBox*      caseA_ = nullptr;
    QComboBox*      caseB_ = nullptr;
    QDoubleSpinBox* absTol_ = nullptr;
    QDoubleSpinBox* relTol_ = nullptr;
    QComboBox*      metric_ = nullptr;
    QCheckBox*      onlyBad_ = nullptr;
    QPushButton*    runBtn_ = nullptr;
    QProgressBar*   bar_    = nullptr;
    QLabel*         verdict_ = nullptr;
    QLabel*         note_    = nullptr;
    QTableWidget*   table_  = nullptr;
    QComboBox*      detailMode_ = nullptr;  // by property, or by report step
    QComboBox*      detailPick_ = nullptr;  // which property / which step
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
