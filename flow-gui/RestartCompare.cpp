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
#include <QDateTimeAxis>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineSeries>
#include <QLogValueAxis>
#include <QMap>
#include <QTimeZone>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QTimer>
#include <QToolTip>
#include <QValueAxis>
#include <QVBoxLayout>

#include <opm/io/eclipse/EGrid.hpp>
#include <opm/io/eclipse/EInit.hpp>
#include <opm/io/eclipse/ERst.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>

namespace {

QString caseBase(const QString& smspecPath)
{
    QString base = flowgui::normalizeCasePath(smspecPath);
    if (base.endsWith(QStringLiteral(".SMSPEC"), Qt::CaseInsensitive)) base.chop(7);
    return base;
}

// INTEHEAD slots, named in opm-common's VectorItems/intehead.hpp.
constexpr int IH_NX = 8, IH_NY = 9, IH_NZ = 10, IH_NACTIV = 11;
constexpr int IH_DAY = 64, IH_MONTH = 65, IH_YEAR = 66;
constexpr int IH_HOUR = 206, IH_MIN = 207;

// The calendar date a restart step was written at - the key the two runs are
// paired on. Integers, so equal means equal and there is no tolerance to pick.
QDateTime dateOfStep(Opm::EclIO::ERst& f, int seqnum)
{
    try {
        const auto& ih = f.getRestartData<int>("INTEHEAD", seqnum);
        if (int(ih.size()) <= IH_YEAR) return {};
        const QDate d(ih[IH_YEAR], ih[IH_MONTH], ih[IH_DAY]);
        if (!d.isValid()) return {};
        const int hh = int(ih.size()) > IH_HOUR ? ih[IH_HOUR] : 0;
        const int mm = int(ih.size()) > IH_MIN  ? ih[IH_MIN]  : 0;
        return QDateTime(d, QTime(std::clamp(hh, 0, 23), std::clamp(mm, 0, 59)),
                         QTimeZone::utc());
    } catch (...) { return {}; }
}

// Pore volume per ACTIVE cell, for weighting the field averages.
//
// INIT writes PORV for every cell of the Cartesian grid, inactive ones
// included, so it usually has to be compressed through the grid's active-cell
// map before it lines up with a restart array. Empty on any failure, and the
// caller then falls back to an unweighted mean and says so.
std::vector<double> loadPorv(const QString& base, std::int64_t nActive)
{
    using namespace Opm::EclIO;
    std::vector<double> out;
    if (nActive <= 0) return out;
    const QString initPath = base + QStringLiteral(".INIT");
    if (!QFileInfo::exists(initPath)) return out;
    try {
        EInit init(initPath.toStdString());
        if (!init.hasKey("PORV")) return out;
        const std::vector<float>& porv = init.getInitData<float>("PORV");
        if (std::int64_t(porv.size()) == nActive) {
            out.assign(porv.begin(), porv.end());
            return out;
        }
        const QString egridPath = base + QStringLiteral(".EGRID");
        if (!QFileInfo::exists(egridPath)) return out;
        EGrid grid(egridPath.toStdString());
        if (std::int64_t(porv.size()) != std::int64_t(grid.totalNumberOfCells()))
            return out;
        out.resize(std::size_t(nActive), 0.0);
        for (std::int64_t a = 0; a < nActive; ++a) {
            const auto ijk = grid.ijk_from_active_index(int(a));
            const int g = grid.global_index(ijk[0], ijk[1], ijk[2]);
            if (g >= 0 && std::size_t(g) < porv.size())
                out[std::size_t(a)] = porv[std::size_t(g)];
        }
    } catch (...) { out.clear(); }
    return out;
}

// One real value per active cell. Headers and character arrays are metadata;
// the face/flux arrays have a length of their own and are meaningful
// elementwise but not per cell, so they are named and set aside rather than
// silently dropped.
bool isCellField(const std::string& name, Opm::EclIO::eclArrType t,
                 std::int64_t len, std::int64_t nActive)
{
    if (t != Opm::EclIO::REAL && t != Opm::EclIO::DOUB) return false;
    if (QString::fromStdString(name).trimmed() == QStringLiteral("DOUBHEAD")) return false;
    return nActive <= 0 || len == nActive;
}

QString dstr(const QDateTime& d) { return d.toString(QStringLiteral("yyyy-MM-dd")); }

} // namespace

