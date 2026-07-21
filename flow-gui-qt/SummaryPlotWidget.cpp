/*
  SummaryPlotWidget implementation. Part of the opm_flow_windows harness;
  GPL v3+ (see repository LICENSE).
*/
#include "SummaryPlotWidget.h"

#include <opm/io/eclipse/ESmry.hpp>

#include <QChart>
#include <QChartView>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLineSeries>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <QValueAxis>

#include <algorithm>
#include <exception>
#include <string>
#include <vector>

SummaryPlotWidget::SummaryPlotWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* top = new QVBoxLayout(this);

    // --- case row ------------------------------------------------------------
    {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("Case:")));
        caseBox_ = new QComboBox;
        caseBox_->setMinimumWidth(320);
        row->addWidget(caseBox_, 1);
        auto* bbrowse = new QPushButton(QStringLiteral("Open SMSPEC..."));
        auto* brefresh = new QPushButton(QStringLiteral("Refresh"));
        autoRef_ = new QCheckBox(QStringLiteral("auto-refresh (10 s)"));
        row->addWidget(bbrowse);
        row->addWidget(brefresh);
        row->addWidget(autoRef_);
        top->addLayout(row);

        connect(bbrowse, &QPushButton::clicked, this, [this] { browseCase(); });
        connect(brefresh, &QPushButton::clicked, this, [this] { reload(true); });
        connect(caseBox_, &QComboBox::currentIndexChanged, this,
                [this](int) { reload(false); });
        timer_ = new QTimer(this);
        timer_->setInterval(10000);
        connect(timer_, &QTimer::timeout, this, [this] { reload(true); });
        connect(autoRef_, &QCheckBox::toggled, this, [this](bool on) {
            if (on) timer_->start(); else timer_->stop();
        });
    }

    // --- key list + chart ----------------------------------------------------
    auto* split = new QSplitter;
    {
        auto* left = new QWidget;
        auto* ll = new QVBoxLayout(left);
        ll->setContentsMargins(0, 0, 0, 0);
        filter_ = new QLineEdit;
        filter_->setPlaceholderText(QStringLiteral("filter, e.g. FOPR or WBHP:*"));
        keyList_ = new QListWidget;
        keyList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        ll->addWidget(filter_);
        ll->addWidget(keyList_, 1);
        split->addWidget(left);

        chart_ = new QChart;
        chart_->legend()->setVisible(true);
        chartView_ = new QChartView(chart_);
        chartView_->setRenderHint(QPainter::Antialiasing);
        split->addWidget(chartView_);
        split->setStretchFactor(0, 0);
        split->setStretchFactor(1, 1);
        split->setSizes({ 260, 700 });
        top->addWidget(split, 1);

        connect(filter_, &QLineEdit::textChanged, this, [this] { applyFilter(); });
        connect(keyList_, &QListWidget::itemSelectionChanged, this, [this] { replot(); });
    }

    status_ = new QLabel;
    top->addWidget(status_);
    setStatus(QStringLiteral("run a job (or open an SMSPEC) to plot summary vectors"));
}

SummaryPlotWidget::~SummaryPlotWidget() = default;

void SummaryPlotWidget::setStatus(const QString& s) { status_->setText(s); }

void SummaryPlotWidget::addCase(const QString& label, const QString& smspecPath)
{
    for (int i = 0; i < caseBox_->count(); ++i)
        if (caseBox_->itemData(i).toString() == smspecPath) return;
    caseBox_->addItem(label, smspecPath);
    if (caseBox_->count() == 1) caseBox_->setCurrentIndex(0);
}

void SummaryPlotWidget::browseCase()
{
    const QString f = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open summary specification"), QString(),
        QStringLiteral("Summary spec (*.SMSPEC);;All files (*)"));
    if (!f.isEmpty()) {
        addCase(QFileInfo(f).completeBaseName(), f);
        caseBox_->setCurrentIndex(caseBox_->count() - 1);
    }
}

