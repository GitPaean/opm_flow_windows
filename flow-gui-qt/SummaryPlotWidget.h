/*
  SummaryPlotWidget - plot summary vectors (FOPR, WBHP, ...) from a run's
  SMSPEC/UNSMRY files via opm-common's EclIO::ESmry, with live refresh
  while a simulation is writing.

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QWidget>
#include <QString>

#include <memory>

class QChart;
class QChartView;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
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
    QComboBox*   caseBox_   = nullptr;
    QLineEdit*   filter_    = nullptr;
    QListWidget* keyList_   = nullptr;
    QChartView*  chartView_ = nullptr;
    QChart*      chart_     = nullptr;
    QCheckBox*   autoRef_   = nullptr;
    QLabel*      status_    = nullptr;
    QTimer*      timer_     = nullptr;

    std::unique_ptr<Opm::EclIO::ESmry> smry_;
    QString loadedPath_;

    void browseCase();
    void reload(bool keepSelection);
    void applyFilter();
    void replot();
    void setStatus(const QString& s);
};