namespace flowgui {

bool CompareResult::sameEnd() const
{
    return endA.isValid() && endB.isValid() && endA == endB;
}

bool CompareResult::identical() const
{
    if (!ran) return false;
    if (!timesOnlyInA.isEmpty() || !timesOnlyInB.isEmpty()) return false;
    if (!kwOnlyInA.isEmpty() || !kwOnlyInB.isEmpty()) return false;
    for (const auto& k : keywords) if (!k.clean()) return false;
    return true;
}

QString CompareResult::verdict() const
{
    if (!ran) return cancelled ? QStringLiteral("cancelled") : problem;

    QStringList bits;
    // A run that stopped early is the headline, not whatever it disagreed
    // about on the way there: the two are not the same experiment.
    if (!sameEnd() && endA.isValid() && endB.isValid())
        bits << QStringLiteral("A ends %1, B ends %2").arg(dstr(endA), dstr(endB));

    if (identical()) {
        bits.prepend(QStringLiteral("identical over %1 report date(s), %2 propert(y/ies)")
                         .arg(times.size()).arg(keywords.size()));
        return bits.join(QStringLiteral("; "));
    }

    QDateTime first;
    QString firstKw;
    long bad = 0;
    for (const auto& k : keywords) {
        bad += k.totalBad;
        if (k.firstBad.isValid() && (!first.isValid() || k.firstBad < first)) {
            first = k.firstBad; firstKw = k.keyword;
        }
    }
    if (first.isValid())
        bits.prepend(QStringLiteral("first differs %1 in %2").arg(dstr(first), firstKw));
    if (bad > 0)
        bits << QStringLiteral("%1 cell value(s) outside tolerance").arg(bad);
    if (!timesOnlyInA.isEmpty() || !timesOnlyInB.isEmpty())
        bits << QStringLiteral("%1/%2 report date(s) only in A/B")
                    .arg(timesOnlyInA.size()).arg(timesOnlyInB.size());
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
    const QString baseA = caseBase(smspecA), baseB = caseBase(smspecB);
    const QString pa = baseA + QStringLiteral(".UNRST");
    const QString pb = baseB + QStringLiteral(".UNRST");
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

    // --- pair the steps by DATE, not by position and not by SEQNUM ---------
    QMap<QDateTime, int> byDateA, byDateB;
    for (int s : A->listOfReportStepNumbers()) {
        const QDateTime d = dateOfStep(*A, s);
        if (d.isValid()) byDateA.insert(d, s);
    }
    for (int s : B->listOfReportStepNumbers()) {
        const QDateTime d = dateOfStep(*B, s);
        if (d.isValid()) byDateB.insert(d, s);
    }
    if (byDateA.isEmpty() || byDateB.isEmpty()) {
        r.problem = QStringLiteral("no dated report step (INTEHEAD carries no date)");
        return r;
    }
    r.endA = byDateA.lastKey();
    r.endB = byDateB.lastKey();
    for (auto it = byDateA.constBegin(); it != byDateA.constEnd(); ++it) {
        if (byDateB.contains(it.key())) r.times << it.key();
        else                            r.timesOnlyInA << it.key();
    }
    for (auto it = byDateB.constBegin(); it != byDateB.constEnd(); ++it)
        if (!byDateA.contains(it.key())) r.timesOnlyInB << it.key();
    if (r.times.isEmpty()) {
        r.problem = QStringLiteral(
            "no report date in common - A covers %1 to %2, B covers %3 to %4. "
            "A cell field can only be compared where both runs reported; "
            "interpolating one would be inventing the field.")
            .arg(dstr(byDateA.firstKey()), dstr(byDateA.lastKey()),
                 dstr(byDateB.firstKey()), dstr(byDateB.lastKey()));
        return r;
    }

    // --- grid compatibility -------------------------------------------------
    const int probeA = byDateA.value(r.times.front());
    const int probeB = byDateB.value(r.times.front());
    std::int64_t nActA = -1, nActB = -1;
    auto dimsOf = [&](ERst& f, int probe, std::int64_t& nAct) {
        QString dims;
        try {
            const auto& ih = f.getRestartData<int>("INTEHEAD", probe);
            if (int(ih.size()) > IH_NACTIV) {
                dims = QStringLiteral("%1x%2x%3")
                           .arg(ih[IH_NX]).arg(ih[IH_NY]).arg(ih[IH_NZ]);
                nAct = ih[IH_NACTIV];
            }
        } catch (...) {}
        if (nAct <= 0)
            for (const auto& [name, type, len] : f.listOfRstArrays(probe))
                if (type == REAL && len > 1) { nAct = len; break; }
        return dims;
    };
    const QString dimA = dimsOf(*A, probeA, nActA);
    const QString dimB = dimsOf(*B, probeB, nActB);
    if (nActA > 0 && nActB > 0 && nActA != nActB) {
        r.problem = QStringLiteral(
            "the two runs are on different grids - %1 active cells (%2) against "
            "%3 (%4). Cell values cannot be compared.")
            .arg(nActA).arg(dimA.isEmpty() ? QStringLiteral("?") : dimA)
            .arg(nActB).arg(dimB.isEmpty() ? QStringLiteral("?") : dimB);
        return r;
    }

    // --- weights ------------------------------------------------------------
    const std::vector<double> porv = loadPorv(baseA, nActA);
    double porvSum = 0.0;
    for (double v : porv) porvSum += v;
    r.porvWeighted = !porv.empty() && porvSum > 0.0;
    r.gridNote = QStringLiteral("grid %1, %2 active cells, field averages %3")
                     .arg(dimA.isEmpty() ? QStringLiteral("?") : dimA).arg(nActA)
                     .arg(r.porvWeighted ? QStringLiteral("pore-volume weighted")
                                         : QStringLiteral("unweighted - no PORV found"));

    // --- property alignment -------------------------------------------------
    QStringList kwA, kwB;
    auto collect = [&](ERst& f, int probe, std::int64_t nAct, QStringList& into) {
        for (const auto& [name, type, len] : f.listOfRstArrays(probe)) {
            const QString n = QString::fromStdString(name).trimmed();
            if (isCellField(name, type, len, nAct)) into << n;
            else if ((type == REAL || type == DOUB) && len > 1
                     && !r.kwSkipped.contains(n))
                r.kwSkipped << n;
        }
    };
    collect(*A, probeA, nActA, kwA);
    collect(*B, probeB, nActB, kwB);
    kwA.removeDuplicates(); kwB.removeDuplicates();
    QStringList common;
    for (const QString& k : kwA) { if (kwB.contains(k)) common << k; else r.kwOnlyInA << k; }
    for (const QString& k : kwB) if (!kwA.contains(k)) r.kwOnlyInB << k;
    if (common.isEmpty()) {
        r.problem = QStringLiteral("no comparable cell property in common");
        return r;
    }

    // --- the pass -----------------------------------------------------------
    const int totalUnits = common.size() * r.times.size();
    int done = 0;
    for (const QString& kw : common) {
        KeywordDiff kd;
        kd.keyword = kw;
        const std::string k = kw.toStdString();
        for (const QDateTime& when : r.times) {
            if (stop()) { r.cancelled = true; return r; }
            StepDiff sd;
            sd.when = when;
            sd.seqA = byDateA.value(when, -1);
            sd.seqB = byDateB.value(when, -1);
            try {
                if (A->hasArray(k, sd.seqA) && B->hasArray(k, sd.seqB)) {
                    const std::vector<float>& va = A->getRestartData<float>(k, sd.seqA);
                    const std::vector<float>& vb = B->getRestartData<float>(k, sd.seqB);
                    const std::size_t n = std::min(va.size(), vb.size());
                    const bool weight = r.porvWeighted && porv.size() >= n;
                    double sq = 0.0, sumA = 0.0, sumB = 0.0, wsum = 0.0;
                    for (std::size_t i = 0; i < n; ++i) {
                        const double a = va[i], b = vb[i];
                        const double d = std::abs(a - b);
                        sq += d * d;
                        if (d > sd.maxAbs) {
                            sd.maxAbs = d; sd.worstCell = int(i);
                            sd.aWorst = a; sd.bWorst = b;
                        }
                        if (diffIsSignificant(a, b, tol)) ++sd.nBad;
                        const double w = weight ? porv[i] : 1.0;
                        sumA += a * w; sumB += b * w; wsum += w;
                    }
                    if (n)          sd.rms = std::sqrt(sq / double(n));
                    if (wsum > 0.0) { sd.aggA = sumA / wsum; sd.aggB = sumB / wsum; }
                }
            } catch (...) {
                // An unreadable array is not a difference; leave it at zero
                // rather than inventing one.
            }
            if (sd.nBad > 0 && !kd.firstBad.isValid()) kd.firstBad = when;
            kd.totalBad += sd.nBad;
            kd.maxAbsOverall = std::max(kd.maxAbsOverall, sd.maxAbs);
            kd.steps.push_back(sd);
            tick(2 + int(96.0 * double(++done) / double(std::max(1, totalUnits))));
        }
        A->clearData(); B->clearData();     // both readers cache what they load
        r.keywords.push_back(kd);
    }
    tick(100);
    r.ran = true;
    return r;
}

// ===========================================================================
// DivergenceHeatmap
// ===========================================================================

DivergenceHeatmap::DivergenceHeatmap(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
    setMinimumHeight(120);
}

void DivergenceHeatmap::setResult(const CompareResult* r, int metric, bool onlyDiffering)
{
    r_ = r; metric_ = metric; onlyDiffering_ = onlyDiffering;
    rows_.clear();
    vmax_ = 0.0;
    if (r_ && r_->ran) {
        for (int i = 0; i < r_->keywords.size(); ++i) {
            const auto& k = r_->keywords[i];
            if (onlyDiffering_ && k.clean()) continue;
            rows_ << i;
            for (const auto& s : k.steps)
                vmax_ = std::max(vmax_, metric_ == 0 ? double(s.nBad)
                                      : metric_ == 1 ? s.maxAbs : s.rms);
        }
    }
    updateGeometry();
    update();
}

double DivergenceHeatmap::valueAt(int row, int col) const
{
    if (!r_ || row < 0 || row >= rows_.size()) return 0.0;
    const auto& k = r_->keywords[rows_[row]];
    if (col < 0 || col >= k.steps.size()) return 0.0;
    const auto& s = k.steps[col];
    return metric_ == 0 ? double(s.nBad) : metric_ == 1 ? s.maxAbs : s.rms;
}

int DivergenceHeatmap::rowAt(int y) const
{
    if (rows_.isEmpty()) return -1;
    const double h = double(height() - 18) / rows_.size();
    const int i = int((y - 18) / std::max(1.0, h));
    return (i >= 0 && i < rows_.size()) ? i : -1;
}

int DivergenceHeatmap::colAt(int x) const
{
    if (!r_ || r_->times.isEmpty()) return -1;
    const double w = double(width() - labelW_) / r_->times.size();
    const int i = int((x - labelW_) / std::max(1.0, w));
    return (i >= 0 && i < r_->times.size()) ? i : -1;
}

QSize DivergenceHeatmap::minimumSizeHint() const
{
    return { 320, 18 + 14 * std::max(1, int(rows_.size())) };
}

void DivergenceHeatmap::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::white);
    if (!r_ || !r_->ran || rows_.isEmpty() || r_->times.isEmpty()) {
        p.setPen(QColor(0x88, 0x8e, 0x94));
        p.drawText(rect(), Qt::AlignCenter,
                   r_ && r_->ran ? QStringLiteral("every property agrees within tolerance")
                                 : QStringLiteral("no comparison yet"));
        return;
    }
    const int nT = r_->times.size();
    const double cw = double(width() - labelW_) / nT;
    const double ch = double(height() - 18) / rows_.size();

    QFont f = p.font(); f.setPointSizeF(8.0); p.setFont(f);

    // Time ruler: first, middle and last date, which is as much as fits and
    // as much as is needed to place a column.
    p.setPen(QColor(0x33, 0x38, 0x3d));
    for (int c : { 0, nT / 2, nT - 1 }) {
        if (c < 0 || c >= nT) continue;
        const int x = int(labelW_ + c * cw);
        p.drawText(QRect(x - 34, 0, 80, 16),
                   c == 0 ? Qt::AlignLeft : (c == nT - 1 ? Qt::AlignRight : Qt::AlignHCenter),
                   r_->times[c].toString(QStringLiteral("yyyy-MM")));
    }

    for (int row = 0; row < rows_.size(); ++row) {
        const auto& k = r_->keywords[rows_[row]];
        const int y = int(18 + row * ch);
        p.setPen(QColor(0x22, 0x26, 0x2b));
        p.drawText(QRect(2, y, labelW_ - 6, int(ch)),
                   Qt::AlignVCenter | Qt::AlignRight, k.keyword);
        for (int col = 0; col < nT && col < k.steps.size(); ++col) {
            const double v = valueAt(row, col);
            QColor c;
            if (v <= 0.0) {
                c = QColor(0xf2, 0xf5, 0xf8);        // agrees: near-white
            } else {
                // Log-scaled: divergence spans orders of magnitude, and on a
                // linear ramp everything below the worst cell reads as zero.
                const double t = vmax_ > 0.0
                    ? std::clamp(std::log10(v + 1e-30) - std::log10(vmax_ * 1e-4), 0.0, 4.0) / 4.0
                    : 0.0;
                c = QColor::fromHsvF(0.13 * (1.0 - t), 0.15 + 0.80 * t, 1.0 - 0.25 * t);
            }
            p.fillRect(QRectF(labelW_ + col * cw, y, std::max(1.0, cw), std::max(1.0, ch)), c);
        }
    }
    p.setPen(QColor(0xdc, 0xe0, 0xe4));
    p.drawRect(QRect(labelW_, 18, width() - labelW_ - 1, height() - 19));
}

