/*
  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  RestartCompare implementation. Part of the opm_flow_windows harness;
  GPL v3+ (see repository LICENSE).
*/
#include "RestartCompare.h"

#include "CasePath.h"

#include <QCheckBox>
#include <QChart>
#include <QChartView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineSeries>
#include <QLogValueAxis>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QTimer>
#include <QValueAxis>
#include <QVBoxLayout>

#include <opm/io/eclipse/ERst.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <set>

namespace {

QString unrstOf(const QString& smspecPath)
{
    QString base = flowgui::normalizeCasePath(smspecPath);
    if (base.endsWith(QStringLiteral(".SMSPEC"), Qt::CaseInsensitive)) base.chop(7);
    return base + QStringLiteral(".UNRST");
}

// Arrays worth comparing: the per-cell float fields. The headers (INTEHEAD,
// LOGIHEAD, DOUBHEAD) and the character arrays are metadata, and the well
// arrays are both integer-coded and already answered better by the summary
// tab, so a difference in them is reported by name but not reduced to a curve.
bool isCellField(const std::string& name, Opm::EclIO::eclArrType t, std::int64_t len,
                 std::int64_t activeCells)
{
    if (t != Opm::EclIO::REAL && t != Opm::EclIO::DOUB) return false;
    if (activeCells > 0 && len != activeCells) return false;
    const QString n = QString::fromStdString(name).trimmed();
    return n != QStringLiteral("DOUBHEAD");
}

} // namespace

