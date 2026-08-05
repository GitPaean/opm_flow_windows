/*
  SummaryPlotWidget - plot summary vectors (FOPR, WBHP, ...) from a run's
  SMSPEC/UNSMRY files via opm-common's EclIO::ESmry, with a grouped/filtered
  vector selector (by category, type and item) and live refresh while a
  simulation is still writing.

  Copyright (C) 2026 SINTEF Digital

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QWidget>
#include <QString>
#include <QVector>

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <opm/io/eclipse/SummaryNode.hpp>

class QAbstractAxis;
class QChart;
class QChartView;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QJsonObject;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;
class QTimer;

namespace Opm { namespace EclIO { class ESmry; } }

class SummaryPlotWidget : public QWidget
{
    Q_OBJECT
public:
    SummaryPlotWidget(QWidget* parent = nullptr);
    ~SummaryPlotWidget();

    // Register a run so it appears in the case selector. Called when a job
    // starts (the SMSPEC appears shortly after); missing files are fine.
    void addCase(const QString& label, const QString& smspecPath, bool checked = true);

    // Make the given registered case the active one (no-op if unknown).
    void activateCase(const QString& smspecPath);

    // Give a case the name the user chose for it (restored from a project or
    // a session): it replaces the automatic label and is never tagged.
    void setCaseLabel(const QString& smspecPath, const QString& label);

    // The job writing this case just finished: reload if it is the active
    // case (it was registered at job start, possibly before the SMSPEC
    // existed), else refresh the comparison curves it may contribute to.
    void caseFinished(const QString& smspecPath);

    // Project support: enumerate / clear the loaded cases.
    struct CaseInfo { QString label; QString path; bool checked; };
    QList<CaseInfo> caseInfos() const;
    void clearCases();

    // Everything needed to come back to this plot in a later session: the
    // cases and which one is active, the vector filters, what each subplot
    // shows, and how it is drawn. Stored in the project file and, on exit,
    // in the settings. Missing entries keep their current value, so a state
    // written by an older version still restores what it does carry.
    QJsonObject uiState() const;
    void restoreUiState(const QJsonObject& state);

signals:
    // Emitted for every newly registered case (dedup already applied) so
    // other views (e.g. the 3D viewer) can mirror the case list.
    void caseAdded(const QString& label, const QString& smspecPath);
    // A case was renamed, so mirrors of the list (the 3D tab) can follow.
    void caseRenamed(const QString& smspecPath, const QString& label);
    // A case was dropped from the list; mirrors drop it too rather than keep
    // growing with cases this tab no longer knows about.
    void caseRemoved(const QString& smspecPath);

protected:
    // The active case's files may have appeared while the tab was hidden.
    void showEvent(QShowEvent* ev) override;

private:
    // one plottable summary vector, parsed from an ESmry SummaryNode
    struct Vec {
        Opm::EclIO::SummaryNode node;               // used for get()/get_unit()
        QString key;                                // identity + display, e.g. WOPR:P1
        QString keyword;                            // WOPR
        QString item;                               // full item, e.g. C-1H:26,44,3
        QString itemMain;                           // first level, e.g. C-1H
        QString itemSub;                            // second level, e.g. 26,44,3 ("" if none)
        QString unit;                               // e.g. SM3/DAY
        Opm::EclIO::SummaryNode::Category cat{};
        Opm::EclIO::SummaryNode::Type     type{};
    };

    QListWidget* caseList_   = nullptr;   // checkable: checked = plotted;
                                          // highlighted row = active case
    QComboBox*   catBox_     = nullptr;
    QComboBox*   typeBox_    = nullptr;
    QComboBox*   itemBox_    = nullptr;
    QLabel*      subLabel_   = nullptr;
    QComboBox*   subItemBox_ = nullptr;
    QLineEdit*   filter_    = nullptr;
    QTreeWidget* tree_      = nullptr;
    QComboBox*   layoutBox_ = nullptr;   // subplot layout: 1 / 2x1 / 2x2
    QComboBox*   legendBox_ = nullptr;   // legend placement, incl. inside corners
    QWidget*     chartArea_ = nullptr;   // grid container holding the subplots
    QGridLayout* chartGrid_ = nullptr;
    QVector<QChart*>     charts_;        // fixed pool of 4, shown as needed
    QVector<QChartView*> chartViews_;
    int          visibleCharts_ = 1;
    // Per-subplot selection (vector keys, e.g. "WBHP:B-1H"). The tree edits
    // the FOCUSED subplot's list; clicking a subplot moves the focus.
    QVector<QStringList> chartSel_;
    int          focusChart_ = 0;
    bool         syncingTree_ = false;   // guard: programmatic tree reselect

    // A subplot's zoomed-in axis ranges, kept across refreshes so live
    // updates do not yank the view; cleared by the Reset zoom button.
    struct ZoomSnap {
        bool valid = false;
        bool dates = false;              // x was a date axis (ms since epoch)
        double xmin = 0, xmax = 0;
        bool hasL = false; double lmin = 0, lmax = 0;
        bool hasR = false; double rmin = 0, rmax = 0;
    };
    QVector<ZoomSnap> zoomSnap_;

    // A floating legend the user dragged: position per chart, normalised to
    // the chart rect so it survives a resize. Null = park it in the corner
    // the placement box asks for.
    QVector<QPointF> legendPos_;
    int      legendDrag_ = -1;    // chart whose legend is being dragged
    QPointF  legendGrab_;         // cursor offset inside the legend
    // Chart-local position of a viewport event, or a null point if unmapped.
    QPointF chartPos(int idx, const QPoint& viewportPos) const;
    ZoomSnap captureZoom(QChart* chart) const;
    void applyZoom(QChart* chart, const ZoomSnap& z);
    QCheckBox*   autoRef_   = nullptr;
    QCheckBox*   dateAxis_  = nullptr;
    QCheckBox*   markers_   = nullptr;   // show data points on the curves
    QDoubleSpinBox* lineWidthSpin_   = nullptr;   // curve pen width, points
    QDoubleSpinBox* markerSizeSpin_  = nullptr;   // data-point marker size, px
    QSpinBox*       markerEverySpin_ = nullptr;   // mark every n-th point (1 = all)
    QLabel*      status_    = nullptr;
    QTimer*      timer_     = nullptr;

    std::unique_ptr<Opm::EclIO::ESmry> smry_;   // the ACTIVE case
    QString smryPath_;                           // ... and the case it holds:
                                                 // a failed read of ANOTHER
                                                 // case must not leave this
                                                 // one on screen under its name
    int         checkedCount_ = 0;   // cases asked for by the last replot
    QStringList unreadable_;         // ... of those, the ones that would not open
    QVector<Vec> vecs_;                          // parsed from the active case
    int nx_ = 0, ny_ = 0, nz_ = 0;   // grid dims from SMSPEC DIMENS (0 = unknown)
    // lazily-opened other cases for comparison plots (path -> reader);
    // cleared on every reload so refreshes see fresh data
    std::map<QString, std::unique_ptr<Opm::EclIO::ESmry>> others_;

    QString activePath() const;
    QString activeLabel() const;
    void caseItemChanged(QListWidgetItem* it);   // check toggle or rename
    // Name every case by what tells it apart: its own name while that is
    // unique in the list, otherwise the name plus the part of its path that
    // separates it from the cases sharing that name. Run over the whole list
    // after every add, remove or rename.
    void relabelCases();
    void removeCurrentCase();
    void clearActiveCase();
    void browseCase();
    void reload(bool keepSelection);
    void rebuildFilters();
    void populateItemBox();
    void populateSubItemBox();
    void rebuildTree(const QStringList& reselect);
    void replot();
    void savePng();
    void saveCsv();
    // Put the legend where legendBox_ asks: docked to an edge, floating in a
    // corner of the plot area, or hidden.
    void placeLegend(QChart* chart);
    // Print-oriented chart/axis cosmetics shared by every subplot.
    static void styleChart(QChart* chart);
    static void styleAxis(QAbstractAxis* axis);
    void setStatus(const QString& s);
    // Every checked case, the active one first-hand, others opened lazily.
    std::vector<std::pair<QString, Opm::EclIO::ESmry*>> checkedCases();
    void applyChartLayout(int n);   // n = 1, 2 or 4 visible subplots
    void setFocusChart(int i);      // focus subplot i, mirror its keys in the tree
    void updateChartFrames();       // border highlight on the focused subplot
    // Build one chart from the given vecs_ indices; returns the number of
    // vectors skipped because the chart already carries two units.
    int  plotChart(QChart* chart, const QList<int>& sel, const QString& title,
                   const std::vector<std::pair<QString, Opm::EclIO::ESmry*>>& plotCases);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;   // subplot click -> focus
    static QString friendlyName(const QString& keyword,
                                Opm::EclIO::SummaryNode::Category cat);
};