void DivergenceHeatmap::mousePressEvent(QMouseEvent* ev)
{
    const int row = rowAt(int(ev->position().y()));
    const int col = colAt(int(ev->position().x()));
    if (row < 0 || col < 0 || !r_) return;
    emit cellPicked(r_->keywords[rows_[row]].keyword, r_->times[col]);
}

bool DivergenceHeatmap::event(QEvent* ev)
{
    if (ev->type() == QEvent::ToolTip && r_ && r_->ran) {
        auto* he = static_cast<QHelpEvent*>(ev);
        const int row = rowAt(he->pos().y());
        const int col = colAt(he->pos().x());
        if (row >= 0 && col >= 0) {
            const auto& k = r_->keywords[rows_[row]];
            const double v = valueAt(row, col);
            QToolTip::showText(he->globalPos(),
                QStringLiteral("%1\n%2\n%3: %4")
                    .arg(k.keyword,
                         r_->times[col].toString(QStringLiteral("yyyy-MM-dd")),
                         metric_ == 0 ? QStringLiteral("cells outside tolerance")
                                      : metric_ == 1 ? QStringLiteral("max |A-B|")
                                                     : QStringLiteral("RMS"))
                    .arg(v, 0, 'g', 6), this);
            return true;
        }
        QToolTip::hideText();
    }
    return QWidget::event(ev);
}