void SummaryPlotWidget::reload(bool keepSelection)
{
    const QString path = caseBox_->currentData().toString();
    if (path.isEmpty()) return;
    if (!QFileInfo::exists(path)) {
        setStatus(QStringLiteral("waiting for %1 ...").arg(path));
        return;
    }

    QStringList selected;
    if (keepSelection)
        for (auto* it : keyList_->selectedItems()) selected << it->text();

    // Re-open for a fresh snapshot; while flow is still writing, a read can
    // transiently fail - keep the previous data and try again next refresh.
    try {
        smry_ = std::make_unique<Opm::EclIO::ESmry>(path.toStdString());
        loadedPath_ = path;
    } catch (const std::exception& e) {
        setStatus(QStringLiteral("could not read summary (still being written?): %1")
                      .arg(QString::fromLocal8Bit(e.what())));
        return;
    }

    keyList_->blockSignals(true);
    keyList_->clear();
    for (const auto& k : smry_->keywordList())
        keyList_->addItem(QString::fromStdString(k));
    applyFilter();
    if (!selected.isEmpty())
        for (int i = 0; i < keyList_->count(); ++i)
            if (selected.contains(keyList_->item(i)->text()))
                keyList_->item(i)->setSelected(true);
    keyList_->blockSignals(false);

    setStatus(QStringLiteral("%1: %2 vectors, %3 timesteps")
                  .arg(QFileInfo(path).completeBaseName())
                  .arg(keyList_->count())
                  .arg(smry_ ? int(smry_->numberOfTimeSteps()) : 0));
    replot();
}

void SummaryPlotWidget::applyFilter()
{
    const QString pat = filter_->text().trimmed();
    const QRegularExpression rx(
        QRegularExpression::wildcardToRegularExpression(
            pat.contains(QLatin1Char('*')) || pat.contains(QLatin1Char('?'))
                ? pat : QLatin1Char('*') + pat + QLatin1Char('*')),
        QRegularExpression::CaseInsensitiveOption);
    for (int i = 0; i < keyList_->count(); ++i) {
        auto* it = keyList_->item(i);
        it->setHidden(!pat.isEmpty() && !rx.match(it->text()).hasMatch());
    }
}

void SummaryPlotWidget::replot()
{
    chart_->removeAllSeries();
    const auto axes = chart_->axes();
    for (auto* a : axes) chart_->removeAxis(a);
    if (!smry_) return;

    std::vector<float> time;
    try { time = smry_->get("TIME"); }
    catch (...) { setStatus(QStringLiteral("no TIME vector in summary")); return; }

    double ymin = 0, ymax = 0; bool first = true;
    int plotted = 0;
    for (auto* it : keyList_->selectedItems()) {
        const std::string key = it->text().toStdString();
        std::vector<float> v;
        try { v = smry_->get(key); } catch (...) { continue; }
        auto* s = new QLineSeries;
        s->setName(it->text());
        const size_t n = std::min(time.size(), v.size());
        for (size_t i = 0; i < n; ++i) {
            s->append(time[i], v[i]);
            if (first) { ymin = ymax = v[i]; first = false; }
            ymin = std::min<double>(ymin, v[i]);
            ymax = std::max<double>(ymax, v[i]);
        }
        chart_->addSeries(s);
        ++plotted;
    }
    if (!plotted) { chart_->setTitle(QString()); return; }

    auto* ax = new QValueAxis; ax->setTitleText(QStringLiteral("time [days]"));
    auto* ay = new QValueAxis;
    if (ymax > ymin) ay->setRange(ymin - 0.05 * (ymax - ymin), ymax + 0.05 * (ymax - ymin));
    chart_->addAxis(ax, Qt::AlignBottom);
    chart_->addAxis(ay, Qt::AlignLeft);
    const auto series = chart_->series();
    for (auto* s : series) { s->attachAxis(ax); s->attachAxis(ay); }
    chart_->setTitle(caseBox_->currentText());
}