namespace flowgui {

bool CompareResult::identical() const
{
    if (!ran) return false;
    if (!stepsOnlyInA.isEmpty() || !stepsOnlyInB.isEmpty()) return false;
    if (!kwOnlyInA.isEmpty() || !kwOnlyInB.isEmpty()) return false;
    for (const auto& k : keywords) if (!k.clean()) return false;
    return true;
}

QString CompareResult::verdict() const
{
    if (!ran) return cancelled ? QStringLiteral("cancelled") : problem;
    if (identical())
        return QStringLiteral("identical over %1 report step(s), %2 propert(y/ies)")
                   .arg(steps.size()).arg(keywords.size());

    // The first thing that parted company, which is the fact worth leading on.
    int firstStep = -1;
    QString firstKw;
    long bad = 0;
    for (const auto& k : keywords) {
        bad += k.totalBad;
        if (k.firstBadSeqnum >= 0 && (firstStep < 0 || k.firstBadSeqnum < firstStep)) {
            firstStep = k.firstBadSeqnum;
            firstKw   = k.keyword;
        }
    }
    QStringList bits;
    if (firstStep >= 0)
        bits << QStringLiteral("first differs at report step %1 in %2")
                    .arg(firstStep).arg(firstKw);
    if (bad > 0)
        bits << QStringLiteral("%1 cell value(s) outside tolerance").arg(bad);
    if (!stepsOnlyInA.isEmpty() || !stepsOnlyInB.isEmpty())
        bits << QStringLiteral("%1/%2 step(s) only in A/B")
                    .arg(stepsOnlyInA.size()).arg(stepsOnlyInB.size());
    if (!kwOnlyInA.isEmpty() || !kwOnlyInB.isEmpty())
        bits << QStringLiteral("%1/%2 propert(y/ies) only in A/B")
                    .arg(kwOnlyInA.size()).arg(kwOnlyInB.size());
    return bits.join(QStringLiteral("; "));
}

CompareResult compareRestarts(const QString& smspecA, const QString& smspecB,
                              const DiffTol& tol,
                              std::atomic<int>* progress,
                              std::atomic<bool>* cancel)
{
    using namespace Opm::EclIO;
    CompareResult r;
    const QString pa = unrstOf(smspecA), pb = unrstOf(smspecB);
    const auto stop = [cancel] { return cancel && cancel->load(); };
    const auto tick = [progress](int p) { if (progress) progress->store(p); };

    if (!QFileInfo::exists(pa) || !QFileInfo::exists(pb)) {
        r.problem = QStringLiteral("no restart file for %1")
                        .arg(QFileInfo::exists(pa) ? QFileInfo(pb).fileName()
                                                   : QFileInfo(pa).fileName());
        return r;
    }

    std::unique_ptr<ERst> A, B;
    try {
        A = std::make_unique<ERst>(pa.toStdString());
        B = std::make_unique<ERst>(pb.toStdString());
    } catch (const std::exception& e) {
        r.problem = QStringLiteral("could not read a restart file: %1")
                        .arg(QString::fromLocal8Bit(e.what()));
        return r;
    }
    tick(2);

    // --- step alignment: by SEQNUM, never by index. Two runs need not have
    // written the same number of steps, and step 7 of a run that stopped early
    // is not step 7 of one that did not.
    const std::vector<int> sa = A->listOfReportStepNumbers();
    const std::vector<int> sb = B->listOfReportStepNumbers();
    const std::set<int> setA(sa.begin(), sa.end()), setB(sb.begin(), sb.end());
    for (int s : sa) { if (!setB.count(s)) r.stepsOnlyInA << s; else r.steps << s; }
    for (int s : sb) if (!setA.count(s)) r.stepsOnlyInB << s;
    if (r.steps.isEmpty()) {
        r.problem = QStringLiteral("no report step in common (A has %1, B has %2)")
                        .arg(sa.size()).arg(sb.size());
        return r;
    }

    // --- grid compatibility. The authority is the length of the cell arrays:
    // that IS the active cell count, and if it differs then cell i of one run
    // is not cell i of the other and every number below would be meaningless.
    // INTEHEAD is read too, purely to be able to say what the dimensions were.
    const int probe = r.steps.front();
    std::int64_t nActA = -1, nActB = -1;
    auto dimsOf = [&](ERst& f, std::int64_t& nAct) {
        QString dims;
        try {
            const auto& ih = f.getRestartData<int>("INTEHEAD", probe);
            if (ih.size() > 11) {
                dims = QStringLiteral("%1x%2x%3").arg(ih[8]).arg(ih[9]).arg(ih[10]);
                nAct = ih[11];
            }
        } catch (...) {}
        for (const auto& [name, type, len] : f.listOfRstArrays(probe))
            if (type == REAL && len > 1) { nAct = len; break; }
        return dims;
    };
    const QString dimA = dimsOf(*A, nActA), dimB = dimsOf(*B, nActB);
    if (nActA > 0 && nActB > 0 && nActA != nActB) {
        r.problem = QStringLiteral(
            "the two runs are on different grids - %1 active cells (%2) against "
            "%3 (%4). Cell values cannot be compared.")
            .arg(nActA).arg(dimA.isEmpty() ? QStringLiteral("?") : dimA)
            .arg(nActB).arg(dimB.isEmpty() ? QStringLiteral("?") : dimB);
        return r;
    }
    r.gridNote = QStringLiteral("grid %1, %2 active cells")
                     .arg(dimA.isEmpty() ? QStringLiteral("?") : dimA).arg(nActA);

    // --- property alignment, from the first common step.
    QStringList kwA, kwB;
    for (const auto& [name, type, len] : A->listOfRstArrays(probe))
        if (isCellField(name, type, len, nActA))
            kwA << QString::fromStdString(name).trimmed();
    for (const auto& [name, type, len] : B->listOfRstArrays(probe))
        if (isCellField(name, type, len, nActB))
            kwB << QString::fromStdString(name).trimmed();
    kwA.removeDuplicates(); kwB.removeDuplicates();
    QStringList common;
    for (const QString& k : kwA) { if (kwB.contains(k)) common << k; else r.kwOnlyInA << k; }
    for (const QString& k : kwB) if (!kwA.contains(k)) r.kwOnlyInB << k;
    if (common.isEmpty()) {
        r.problem = QStringLiteral("no comparable cell property in common");
        return r;
    }

    // --- the pass itself. One array from each side at a time, reduced to
    // three numbers and thrown away, so peak memory is two arrays however long
    // the run is.
    const int totalUnits = common.size() * r.steps.size();
    int done = 0;
    for (const QString& kw : common) {
        KeywordDiff kd;
        kd.keyword = kw;
        const std::string k = kw.toStdString();
        for (int seq : r.steps) {
            if (stop()) { r.cancelled = true; return r; }
            StepDiff sd;
            sd.seqnum = seq;
            try {
                if (A->hasArray(k, seq) && B->hasArray(k, seq)) {
                    const std::vector<float>& va = A->getRestartData<float>(k, seq);
                    const std::vector<float>& vb = B->getRestartData<float>(k, seq);
                    const std::size_t n = std::min(va.size(), vb.size());
                    double sq = 0.0;
                    for (std::size_t i = 0; i < n; ++i) {
                        const double a = va[i], b = vb[i];
                        const double d = std::abs(a - b);
                        sq += d * d;
                        if (d > sd.maxAbs) {
                            sd.maxAbs = d; sd.worstCell = int(i);
                            sd.aWorst = a; sd.bWorst = b;
                        }
                        if (diffIsSignificant(a, b, tol)) ++sd.nBad;
                    }
                    if (n) sd.rms = std::sqrt(sq / double(n));
                }
            } catch (...) {
                // An unreadable array is not a difference; leave the step at
                // zero rather than inventing one.
            }
            if (sd.nBad > 0 && kd.firstBadSeqnum < 0) kd.firstBadSeqnum = seq;
            kd.totalBad += sd.nBad;
            kd.maxAbsOverall = std::max(kd.maxAbsOverall, sd.maxAbs);
            kd.steps.push_back(sd);
            tick(2 + int(96.0 * double(++done) / double(std::max(1, totalUnits))));
        }
        // Both readers cache what they load; a whole restart would otherwise
        // accumulate across properties.
        A->clearData(); B->clearData();
        r.keywords.push_back(kd);
    }
    tick(100);
    r.ran = true;
    return r;
}

} // namespace flowgui