// ===========================================================================
// RestartComparePanel
// ===========================================================================

RestartComparePanel::RestartComparePanel(QWidget* parent)
    : QWidget(parent)
{
    caseA_ = new QComboBox; caseB_ = new QComboBox;
    caseA_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    caseB_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);

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

    // Its own way in. Mirroring the Summary tab is how the list is kept in
    // step, but a tab you cannot put anything into reads as broken when it is
    // the tab you happen to open first.
    addBtn_ = new QPushButton(QStringLiteral("Add case..."));
    addBtn_->setToolTip(QStringLiteral(
        "open one or more runs' SMSPEC (or EGRID) and add them to the case "
        "list shared with the Summary Plots and 3D View tabs"));
    runBtn_ = new QPushButton(QStringLiteral("Compare"));
    bar_ = new QProgressBar; bar_->setRange(0, 100); bar_->setValue(0);
    bar_->setVisible(false);

    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("A:")));   top->addWidget(caseA_, 1);
    top->addWidget(new QLabel(QStringLiteral("B:")));   top->addWidget(caseB_, 1);
    top->addWidget(new QLabel(QStringLiteral("abs:"))); top->addWidget(absTol_);
    top->addWidget(new QLabel(QStringLiteral("rel:"))); top->addWidget(relTol_);
    top->addWidget(addBtn_);
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
    metric_->addItem(QStringLiteral("field average of A and B"));
    metric_->setToolTip(QStringLiteral(
        "what to show per report date.\n\n"
        "The cell count is usually the most telling of the three divergence "
        "measures: a single pathological cell dominates max|A-B|, while a "
        "count going 0, 0, 3, 47, 1200 shows both when the runs parted and how "
        "fast they are separating.\n\n"
        "The field average is not a difference at all - it is the quantity "
        "itself, averaged over the field for each run and drawn as two curves, "
        "which is what tells you whether a divergence matters."));
    onlyBad_ = new QCheckBox(QStringLiteral("only properties that differ"));
    onlyBad_->setChecked(true);
    onlyBad_->setToolTip(QStringLiteral(
        "hide the properties that agree everywhere - usually most of them"));

    auto* mrow = new QHBoxLayout;
    mrow->addWidget(new QLabel(QStringLiteral("Show:")));
    mrow->addWidget(metric_);
    mrow->addWidget(onlyBad_);
    mrow->addStretch(1);

    // Overview first. With a dozen properties a line apiece is a tangle, and
    // the question to answer first is which property and when, not by how much.
    heat_ = new DivergenceHeatmap;
    chart_ = new QChart;
    chart_->setBackgroundRoundness(0);
    chart_->setDropShadowEnabled(false);
    chart_->legend()->setAlignment(Qt::AlignBottom);
    chartView_ = new QChartView(chart_);
    chartView_->setRenderHint(QPainter::Antialiasing);

    views_ = new QTabWidget;
    views_->addTab(heat_, QStringLiteral("Overview"));
    views_->addTab(chartView_, QStringLiteral("Curves"));

    table_ = new QTableWidget(0, 4);
    table_->setHorizontalHeaderLabels({ QStringLiteral("Property"),
                                        QStringLiteral("First differs"),
                                        QStringLiteral("Cells outside tol"),
                                        QStringLiteral("max |A-B|") });
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);

    detailMode_ = new QComboBox;
    detailMode_->addItem(QStringLiteral("dates of one property"));
    detailMode_->addItem(QStringLiteral("all properties at one date"));
    detailPick_ = new QComboBox;
    detailPick_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    detailPick_->setMinimumWidth(150);
    detailInfo_ = new QLabel;
    detailInfo_->setStyleSheet(QStringLiteral("color:#555b61;"));
    auto* drow = new QHBoxLayout;
    drow->addWidget(new QLabel(QStringLiteral("Detail:")));
    drow->addWidget(detailMode_);
    drow->addWidget(detailPick_);
    drow->addWidget(detailInfo_, 1);

    detail_ = new QTableWidget(0, 8);
    detail_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    detail_->setSelectionBehavior(QAbstractItemView::SelectRows);
    detail_->horizontalHeader()->setStretchLastSection(true);

    auto* detailBox = new QWidget;
    auto* dlay = new QVBoxLayout(detailBox);
    dlay->setContentsMargins(0, 0, 0, 0);
    dlay->addLayout(drow);
    dlay->addWidget(detail_, 1);

    auto* lower = new QSplitter(Qt::Horizontal);
    lower->addWidget(table_);
    lower->addWidget(detailBox);
    lower->setStretchFactor(0, 2);
    lower->setStretchFactor(1, 3);

    auto* split = new QSplitter(Qt::Vertical);
    split->addWidget(views_);
    split->addWidget(lower);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    // Explicit sizes: stretch factors alone let the tables' size hints squeeze
    // the views to a sliver once they have a few hundred rows in them.
    split->setSizes({ 460, 320 });
    lower->setSizes({ 380, 620 });

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(top);
    lay->addWidget(bar_);
    lay->addWidget(verdict_);
    lay->addWidget(note_);
    lay->addLayout(mrow);
    lay->addWidget(split, 1);

    connect(addBtn_, &QPushButton::clicked, this, [this] {
        const QString start = cases_.isEmpty()
            ? QString() : QFileInfo(cases_.last().smspec).absolutePath();
        const QStringList files = QFileDialog::getOpenFileNames(
            this, QStringLiteral("Add cases"), start,
            QStringLiteral("Eclipse case (*.SMSPEC *.EGRID);;All files (*)"));
        for (const QString& f : files) {
            QString base = f;
            if (base.endsWith(QStringLiteral(".EGRID"), Qt::CaseInsensitive)) base.chop(6);
            else if (base.endsWith(QStringLiteral(".SMSPEC"), Qt::CaseInsensitive)) base.chop(7);
            emit openCaseRequested(base + QStringLiteral(".SMSPEC"));
        }
    });
    connect(runBtn_, &QPushButton::clicked, this, [this] { startCompare(); });
    connect(metric_, &QComboBox::currentIndexChanged, this, [this](int) { replot(); });
    connect(onlyBad_, &QCheckBox::toggled, this, [this](bool) { replot(); });
    // A cell of the overview is a property at a date, which is exactly what
    // the detail view wants to be told.
    connect(heat_, &DivergenceHeatmap::cellPicked, this,
            [this](const QString& kw, const QDateTime& when) { pickFromHeatmap(kw, when); });
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = table_->currentRow();
        if (row < 0 || row >= result_.keywords.size()) return;
        if (detailMode_->currentIndex() != 0) return;
        const QString kw = result_.keywords[row].keyword;
        const int at = detailPick_->findText(kw);
        if (at >= 0 && at != detailPick_->currentIndex()) detailPick_->setCurrentIndex(at);
        else refreshDetail();
        if (metric_->currentIndex() == 3) replot();   // the average is per property
    });
    connect(detailMode_, &QComboBox::currentIndexChanged, this, [this](int) { syncCombos(); });
    connect(detailPick_, &QComboBox::currentIndexChanged, this, [this](int) {
        refreshDetail();
        if (metric_->currentIndex() == 3) replot();
    });

    poll_ = new QTimer(this);
    poll_->setInterval(120);
    connect(poll_, &QTimer::timeout, this, [this] { bar_->setValue(progress_.load()); });
}

