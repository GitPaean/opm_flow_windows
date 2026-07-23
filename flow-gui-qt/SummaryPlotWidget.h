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

#include <memory>

#include <opm/io/eclipse/SummaryNode.hpp>

class QChart;
class QChartView;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
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

private:
    // one plottable summary vector, parsed from an ESmry SummaryNode
    struct Vec {
        Opm::EclIO::SummaryNode node;               // used for get()/get_unit()
        QString key;                                // identity + display, e.g. WOPR:P1
        QString keyword;                            // WOPR
        QString item;                               // P1 / region no. / "" for field
        QString unit;                               // e.g. SM3/DAY
        Opm::EclIO::SummaryNode::Category cat{};
        Opm::EclIO::SummaryNode::Type     type{};
    };

    QComboBox*   caseBox_   = nullptr;
    QComboBox*   catBox_    = nullptr;
    QComboBox*   typeBox_   = nullptr;
    QComboBox*   itemBox_   = nullptr;
    QLineEdit*   filter_    = nullptr;
    QTreeWidget* tree_      = nullptr;
    QChartView*  chartView_ = nullptr;
    QChart*      chart_     = nullptr;
    QCheckBox*   autoRef_   = nullptr;
    QLabel*      status_    = nullptr;
    QTimer*      timer_     = nullptr;

    std::unique_ptr<Opm::EclIO::ESmry> smry_;
    QVector<Vec> vecs_;
    int nx_ = 0, ny_ = 0, nz_ = 0;   // grid dims from SMSPEC DIMENS (0 = unknown)

    void browseCase();
    void reload(bool keepSelection);
    void rebuildFilters();
    void populateItemBox();
    void rebuildTree(const QStringList& reselect);
    void replot();
    void setStatus(const QString& s);
    static QString friendlyName(const QString& keyword,
                                Opm::EclIO::SummaryNode::Category cat);
};