// ===========================================================================
// RestartCompareDialog
// ===========================================================================
namespace flowgui {

RestartCompareDialog::RestartCompareDialog(const QVector<CaseEntry>& cases, QWidget* parent)
    : QDialog(parent), cases_(cases)
{
    setWindowTitle(QStringLiteral("Compare restarts"));
    setModal(false);
    resize(1000, 700);

    caseA_ = new QComboBox; caseB_ = new QComboBox;
    for (const auto& c : cases_) {
        caseA_->addItem(c.label);
        caseB_->addItem(c.label);
    }
    if (caseB_->count() > 1) caseB_->setCurrentIndex(1);

    absTol_ = new QDoubleSpinBox;
    absTol_->setDecimals(10); absTol_->setRange(0.0, 1e6);
    absTol_->setValue(1e-4); absTol_->setSingleStep(1e-4);
    absTol_->setToolTip(QStringLiteral(
        "absolute tolerance, as compareECL takes it. A pair of values differs "
        "only when BOTH this and the relative tolerance are exceeded - which "
        "is what keeps floating-point noise on a large value from counting."));
    relTol_ = new QDoubleSpinBox;
    relTol_->setDecimals(10); relTol_->setRange(0.0, 1.0);
    relTol_->setValue(1e-4); relTol_->setSingleStep(1e-4);
    relTol_->setToolTip(QStringLiteral(
        "relative tolerance (0..1). Ignored for a pair where one value is "
        "zero: there is no ratio to take, so the absolute tolerance decides "
        "on its own."));

    runBtn_ = new QPushButton(QStringLiteral("Compare"));
    bar_ = new QProgressBar; bar_->setRange(0, 100); bar_->setValue(0);
    bar_->setVisible(false);

    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("A:")));   top->addWidget(caseA_, 1);
    top->addWidget(new QLabel(QStringLiteral("B:")));   top->addWidget(caseB_, 1);
    top->addWidget(new QLabel(QStringLiteral("abs:"))); top->addWidget(absTol_);
    top->addWidget(new QLabel(QStringLiteral("rel:"))); top->addWidget(relTol_);
    top->addWidget(runBtn_);

    verdict_ = new QLabel(QStringLiteral("pick two cases and press Compare"));
    QFont vf = verdict_->font(); vf.setBold(true); vf.setPointSizeF(vf.pointSizeF() + 1.5);
    verdict_->setFont(vf);
    verdict_->setWordWrap(true);
    note_ = new QLabel;
    note_->setWordWrap(true);
    note_->setStyleSheet(QStringLiteral("color:#555b61;"));

    metric_ = new QComboBox;
    metric_->addItem(QStringLiteral("cells outside tolerance"));
    metric_->addItem(QStringLiteral("max |A-B|"));
    metric_->addItem(QStringLiteral("RMS of A-B"));
    metric_->setToolTip(QStringLiteral(
        "what to plot per report step.\n\n"
        "The cell count is usually the most telling: a single pathological "
        "cell dominates max|A-B|, while a count going 0, 0, 3, 47, 1200 shows "
        "both when the runs parted and how fast they are separating."));
    onlyBad_ = new QCheckBox(QStringLiteral("only properties that differ"));
    onlyBad_->setChecked(true);
    onlyBad_->setToolTip(QStringLiteral(
        "hide the properties that agree everywhere - usually most of them"));

    auto* mrow = new QHBoxLayout;
    mrow->addWidget(new QLabel(QStringLiteral("Plot:")));
    mrow->addWidget(metric_);
    mrow->addWidget(onlyBad_);
    mrow->addStretch(1);

    chart_ = new QChart;
    chart_->setBackgroundRoundness(0);
    chart_->setDropShadowEnabled(false);
    chart_->legend()->setAlignment(Qt::AlignBottom);
    chartView_ = new QChartView(chart_);
    chartView_->setRenderHint(QPainter::Antialiasing);

    table_ = new QTableWidget(0, 4);
    table_->setHorizontalHeaderLabels({ QStringLiteral("Property"),
                                        QStringLiteral("First differs at step"),
                                        QStringLiteral("Cells outside tol"),
                                        QStringLiteral("max |A-B|") });
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setMaximumHeight(200);

    auto* split = new QSplitter(Qt::Vertical);
    split->addWidget(chartView_);
    split->addWidget(table_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(top);
    lay->addWidget(bar_);
    lay->addWidget(verdict_);
    lay->addWidget(note_);
    lay->addLayout(mrow);
    lay->addWidget(split, 1);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::close);
    lay->addWidget(bb);

    connect(runBtn_, &QPushButton::clicked, this, [this] { startCompare(); });
    connect(metric_, &QComboBox::currentIndexChanged, this, [this](int) { replot(); });
    connect(onlyBad_, &QCheckBox::toggled, this, [this](bool) { replot(); });

    poll_ = new QTimer(this);
    poll_->setInterval(120);
    connect(poll_, &QTimer::timeout, this, [this] { bar_->setValue(progress_.load()); });
}