RestartComparePanel::~RestartComparePanel()
{
    // A worker still reading two restarts has to be told to stop and waited
    // for: it writes into members of this panel.
    cancel_.store(true);
    if (worker_) { worker_->quit(); worker_->wait(5000); }
}

// The overview was clicked: show that property at that date.
void RestartComparePanel::pickFromHeatmap(const QString& keyword, const QDateTime& when)
{
    for (int i = 0; i < result_.keywords.size(); ++i)
        if (result_.keywords[i].keyword == keyword) { table_->selectRow(i); break; }
    detailMode_->setCurrentIndex(1);          // all properties at that date
    const int at = detailPick_->findText(when.toString(QStringLiteral("yyyy-MM-dd")));
    if (at >= 0) detailPick_->setCurrentIndex(at);
    refreshDetail();
}

// -- the case list, mirrored from the Summary tab ---------------------------

void RestartComparePanel::addCase(const QString& label, const QString& smspecPath)
{
    const QString p = normalizeCasePath(smspecPath);
    for (const auto& c : cases_) if (sameCasePath(c.smspec, p)) return;
    cases_.push_back({ label, p });
    const int a = caseA_->currentIndex();
    // The path on the tooltip: two runs of one deck can only differ by folder,
    // and the label is a tag at best.
    caseA_->addItem(label); caseA_->setItemData(caseA_->count() - 1, p, Qt::ToolTipRole);
    caseB_->addItem(label); caseB_->setItemData(caseB_->count() - 1, p, Qt::ToolTipRole);
    // A first pair is worth offering; after that, leave the user's choice be.
    // Qt puts currentIndex at 0 the moment the first item lands, so testing
    // "unset" never fires for B and both boxes would sit on the same case -
    // whereupon Compare has nothing to do. The second case is the moment to
    // offer a pair, and the only one.
    if (a < 0) caseA_->setCurrentIndex(0);
    if (cases_.size() == 2) caseB_->setCurrentIndex(1);
}

