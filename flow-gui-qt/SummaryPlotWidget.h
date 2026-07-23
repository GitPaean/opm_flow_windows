/*
  SummaryPlotWidget - plot summary vectors (FOPR, WBHP, ...) from a run's
  SMSPEC/UNSMRY files via opm-common's EclIO::ESmry, with a grouped/filtered
  vector selector (by category, type and item) and live refresh while a
  simulation is still writing.

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QWidget>
#include <QString>
#include <QVector>

#include <map>
#include <memory>

#include <opm/io/eclipse/SummaryNode.hpp>

class QChart;
class QChartView;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QTimer;

namespace Opm { namespace EclIO { class ESmry; } }

class SummaryPlotWidget : public QWidget
{
public:
    SummaryPlotWidget(QWidget* parent = nullptr);
    ~SummaryPlotWidget();

    // Register a run so it appears in the case selector. Called when a job
    // starts (the SMSPEC appears shortly after); missing files are fine.
    void addCase(const QString& label, const QString& smspecPath);

    // Make the given registered case the active one (no-op if unknown).
    void activateCase(const QString& smspecPath);

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
    QChartView*  chartView_ = nullptr;
    QChart*      chart_     = nullptr;
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
    void setStatus(const QString& s);
    static QString friendlyName(const QString& keyword,
                                Opm::EclIO::SummaryNode::Category cat);
};