RestartCompareDialog::~RestartCompareDialog()
{
    // A worker still reading two restarts has to be told to stop and waited
    // for: it writes into members of this dialog.
    cancel_.store(true);
    if (worker_) { worker_->quit(); worker_->wait(5000); }
}

void RestartCompareDialog::startCompare()
{
    if (worker_) return;
    const int ia = caseA_->currentIndex(), ib = caseB_->currentIndex();
    if (ia < 0 || ib < 0) return;
    if (ia == ib) {
        verdict_->setText(QStringLiteral("A and B are the same case"));
        return;
    }
    const QString a = cases_[ia].smspec, b = cases_[ib].smspec;
    const DiffTol tol{ absTol_->value(), relTol_->value() };

    cancel_.store(false);
    progress_.store(0);
    bar_->setValue(0); bar_->setVisible(true);
    runBtn_->setEnabled(false);
    verdict_->setText(QStringLiteral("comparing..."));
    note_->clear();
    poll_->start();

    worker_ = QThread::create([this, a, b, tol] {
        result_ = compareRestarts(a, b, tol, &progress_, &cancel_);
    });
    connect(worker_, &QThread::finished, this, [this] { finishCompare(); });
    worker_->start();
}

void RestartCompareDialog::finishCompare()
{
    poll_->stop();
    bar_->setVisible(false);
    runBtn_->setEnabled(true);
    if (worker_) { worker_->deleteLater(); worker_ = nullptr; }
    showResult();
}

void RestartCompareDialog::showResult()
{
    const bool same = result_.identical();
    verdict_->setText(result_.verdict());
    verdict_->setStyleSheet(!result_.ran ? QStringLiteral("color:#8a1f1f;")
                            : same       ? QStringLiteral("color:#05814f;")
                                         : QStringLiteral("color:#a8500d;"));

    QStringList notes;
    if (!result_.gridNote.isEmpty()) notes << result_.gridNote;
    if (!result_.steps.isEmpty())
        notes << QStringLiteral("%1 common report step(s)").arg(result_.steps.size());
    auto listNote = [&notes](const QString& what, const QStringList& v) {
        if (v.isEmpty()) return;
        notes << QStringLiteral("%1: %2").arg(what,
                  v.mid(0, 6).join(QStringLiteral(", "))
                  + (v.size() > 6 ? QStringLiteral(" +%1 more").arg(v.size() - 6)
                                  : QString()));
    };
    listNote(QStringLiteral("only in A"), result_.kwOnlyInA);
    listNote(QStringLiteral("only in B"), result_.kwOnlyInB);
    if (!result_.stepsOnlyInA.isEmpty())
        notes << QStringLiteral("%1 step(s) only in A").arg(result_.stepsOnlyInA.size());
    if (!result_.stepsOnlyInB.isEmpty())
        notes << QStringLiteral("%1 step(s) only in B").arg(result_.stepsOnlyInB.size());
    note_->setText(notes.join(QStringLiteral("   |   ")));

    table_->setRowCount(0);
    for (const auto& k : result_.keywords) {
        const int row = table_->rowCount();
        table_->insertRow(row);
        table_->setItem(row, 0, new QTableWidgetItem(k.keyword));
        table_->setItem(row, 1, new QTableWidgetItem(
            k.firstBadSeqnum < 0 ? QStringLiteral("-")
                                 : QString::number(k.firstBadSeqnum)));
        table_->setItem(row, 2, new QTableWidgetItem(QString::number(k.totalBad)));
        table_->setItem(row, 3, new QTableWidgetItem(
            QStringLiteral("%1").arg(k.maxAbsOverall, 0, 'g', 4)));
        if (!k.clean())
            for (int c = 0; c < 4; ++c)
                table_->item(row, c)->setForeground(QBrush(QColor(0xa8, 0x50, 0x0d)));
    }
    replot();
}