void RestartComparePanel::renameCase(const QString& smspecPath, const QString& label)
{
    for (int i = 0; i < cases_.size(); ++i)
        if (sameCasePath(cases_[i].smspec, smspecPath)) {
            cases_[i].label = label;
            caseA_->setItemText(i, label);
            caseB_->setItemText(i, label);
            return;
        }
}

void RestartComparePanel::removeCase(const QString& smspecPath)
{
    for (int i = 0; i < cases_.size(); ++i)
        if (sameCasePath(cases_[i].smspec, smspecPath)) {
            cases_.removeAt(i);
            caseA_->removeItem(i);
            caseB_->removeItem(i);
            return;
        }
}

void RestartComparePanel::reorderCases(const QStringList& smspecPaths)
{
    QVector<CaseEntry> out;
    for (const QString& p : smspecPaths)
        for (const auto& c : cases_)
            if (sameCasePath(c.smspec, p)) { out.push_back(c); break; }
    for (const auto& c : cases_) {          // anything the list did not mention
        bool seen = false;
        for (const auto& o : out) if (sameCasePath(o.smspec, c.smspec)) { seen = true; break; }
        if (!seen) out.push_back(c);
    }
    if (out.size() != cases_.size()) return;
    const QString a = caseA_->currentIndex() >= 0 ? cases_[caseA_->currentIndex()].smspec : QString();
    const QString b = caseB_->currentIndex() >= 0 ? cases_[caseB_->currentIndex()].smspec : QString();
    cases_ = out;
    caseA_->clear(); caseB_->clear();
    for (const auto& c : cases_) {
        caseA_->addItem(c.label); caseA_->setItemData(caseA_->count() - 1, c.smspec, Qt::ToolTipRole);
        caseB_->addItem(c.label); caseB_->setItemData(caseB_->count() - 1, c.smspec, Qt::ToolTipRole);
    }
    for (int i = 0; i < cases_.size(); ++i) {
        if (!a.isEmpty() && sameCasePath(cases_[i].smspec, a)) caseA_->setCurrentIndex(i);
        if (!b.isEmpty() && sameCasePath(cases_[i].smspec, b)) caseB_->setCurrentIndex(i);
    }
}

// -- the drill-down ----------------------------------------------------------

void RestartComparePanel::syncCombos()
{
    const QSignalBlocker block(detailPick_);
    detailPick_->clear();
    if (!result_.ran) { refreshDetail(); return; }
    if (detailMode_->currentIndex() == 0) {
        for (const auto& k : result_.keywords) detailPick_->addItem(k.keyword);
        const int row = table_->currentRow();
        if (row >= 0 && row < result_.keywords.size()) detailPick_->setCurrentIndex(row);
    } else {
        for (const QDateTime& t : result_.times)
            detailPick_->addItem(t.toString(QStringLiteral("yyyy-MM-dd")));
        // Open on the date the verdict named, which is the one being asked
        // about far more often than the first.
        QDateTime first;
        for (const auto& k : result_.keywords)
            if (k.firstBad.isValid() && (!first.isValid() || k.firstBad < first))
                first = k.firstBad;
        const int at = first.isValid()
            ? detailPick_->findText(first.toString(QStringLiteral("yyyy-MM-dd"))) : -1;
        if (at >= 0) detailPick_->setCurrentIndex(at);
    }
    refreshDetail();
}

void RestartComparePanel::refreshDetail()
{
    detail_->setRowCount(0);
    detailInfo_->clear();
    if (!result_.ran || detailPick_->currentIndex() < 0) return;
    if (detailMode_->currentIndex() == 0) showKeywordDetail(detailPick_->currentText());
    else                                  showStepDetail(
        QDateTime::fromString(detailPick_->currentText(), QStringLiteral("yyyy-MM-dd")));
}

// Every report step of one property: where it holds and where it gives way.
void RestartComparePanel::showKeywordDetail(const QString& keyword)
{
    const KeywordDiff* kd = nullptr;
    for (const auto& k : result_.keywords) if (k.keyword == keyword) { kd = &k; break; }
    if (!kd) return;
    detail_->setColumnCount(8);
    detail_->setHorizontalHeaderLabels({ QStringLiteral("Date"),
                                         QStringLiteral("Step A/B"),
                                         QStringLiteral("Cells outside tol"),
                                         QStringLiteral("max |A-B|"),
                                         QStringLiteral("RMS"),
                                         QStringLiteral("Worst cell"),
                                         QStringLiteral("A / B there"),
                                         QStringLiteral("field avg A / B") });
    for (const auto& sd : kd->steps) {
        const int row = detail_->rowCount();
        detail_->insertRow(row);
        detail_->setItem(row, 0, new QTableWidgetItem(
            sd.when.toString(QStringLiteral("yyyy-MM-dd"))));
        detail_->setItem(row, 1, new QTableWidgetItem(
            QStringLiteral("%1 / %2").arg(sd.seqA).arg(sd.seqB)));
        detail_->setItem(row, 2, new QTableWidgetItem(QString::number(sd.nBad)));
        detail_->setItem(row, 3, new QTableWidgetItem(QStringLiteral("%1").arg(sd.maxAbs, 0, 'g', 6)));
        detail_->setItem(row, 4, new QTableWidgetItem(QStringLiteral("%1").arg(sd.rms, 0, 'g', 6)));
        detail_->setItem(row, 5, new QTableWidgetItem(
            sd.worstCell < 0 ? QStringLiteral("-") : QString::number(sd.worstCell)));
        detail_->setItem(row, 6, new QTableWidgetItem(
            sd.worstCell < 0 ? QStringLiteral("-")
                             : QStringLiteral("%1  /  %2").arg(sd.aWorst, 0, 'g', 8)
                                                          .arg(sd.bWorst, 0, 'g', 8)));
        detail_->setItem(row, 7, new QTableWidgetItem(
            QStringLiteral("%1  /  %2").arg(sd.aggA, 0, 'g', 6).arg(sd.aggB, 0, 'g', 6)));
        if (sd.nBad > 0)
            for (int c = 0; c < 8; ++c)
                detail_->item(row, c)->setForeground(QBrush(QColor(0xa8, 0x50, 0x0d)));
    }
    detail_->resizeColumnsToContents();
    detailInfo_->setText(kd->clean()
        ? QStringLiteral("%1 agrees at every date").arg(keyword)
        : QStringLiteral("%1 first differs %2").arg(keyword,
              kd->firstBad.toString(QStringLiteral("yyyy-MM-dd"))));
}

