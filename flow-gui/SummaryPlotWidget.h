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

class QChart;
class QChartView;
class QCheckBox;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
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

    // The job writing this case just finished: reload if it is the active
    // case (it was registered at job start, possibly before the SMSPEC
    // existed), else refresh the comparison curves it may contribute to.
    void caseFinished(const QString& smspecPath);

    // Project support: enumerate / clear the loaded cases.
    struct CaseInfo { QString label; QString path; bool checked; };
    QList<CaseInfo> caseInfos() const;
    void clearCases();

signals:
    // Emitted for every newly registered case (dedup already applied) so
    // other views (e.g. the 3D viewer) can mirror the case list.
    void caseAdded(const QString& label, const QString& smspecPath);

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
    QCheckBox*   autoRef_   = nullptr;
    QCheckBox*   dateAxis_  = nullptr;
    QCheckBox*   markers_   = nullptr;   // show data points on the curves
    QLabel*      status_    = nullptr;
    QTimer*      timer_     = nullptr;

    std::unique_ptr<Opm::EclIO::ESmry> smry_;   // the ACTIVE case
    QVector<Vec> vecs_;                          // parsed from the active case
    int nx_ = 0, ny_ = 0, nz_ = 0;   // grid dims from SMSPEC DIMENS (0 = unknown)
    // lazily-opened other cases for comparison plots (path -> reader);
    // cleared on every reload so refreshes see fresh data
    std::map<QString, std::unique_ptr<Opm::EclIO::ESmry>> others_;

    QString activePath() const;
    QString activeLabel() const;
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