void RestartCompareDialog::replot()
{
    chart_->removeAllSeries();
    for (auto* ax : chart_->axes()) chart_->removeAxis(ax);
    if (!result_.ran || result_.keywords.isEmpty()) { chart_->setTitle(QString()); return; }

    const int metric = metric_->currentIndex();
    const bool onlyBad = onlyBad_->isChecked();
    double ymax = 0.0;
    double xlo = 0.0, xhi = 0.0;
    bool any = false;
    int shown = 0, hidden = 0;

    // Zero is both common and meaningful here - it is what agreement looks
    // like - and a log axis has nothing to say about it. Rather than plot a
    // fake floor value, a step with nothing to report is left out: the curve
    // then begins exactly where that property first differs, which is the
    // reading wanted anyway. Said in the title, so an absence is not mistaken
    // for missing data.
    int omitted = 0;
    for (const auto& k : result_.keywords) {
        if (onlyBad && k.clean()) { ++hidden; continue; }
        auto* s = new QLineSeries;
        s->setName(k.keyword);
        int pts = 0;
        for (const auto& sd : k.steps) {
            const double y = metric == 0 ? double(sd.nBad)
                           : metric == 1 ? sd.maxAbs
                                         : sd.rms;
            if (y <= 0.0) { ++omitted; continue; }
            s->append(double(sd.seqnum), y);
            ymax = std::max(ymax, y);
            if (!any) { xlo = xhi = double(sd.seqnum); }
            xlo = std::min(xlo, double(sd.seqnum));
            xhi = std::max(xhi, double(sd.seqnum));
            ++pts;
            any = true;
        }
        if (pts == 0) { delete s; ++hidden; continue; }   // nothing to draw
        chart_->addSeries(s);
        ++shown;
    }
    if (!any) {
        chart_->setTitle(onlyBad && hidden
            ? QStringLiteral("every property agrees within tolerance")
            : QStringLiteral("nothing to plot"));
        return;
    }
    QString sub = QStringLiteral("%1 - %2 propert(y/ies)")
                      .arg(metric_->currentText()).arg(shown);
    if (hidden)  sub += QStringLiteral(", %1 agreeing hidden").arg(hidden);
    if (omitted) sub += QStringLiteral(" (log axis: %1 zero point(s) not drawn)")
                            .arg(omitted);
    chart_->setTitle(sub);

    auto* ax = new QValueAxis;
    ax->setTitleText(QStringLiteral("report step (SEQNUM)"));
    ax->setLabelFormat(QStringLiteral("%.0f"));
    // Ranged from the data. A manually created axis does not follow the series
    // it is attached to, and left at its default the run's later steps fall off
    // the end of the chart without saying so.
    ax->setRange(xlo, std::max(xhi, xlo + 1.0));
    chart_->addAxis(ax, Qt::AlignBottom);

    // A divergence climbs by orders of magnitude, so a linear axis shows the
    // last step and nothing before it. Log where the data allows.
    QAbstractAxis* ay = nullptr;
    if (ymax > 0.0) {
        auto* la = new QLogValueAxis;
        la->setBase(10.0);
        la->setLabelFormat(QStringLiteral("%g"));
        la->setRange(std::max(ymax * 1e-6, 1e-12), ymax * 1.5);
        ay = la;
    } else {
        auto* va = new QValueAxis;
        va->setRange(0.0, 1.0);
        ay = va;
    }
    ay->setTitleText(metric_->currentText());
    chart_->addAxis(ay, Qt::AlignLeft);
    for (auto* s : chart_->series()) { s->attachAxis(ax); s->attachAxis(ay); }
}

} // namespace flowgui