// Every property at one report step: what else went wrong where this did.
void RestartComparePanel::showStepDetail(const QDateTime& when)
{
    detail_->setColumnCount(7);
    detail_->setHorizontalHeaderLabels({ QStringLiteral("Property"),
                                         QStringLiteral("Cells outside tol"),
                                         QStringLiteral("max |A-B|"),
                                         QStringLiteral("RMS"),
                                         QStringLiteral("Worst cell"),
                                         QStringLiteral("A / B there"),
                                         QStringLiteral("field avg A / B") });
    int differing = 0;
    for (const auto& k : result_.keywords) {
        const StepDiff* sd = nullptr;
        for (const auto& s : k.steps)
            if (s.when.date() == when.date()) { sd = &s; break; }
        if (!sd) continue;
        const int row = detail_->rowCount();
        detail_->insertRow(row);
        detail_->setItem(row, 0, new QTableWidgetItem(k.keyword));
        detail_->setItem(row, 1, new QTableWidgetItem(QString::number(sd->nBad)));
        detail_->setItem(row, 2, new QTableWidgetItem(QStringLiteral("%1").arg(sd->maxAbs, 0, 'g', 6)));
        detail_->setItem(row, 3, new QTableWidgetItem(QStringLiteral("%1").arg(sd->rms, 0, 'g', 6)));
        detail_->setItem(row, 4, new QTableWidgetItem(
            sd->worstCell < 0 ? QStringLiteral("-") : QString::number(sd->worstCell)));
        detail_->setItem(row, 5, new QTableWidgetItem(
            sd->worstCell < 0 ? QStringLiteral("-")
                              : QStringLiteral("%1  /  %2").arg(sd->aWorst, 0, 'g', 8)
                                                           .arg(sd->bWorst, 0, 'g', 8)));
        detail_->setItem(row, 6, new QTableWidgetItem(
            QStringLiteral("%1  /  %2").arg(sd->aggA, 0, 'g', 6).arg(sd->aggB, 0, 'g', 6)));
        if (sd->nBad > 0) {
            ++differing;
            for (int c = 0; c < 7; ++c)
                detail_->item(row, c)->setForeground(QBrush(QColor(0xa8, 0x50, 0x0d)));
        }
    }
    detail_->resizeColumnsToContents();
    detailInfo_->setText(differing
        ? QStringLiteral("%1: %2 propert(y/ies) outside tolerance")
              .arg(when.toString(QStringLiteral("yyyy-MM-dd"))).arg(differing)
        : QStringLiteral("%1: every property within tolerance")
              .arg(when.toString(QStringLiteral("yyyy-MM-dd"))));
}

void RestartComparePanel::startCompare()
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

void RestartComparePanel::finishCompare()
{
    poll_->stop();
    bar_->setVisible(false);
    runBtn_->setEnabled(true);
    if (worker_) { worker_->deleteLater(); worker_ = nullptr; }
    showResult();
}

void RestartComparePanel::showResult()
{
    const bool same = result_.identical();
    verdict_->setText(result_.verdict());
    verdict_->setStyleSheet(!result_.ran ? QStringLiteral("color:#8a1f1f;")
                            : same       ? QStringLiteral("color:#05814f;")
                                         : QStringLiteral("color:#a8500d;"));

    QStringList notes;
    if (!result_.gridNote.isEmpty()) notes << result_.gridNote;
    if (!result_.times.isEmpty())
        notes << QStringLiteral("%1 common report date(s), %2 to %3")
                     .arg(result_.times.size())
                     .arg(result_.times.first().toString(QStringLiteral("yyyy-MM-dd")),
                          result_.times.last().toString(QStringLiteral("yyyy-MM-dd")));
    auto listNote = [&notes](const QString& what, const QStringList& v) {
        if (v.isEmpty()) return;
        notes << QStringLiteral("%1: %2").arg(what,
                  v.mid(0, 6).join(QStringLiteral(", "))
                  + (v.size() > 6 ? QStringLiteral(" +%1 more").arg(v.size() - 6)
                                  : QString()));
    };
    listNote(QStringLiteral("only in A"), result_.kwOnlyInA);
    listNote(QStringLiteral("only in B"), result_.kwOnlyInB);
    if (!result_.timesOnlyInA.isEmpty())
        notes << QStringLiteral("%1 date(s) only in A").arg(result_.timesOnlyInA.size());
    if (!result_.timesOnlyInB.isEmpty())
        notes << QStringLiteral("%1 date(s) only in B").arg(result_.timesOnlyInB.size());
    if (!result_.kwSkipped.isEmpty())
        notes << QStringLiteral("not per-cell, not compared: %1")
                     .arg(result_.kwSkipped.mid(0, 8).join(QStringLiteral(", "))
                          + (result_.kwSkipped.size() > 8
                                 ? QStringLiteral(" +%1 more").arg(result_.kwSkipped.size() - 8)
                                 : QString()));
    note_->setText(notes.join(QStringLiteral("   |   ")));

    table_->setRowCount(0);
    for (const auto& k : result_.keywords) {
        const int row = table_->rowCount();
        table_->insertRow(row);
        table_->setItem(row, 0, new QTableWidgetItem(k.keyword));
        table_->setItem(row, 1, new QTableWidgetItem(
            k.firstBad.isValid() ? k.firstBad.toString(QStringLiteral("yyyy-MM-dd"))
                                 : QStringLiteral("-")));
        table_->setItem(row, 2, new QTableWidgetItem(QString::number(k.totalBad)));
        table_->setItem(row, 3, new QTableWidgetItem(
            QStringLiteral("%1").arg(k.maxAbsOverall, 0, 'g', 4)));
        if (!k.clean())
            for (int c = 0; c < 4; ++c)
                table_->item(row, c)->setForeground(QBrush(QColor(0xa8, 0x50, 0x0d)));
    }
    syncCombos();
    table_->resizeColumnsToContents();
    heat_->setResult(&result_, metric_->currentIndex(), onlyBad_->isChecked());
    replot();
}

void RestartComparePanel::replot()
{
    chart_->removeAllSeries();
    for (auto* ax : chart_->axes()) chart_->removeAxis(ax);
    heat_->setResult(&result_, metric_->currentIndex(), onlyBad_->isChecked());
    if (!result_.ran || result_.keywords.isEmpty()) { chart_->setTitle(QString()); return; }

    const int metric = metric_->currentIndex();
    const bool onlyBad = onlyBad_->isChecked();
    const bool valueMode = (metric == 3);
    double ymin = 0.0, ymax = 0.0;
    bool any = false;
    int shown = 0, hidden = 0, omitted = 0;

    auto ms = [](const QDateTime& t) { return double(t.toMSecsSinceEpoch()); };

    if (valueMode) {
        // The quantity itself, for ONE property, as two curves. Every property
        // at once would be meaningless: pressure in bar and saturation in
        // fractions do not share an axis.
        const QString kw = detailMode_->currentIndex() == 0
                               ? detailPick_->currentText()
                               : (table_->currentRow() >= 0
                                      && table_->currentRow() < result_.keywords.size()
                                          ? result_.keywords[table_->currentRow()].keyword
                                          : QString());
        const KeywordDiff* kd = nullptr;
        for (const auto& k : result_.keywords) if (k.keyword == kw) { kd = &k; break; }
        if (!kd) {
            chart_->setTitle(QStringLiteral(
                "pick a property (Detail, left) to see its field average"));
            return;
        }
        auto* sa = new QLineSeries; sa->setName(QStringLiteral("A - %1").arg(kw));
        auto* sb = new QLineSeries; sb->setName(QStringLiteral("B - %1").arg(kw));
        bool first = true;
        for (const auto& sd : kd->steps) {
            sa->append(ms(sd.when), sd.aggA);
            sb->append(ms(sd.when), sd.aggB);
            if (first) { ymin = ymax = sd.aggA; first = false; }
            ymin = std::min({ ymin, sd.aggA, sd.aggB });
            ymax = std::max({ ymax, sd.aggA, sd.aggB });
            any = true;
        }
        chart_->addSeries(sa); chart_->addSeries(sb);
        shown = 2;
        chart_->setTitle(QStringLiteral("%1 - field average, %2")
            .arg(kw, result_.porvWeighted ? QStringLiteral("pore-volume weighted")
                                          : QStringLiteral("unweighted")));
    } else {
        // Zero is both common and meaningful - it is what agreement looks like
        // - and a log axis has nothing to say about it. Rather than plot a fake
        // floor, a date with nothing to report is left out, so a curve begins
        // exactly where its property first differs. Said in the title, so an
        // absence is not mistaken for missing data.
        for (const auto& k : result_.keywords) {
            if (onlyBad && k.clean()) { ++hidden; continue; }
            auto* s = new QLineSeries;
            s->setName(k.keyword);
            int pts = 0;
            for (const auto& sd : k.steps) {
                const double y = metric == 0 ? double(sd.nBad)
                               : metric == 1 ? sd.maxAbs : sd.rms;
                if (y <= 0.0) { ++omitted; continue; }
                s->append(ms(sd.when), y);
                ymax = std::max(ymax, y);
                ++pts; any = true;
            }
            if (pts == 0) { delete s; ++hidden; continue; }
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
    }
    if (!any) return;

    // Time, not step number: the two runs are paired on the date, and a date
    // axis is also what every other plot in this program uses.
    auto* ax = new QDateTimeAxis;
    ax->setFormat(QStringLiteral("yyyy-MM-dd"));
    ax->setTitleText(QStringLiteral("date"));
    ax->setTickCount(4);
    if (!result_.times.isEmpty())
        ax->setRange(result_.times.first(), result_.times.last());
    chart_->addAxis(ax, Qt::AlignBottom);

    QAbstractAxis* ay = nullptr;
    if (valueMode) {
        auto* va = new QValueAxis;
        const double pad = (ymax > ymin) ? 0.05 * (ymax - ymin) : 1.0;
        va->setRange(ymin - pad, ymax + pad);
        ay = va;
    } else if (ymax > 0.0) {
        // A divergence climbs by orders of magnitude, so a linear axis shows
        // the last date and nothing before it.
        auto* la = new QLogValueAxis;
        la->setBase(10.0);
        la->setLabelFormat(QStringLiteral("%g"));
        la->setRange(std::max(ymax * 1e-6, 1e-12), ymax * 1.5);
        ay = la;
    } else {
        auto* va = new QValueAxis; va->setRange(0.0, 1.0); ay = va;
    }
    ay->setTitleText(valueMode ? QStringLiteral("field average")
                               : metric_->currentText());
    chart_->addAxis(ay, Qt::AlignLeft);
    for (auto* s : chart_->series()) { s->attachAxis(ax); s->attachAxis(ay); }
}

} // namespace flowgui
