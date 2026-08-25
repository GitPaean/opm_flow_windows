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
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLegendMarker>
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
#include <QScatterSeries>
#include <QShowEvent>
#include <QSpinBox>
#include <QValueAxis>
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

// Markers are thinned to keep them this many times their own size apart;
// closer than that and the row of them reads as a band, not as points.
constexpr double kMarkerPitch = 2.5;

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

    const bool hasA = QFileInfo::exists(pa), hasB = QFileInfo::exists(pb);
    if (!hasA || !hasB) {
        // Two runs of one case share a file name, so naming the file says
        // nothing about which side is missing it - name the side, and the
        // folder that tells the two apart. Both can be missing at once.
        const auto where = [](const QString& p) {
            const QFileInfo fi(p);
            return QStringLiteral("%1 (%2)")
                .arg(fi.fileName(), QDir(fi.absolutePath()).dirName());
        };
        QStringList miss;
        if (!hasA) miss << QStringLiteral("A: %1").arg(where(pa));
        if (!hasB) miss << QStringLiteral("B: %1").arg(where(pb));
        r.problem = QStringLiteral("no restart file for %1")
                        .arg(miss.join(QStringLiteral(", ")));
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

    metric_->setToolTip(QStringLiteral(
        "what the overview shows per report date.\n\n"
        "The cell count is usually the most telling: a single pathological "
        "cell dominates max|A-B|, while a count going 0, 0, 3, 47, 1200 shows "
        "both when the runs parted and how fast they are separating."));
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

    // The cell-values view: the heatmap says which property and when; this
    // shows how - the field itself, cell by cell, at one date.
    cellProp_ = new QComboBox;
    cellProp_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    cellProp_->setMinimumWidth(110);
    cellDate_ = new QComboBox;
    cellDate_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    cellMode_ = new QComboBox;
    cellMode_->addItem(QStringLiteral("A and B"));
    cellMode_->addItem(QStringLiteral("difference A - B"));
    cellMode_->addItem(QStringLiteral("scatter A vs B"));
    cellMode_->setToolTip(QStringLiteral(
        "A and B: both runs' values against the active-cell index - for a 1D "
        "column, the profile.\n"
        "difference: where along the cells the runs part, and by how much.\n"
        "scatter: each cell as a point, A across, B up; agreement is the "
        "diagonal."));
    // Off to begin with here: this plot has a point per active cell, and a
    // marker on each of a hundred thousand of them is a solid band, not a
    // reading. It earns its keep on a small model or a zoomed-in stretch,
    // which is why it is offered at all.
    cellMarkers_ = new QCheckBox(QStringLiteral("markers"));
    cellMarkers_->setChecked(false);
    cellMarkers_->setToolTip(QStringLiteral(
        "mark each cell's value on the curve.\n"
        "With more cells than can be marked legibly, every n-th is marked and "
        "the caption says which - the curve itself still shows every cell."));
    cellMarkerSize_ = new QDoubleSpinBox;
    cellMarkerSize_->setRange(2.0, 20.0);
    cellMarkerSize_->setSingleStep(0.5);
    cellMarkerSize_->setValue(7.0);
    cellMarkerSize_->setSuffix(QStringLiteral(" px"));
    cellMarkerSize_->setToolTip(QStringLiteral("marker size"));
    cellInfo_ = new QLabel;
    cellInfo_->setStyleSheet(QStringLiteral("color:#555b61;"));
    cellChart_ = new QChart;
    cellChart_->setBackgroundRoundness(0);
    cellChart_->setDropShadowEnabled(false);
    cellChartView_ = new QChartView(cellChart_);
    cellChartView_->setRenderHint(QPainter::Antialiasing);
    cellView_ = new QWidget;
    {
        auto* crow = new QHBoxLayout;
        crow->addWidget(new QLabel(QStringLiteral("Property:")));
        crow->addWidget(cellProp_);
        crow->addWidget(new QLabel(QStringLiteral("at:")));
        crow->addWidget(cellDate_);
        crow->addWidget(cellMode_);
        crow->addWidget(cellMarkers_);
        crow->addWidget(cellMarkerSize_);
        crow->addWidget(cellInfo_, 1);
        auto* clay = new QVBoxLayout(cellView_);
        clay->setContentsMargins(4, 4, 4, 0);
        clay->addLayout(crow);
        clay->addWidget(cellChartView_, 1);
    }

    // One cell through time: the third of the three questions. The heatmap
    // says which property and when; Cell values shows the whole field at one
    // date; this follows a single cell across every common date - the two
    // runs' values as marked curves, the way the summary plots mark theirs.
    histProp_ = new QComboBox;
    histProp_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    histProp_->setMinimumWidth(110);
    histCell_ = new QSpinBox;
    histCell_->setRange(1, 1);
    histCell_->setToolTip(QStringLiteral(
        "which active cell to follow - the same numbering the Cell values "
        "view draws along its X axis"));
    // On here, unlike the cell view: a run reports at a few dozen dates, and
    // exactly where a value was written is most of what this plot is for.
    histMarkers_ = new QCheckBox(QStringLiteral("markers"));
    histMarkers_->setChecked(true);
    histMarkers_->setToolTip(QStringLiteral(
        "mark each report date on the curves - where a value was actually "
        "written, as opposed to the line drawn between"));
    histMarkerSize_ = new QDoubleSpinBox;
    histMarkerSize_->setRange(2.0, 20.0);
    histMarkerSize_->setSingleStep(0.5);
    histMarkerSize_->setValue(8.0);
    histMarkerSize_->setSuffix(QStringLiteral(" px"));
    histMarkerSize_->setToolTip(QStringLiteral("marker size"));
    histInfo_ = new QLabel;
    histInfo_->setStyleSheet(QStringLiteral("color:#555b61;"));
    histChartView_ = new QChartView(new QChart);
    histChartView_->setRenderHint(QPainter::Antialiasing);
    histView_ = new QWidget;
    {
        auto* hrow = new QHBoxLayout;
        hrow->addWidget(new QLabel(QStringLiteral("Property:")));
        hrow->addWidget(histProp_);
        hrow->addWidget(new QLabel(QStringLiteral("cell:")));
        hrow->addWidget(histCell_);
        hrow->addWidget(histMarkers_);
        hrow->addWidget(histMarkerSize_);
        hrow->addWidget(histInfo_, 1);
        auto* hlay = new QVBoxLayout(histView_);
        hlay->setContentsMargins(4, 4, 4, 0);
        hlay->addLayout(hrow);
        hlay->addWidget(histChartView_, 1);
    }

    views_ = new QTabWidget;
    views_->addTab(heat_, QStringLiteral("Overview"));
    views_->addTab(cellView_, QStringLiteral("Cell values"));
    views_->addTab(histView_, QStringLiteral("Cell history"));

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
    connect(caseA_, &QComboBox::currentIndexChanged, this,
            [this](int) { syncCaseTips(); cellCasesChanged(); });
    connect(caseB_, &QComboBox::currentIndexChanged, this,
            [this](int) { syncCaseTips(); cellCasesChanged(); });
    connect(views_, &QTabWidget::currentChanged, this, [this](int idx) {
        if (cellDirty_ && (idx == views_->indexOf(cellView_)
                           || idx == views_->indexOf(histView_)))
            rebuildCellPickers();
    });
    connect(cellProp_, &QComboBox::currentIndexChanged, this,
            [this](int) { if (!cellFilling_) replotCells(); });
    connect(cellDate_, &QComboBox::currentIndexChanged, this,
            [this](int) { if (!cellFilling_) replotCells(); });
    connect(cellMode_, &QComboBox::currentIndexChanged, this,
            [this](int) { if (!cellFilling_) replotCells(); });
    connect(histProp_, &QComboBox::currentIndexChanged, this,
            [this](int) { if (!cellFilling_) replotHistory(); });
    connect(cellMarkers_, &QCheckBox::toggled, this,
            [this](bool) { replotCells(); });
    connect(cellMarkerSize_, &QDoubleSpinBox::valueChanged, this,
            [this](double) { replotCells(); });
    connect(histMarkers_, &QCheckBox::toggled, this,
            [this](bool) { replotHistory(); });
    connect(histMarkerSize_, &QDoubleSpinBox::valueChanged, this,
            [this](double) { replotHistory(); });
    connect(histCell_, &QSpinBox::valueChanged, this,
            [this](int) { if (!cellFilling_) replotHistory(); });
    // The caption counts cells outside tolerance, so it follows the spins.
    connect(absTol_, &QDoubleSpinBox::valueChanged, this,
            [this](double) { if (views_->currentWidget() == cellView_) replotCells(); });
    connect(relTol_, &QDoubleSpinBox::valueChanged, this,
            [this](double) { if (views_->currentWidget() == cellView_) replotCells(); });
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
    });
    connect(detailMode_, &QComboBox::currentIndexChanged, this, [this](int) { syncCombos(); });
    connect(detailPick_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshDetail(); });

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
    // ... and aim the cell-values view at the same property and date, so
    // switching to it continues the same question.
    if (cellProp_ && cellDate_) {
        cellFilling_ = true;
        const int p = cellProp_->findText(keyword);
        if (p >= 0) cellProp_->setCurrentIndex(p);
        int d = -1;
        for (int i = 0; i < cellDate_->count(); ++i)
            if (cellDate_->itemData(i).toDateTime() == when) { d = i; break; }
        if (d >= 0) cellDate_->setCurrentIndex(d);
        // ... and the history at the same property, following the cell the
        // comparison found worst at that date.
        const int hp = histProp_->findText(keyword);
        if (hp >= 0) histProp_->setCurrentIndex(hp);
        for (const auto& k : result_.keywords) {
            if (k.keyword != keyword) continue;
            for (const auto& sd : k.steps)
                if (sd.when == when && sd.worstCell >= 0
                    && sd.worstCell < histCell_->maximum())
                    histCell_->setValue(sd.worstCell + 1);
        }
        cellFilling_ = false;
        if (p >= 0 || cellDate_->count()) replotCells();
        replotHistory();
    }
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
    syncCaseTips();
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
    syncCaseTips();
}

// -- the drill-down ----------------------------------------------------------

// A closed combo shows its own tooltip, never the current item's, so the path
// goes on the widget too - otherwise hovering A or B says nothing until the
// list is dropped down, which is exactly when you no longer need telling.
void RestartComparePanel::syncCaseTips()
{
    auto put = [this](QComboBox* box, const char* what) {
        const int i = box->currentIndex();
        box->setToolTip(i >= 0 && i < cases_.size()
                            ? QDir::toNativeSeparators(cases_[i].smspec)
                            : QString::fromLatin1(what));
    };
    put(caseA_, "the first case of the pair");
    put(caseB_, "the second case of the pair");
}

void RestartComparePanel::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    if (views_ && cellDirty_ && (views_->currentWidget() == cellView_
                                 || views_->currentWidget() == histView_))
        rebuildCellPickers();
}

std::shared_ptr<Opm::EclIO::ERst> RestartComparePanel::rstReader(const QString& smspec)
{
    if (smspec.isEmpty()) return nullptr;
    const QString path = caseBase(smspec) + QStringLiteral(".UNRST");
    const QFileInfo fi(path);
    if (!fi.exists()) return nullptr;
    const QString stamp = QStringLiteral("%1|%2")
                              .arg(fi.lastModified().toMSecsSinceEpoch()).arg(fi.size());
    auto it = rstReaders_.find(path);
    if (it != rstReaders_.end() && it->second.stamp == stamp && it->second.rst)
        return it->second.rst;
    std::shared_ptr<Opm::EclIO::ERst> r;
    try {
        r = std::make_shared<Opm::EclIO::ERst>(path.toStdString());
    } catch (...) { r = nullptr; }
    rstReaders_[path] = RstReader{ r, stamp };
    return r;
}

void RestartComparePanel::cellCasesChanged()
{
    cellDirty_ = true;
    if (isVisible() && views_ && (views_->currentWidget() == cellView_
                                  || views_->currentWidget() == histView_))
        rebuildCellPickers();
}

void RestartComparePanel::rebuildCellPickers()
{
    cellDirty_ = false;
    const QString oldProp = cellProp_->currentText();
    const QDateTime oldDate = cellDate_->currentData().toDateTime();

    cellFilling_ = true;
    cellProp_->clear();
    cellDate_->clear();
    cellDatesA_.clear();
    cellDatesB_.clear();
    cellTimes_.clear();

    const int ia = caseA_->currentIndex(), ib = caseB_->currentIndex();
    auto ra = (ia >= 0 && ia < cases_.size()) ? rstReader(cases_[ia].smspec) : nullptr;
    auto rb = (ib >= 0 && ib < cases_.size()) ? rstReader(cases_[ib].smspec) : nullptr;
    if (!ra || !rb) {
        cellFilling_ = false;
        cellInfo_->setText(QStringLiteral("pick two cases with a restart (UNRST) file"));
        replotCells();
        return;
    }

    // Steps by DATE, exactly as the full comparison pairs them: two runs need
    // not report at the same times, and the n-th step of one against the n-th
    // of the other can be different moments wearing the same number.
    for (int st : ra->listOfReportStepNumbers()) {
        const QDateTime d = dateOfStep(*ra, st);
        if (d.isValid()) cellDatesA_.insert(d, st);
    }
    for (int st : rb->listOfReportStepNumbers()) {
        const QDateTime d = dateOfStep(*rb, st);
        if (d.isValid()) cellDatesB_.insert(d, st);
    }
    for (auto it = cellDatesA_.constBegin(); it != cellDatesA_.constEnd(); ++it)
        if (cellDatesB_.contains(it.key())) cellTimes_ << it.key();
    if (cellTimes_.isEmpty()) {
        cellFilling_ = false;
        cellInfo_->setText(QStringLiteral("no report date in common"));
        replotCells();
        return;
    }
    // A run reporting within a day (this equilibration case writes five steps
    // on 1 Jan) would list one date five times; label with the clock when the
    // calendar alone cannot tell the steps apart. The real QDateTime rides
    // as item data either way, so picking never goes through the label.
    bool intraday = false;
    for (int i = 1; i < cellTimes_.size(); ++i)
        if (cellTimes_[i].date() == cellTimes_[i - 1].date()) { intraday = true; break; }
    for (const QDateTime& d : std::as_const(cellTimes_))
        cellDate_->addItem(intraday
                               ? d.toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                               : dstr(d), d);
    // The latest common date, where the runs have had the longest to part -
    // unless the previous choice is still on offer.
    int keepD = -1;
    if (oldDate.isValid())
        for (int i = 0; i < cellDate_->count(); ++i)
            if (cellDate_->itemData(i).toDateTime() == oldDate) { keepD = i; break; }
    cellDate_->setCurrentIndex(keepD >= 0 ? keepD : cellDate_->count() - 1);

    // The cell fields both files carry at that date, at the same length, in
    // A's own file order - PRESSURE and the saturations first, then the
    // compositional block (ZMF/XMF/YMF per component), the way the file has
    // them.
    const QDateTime at = cellTimes_[cellDate_->currentIndex()];
    const int sa = cellDatesA_.value(at), sb = cellDatesB_.value(at);
    QHash<QString, std::int64_t> lenB;
    try {
        for (const auto& [name, type, len] : rb->listOfRstArrays(sb))
            if (isCellField(name, type, len, -1))
                lenB.insert(QString::fromStdString(name).trimmed(), len);
    } catch (...) {}
    try {
        for (const auto& [name, type, len] : ra->listOfRstArrays(sa)) {
            if (!isCellField(name, type, len, -1)) continue;
            const QString n = QString::fromStdString(name).trimmed();
            if (lenB.value(n, -1) == len) cellProp_->addItem(n);
        }
    } catch (...) {}
    const int keepP = oldProp.isEmpty() ? -1 : cellProp_->findText(oldProp);
    if (keepP >= 0) cellProp_->setCurrentIndex(keepP);
    else {
        const int pr = cellProp_->findText(QStringLiteral("PRESSURE"));
        if (pr >= 0) cellProp_->setCurrentIndex(pr);
    }

    // The history view picks from the same list, and keeps its cell if the
    // grid still has it.
    const QString oldHProp = histProp_->currentText();
    const int oldHCell = histCell_->value();
    histProp_->clear();
    std::int64_t nAct = 0;
    for (int i = 0; i < cellProp_->count(); ++i) {
        histProp_->addItem(cellProp_->itemText(i));
        if (nAct == 0) nAct = lenB.value(cellProp_->itemText(i), 0);
    }
    const int keepH = oldHProp.isEmpty() ? -1 : histProp_->findText(oldHProp);
    if (keepH >= 0) histProp_->setCurrentIndex(keepH);
    else histProp_->setCurrentIndex(cellProp_->currentIndex());
    histCell_->setRange(1, std::max<qint64>(1, nAct));
    histCell_->setValue(std::min<qint64>(oldHCell, std::max<qint64>(1, nAct)));

    cellFilling_ = false;
    replotCells();
    replotHistory();
}

void RestartComparePanel::replotCells()
{
    // A fresh chart per plot. Emptying and refilling one QChart looks right
    // in the object tree - axes() and series() both go to zero - but Qt
    // Charts 6.4 leaves the removed items' tick labels and curves painting in
    // the scene (reproduced in a 30-line probe, in either teardown order), so
    // every replot stacked another set of ghosts. Swapping the whole chart is
    // the clean path: setChart() shows the new one, the old is deleted here.
    auto* chart = new QChart;
    chart->setBackgroundRoundness(0);
    chart->setDropShadowEnabled(false);
    auto present = [this, chart] {
        QChart* old = cellChartView_->chart();
        cellChartView_->setChart(chart);
        cellChart_ = chart;
        if (old != chart) delete old;
    };

    const int ia = caseA_->currentIndex(), ib = caseB_->currentIndex();
    auto ra = (ia >= 0 && ia < cases_.size()) ? rstReader(cases_[ia].smspec) : nullptr;
    auto rb = (ib >= 0 && ib < cases_.size()) ? rstReader(cases_[ib].smspec) : nullptr;
    const int di = cellDate_->currentIndex();
    if (!ra || !rb || cellProp_->currentText().isEmpty()
        || di < 0 || di >= cellTimes_.size())
        { present(); return; }

    const QDateTime at = cellTimes_[di];
    const std::string key = cellProp_->currentText().toStdString();
    std::vector<float> va, vb;
    try {
        va = ra->getRestartData<float>(key, cellDatesA_.value(at));
        vb = rb->getRestartData<float>(key, cellDatesB_.value(at));
    } catch (const std::exception& e) {
        cellInfo_->setText(QString::fromLocal8Bit(e.what()));
        { present(); return; }
    }
    const int n = int(std::min(va.size(), vb.size()));
    if (n == 0) { cellInfo_->setText(QStringLiteral("empty field")); present(); return; }

    // The caption's test is compareECL's, with the tolerances on this panel -
    // the same currency as the heatmap, so the two views cannot disagree
    // about what counts as different.
    const DiffTol tol{ absTol_->value(), relTol_->value() };
    int nBad = 0, worstCell = 0;
    double maxAbs = 0.0;
    for (int i = 0; i < n; ++i) {
        const double d = std::abs(double(va[i]) - double(vb[i]));
        if (d > maxAbs) { maxAbs = d; worstCell = i; }
        if (diffIsSignificant(va[i], vb[i], tol)) ++nBad;
    }
    cellInfo_->setText(maxAbs == 0.0
        ? QStringLiteral("%1 cells, identical").arg(n)
        : QStringLiteral("%1 cells, %2 outside tolerance;  max |A-B| %3 at cell %4 "
                         "(A %5, B %6)")
              .arg(n).arg(nBad).arg(maxAbs, 0, 'g', 4).arg(worstCell + 1)
              .arg(double(va[worstCell]), 0, 'g', 6)
              .arg(double(vb[worstCell]), 0, 'g', 6));

    const QColor colA(0x1f, 0x77, 0xb4), colB(0xd6, 0x27, 0x28);
    const int mode = cellMode_->currentIndex();
    // The scatter mode is nothing but markers, so the option is meaningless
    // there rather than merely ineffective - say so by greying it out.
    const bool canMark = (mode != 2);
    cellMarkers_->setEnabled(canMark);
    cellMarkerSize_->setEnabled(canMark && cellMarkers_->isChecked());

    // A marker reads as a point only while it has clear space around it; once
    // they sit closer than about their own size apart the row closes into a
    // band. So the budget is how many fit at that spacing, not a fixed count:
    // both the plot width and the marker size can change under us. The CURVE
    // is never thinned - what is drawn is still every cell.
    double plotW = cellChart_->plotArea().width();
    if (plotW < 50.0 && cellChartView_) plotW = cellChartView_->width();
    if (plotW < 50.0) plotW = 1000.0;   // first replot, before any layout
    const int budget =
        std::max(8, int(plotW / (kMarkerPitch * cellMarkerSize_->value())));
    const int markEvery = std::max(1, (n + budget - 1) / budget);
    const bool marking = canMark && cellMarkers_->isChecked();
    if (marking && markEvery > 1)
        cellInfo_->setText(cellInfo_->text()
                           + QStringLiteral("  -  one marker per %1 cells").arg(markEvery));
    // B is drawn over A, and in a comparison the two usually agree - a filled
    // B would simply erase A. Hollow it out so A shows through, the same
    // bargain the dashed B curve already strikes with the solid A curve.
    auto marked = [&](const QColor& c, QScatterSeries::MarkerShape shape,
                      bool hollow = false) {
        auto* m = new QScatterSeries;
        m->setMarkerSize(cellMarkerSize_->value());
        m->setMarkerShape(shape);
        if (hollow) {
            m->setBrush(Qt::NoBrush);
            QPen pen(c);
            pen.setWidthF(1.5);
            m->setPen(pen);
        } else {
            m->setColor(c);
            m->setBorderColor(Qt::white);
        }
        return m;
    };
    chart->setTitle(QStringLiteral("%1  at %2")
                             .arg(cellProp_->currentText(), cellDate_->currentText()));

    // Ranges by hand throughout: with axes added manually, attachAxis leaves
    // the range where the first series put it, and the second run would be
    // off the scale exactly when the two disagree.
    auto pad = [](double lo, double hi) {
        double p = (hi - lo) * 0.05;
        if (p <= 0.0) p = (hi == 0.0) ? 1.0 : std::max(1e-30, std::abs(hi) * 0.1);
        return std::pair<double, double>(lo - p, hi + p);
    };

    if (mode == 2) {
        // Each cell a point; agreement is the diagonal.
        auto* sc = new QScatterSeries;
        sc->setMarkerSize(7.0);
        sc->setColor(QColor(0x33, 0x66, 0x99, 0xa0));
        sc->setBorderColor(Qt::transparent);
        double lo = va[0], hi = va[0];
        for (int i = 0; i < n; ++i) {
            sc->append(double(va[i]), double(vb[i]));
            lo = std::min({ lo, double(va[i]), double(vb[i]) });
            hi = std::max({ hi, double(va[i]), double(vb[i]) });
        }
        const auto [rlo, rhi] = pad(lo, hi);
        auto* diag = new QLineSeries;
        diag->setPen(QPen(QColor(0x88, 0x8e, 0x94), 1.0, Qt::DashLine));
        diag->append(rlo, rlo); diag->append(rhi, rhi);
        chart->addSeries(diag);
        chart->addSeries(sc);
        auto* ax = new QValueAxis; ax->setTitleText(QStringLiteral("A"));
        auto* ay = new QValueAxis; ay->setTitleText(QStringLiteral("B"));
        chart->addAxis(ax, Qt::AlignBottom);
        chart->addAxis(ay, Qt::AlignLeft);
        for (auto* ser : chart->series()) { ser->attachAxis(ax); ser->attachAxis(ay); }
        ax->setRange(rlo, rhi); ay->setRange(rlo, rhi);
        chart->legend()->hide();
        { present(); return; }
    }

    auto* ax = new QValueAxis;
    ax->setTitleText(QStringLiteral("active cell"));
    ax->setLabelFormat(QStringLiteral("%d"));
    auto* ay = new QValueAxis;
    chart->addAxis(ax, Qt::AlignBottom);
    chart->addAxis(ay, Qt::AlignLeft);

    if (mode == 1) {
        auto* sd = new QLineSeries;
        sd->setPen(QPen(QColor(0x44, 0x2a, 0x66), 2.0));
        double lo = 0.0, hi = 0.0;      // the zero line stays in view
        for (int i = 0; i < n; ++i) {
            const double d = double(va[i]) - double(vb[i]);
            sd->append(i + 1, d);
            lo = std::min(lo, d); hi = std::max(hi, d);
        }
        chart->addSeries(sd);
        sd->attachAxis(ax); sd->attachAxis(ay);
        if (marking) {
            auto* md = marked(QColor(0x44, 0x2a, 0x66), QScatterSeries::MarkerShapeCircle);
            for (int i = 0; i < n; i += markEvery)
                md->append(i + 1, double(va[i]) - double(vb[i]));
            chart->addSeries(md);
            md->attachAxis(ax); md->attachAxis(ay);
        }
        const auto [rlo, rhi] = pad(lo, hi);
        ay->setRange(rlo, rhi);
        chart->legend()->hide();
    } else {
        auto* sa2 = new QLineSeries; sa2->setName(QStringLiteral("A"));
        sa2->setPen(QPen(colA, 2.0));
        auto* sb2 = new QLineSeries; sb2->setName(QStringLiteral("B"));
        sb2->setPen(QPen(colB, 2.0, Qt::DashLine));
        double lo = va[0], hi = va[0];
        for (int i = 0; i < n; ++i) {
            sa2->append(i + 1, double(va[i]));
            sb2->append(i + 1, double(vb[i]));
            lo = std::min({ lo, double(va[i]), double(vb[i]) });
            hi = std::max({ hi, double(va[i]), double(vb[i]) });
        }
        chart->addSeries(sa2);
        chart->addSeries(sb2);
        sa2->attachAxis(ax); sa2->attachAxis(ay);
        sb2->attachAxis(ax); sb2->attachAxis(ay);
        if (marking) {
            auto* ma2 = marked(colA, QScatterSeries::MarkerShapeCircle);
            auto* mb2 = marked(colB, QScatterSeries::MarkerShapeRectangle, true);
            for (int i = 0; i < n; i += markEvery) {
                ma2->append(i + 1, double(va[i]));
                mb2->append(i + 1, double(vb[i]));
            }
            chart->addSeries(ma2); chart->addSeries(mb2);
            ma2->attachAxis(ax); ma2->attachAxis(ay);
            mb2->attachAxis(ax); mb2->attachAxis(ay);
            // The markers stand for the curves already named, so they add
            // nothing to the legend but a second copy of each name.
            for (auto* m : chart->legend()->markers(ma2)) m->setVisible(false);
            for (auto* m : chart->legend()->markers(mb2)) m->setVisible(false);
        }
        const auto [rlo, rhi] = pad(lo, hi);
        ay->setRange(rlo, rhi);
        chart->legend()->show();
        chart->legend()->setAlignment(Qt::AlignBottom);
    }
    ax->setRange(1, std::max(2, n));
    ay->applyNiceNumbers();
    present();
}

void RestartComparePanel::replotHistory()
{
    // Fresh chart per plot, swapped in whole - same reason as replotCells():
    // emptying a QChart in place leaves ghosts painting in the scene.
    auto* chart = new QChart;
    chart->setBackgroundRoundness(0);
    chart->setDropShadowEnabled(false);
    auto present = [this, chart] {
        QChart* old = histChartView_->chart();
        histChartView_->setChart(chart);
        if (old != chart) delete old;
    };

    const int ia = caseA_->currentIndex(), ib = caseB_->currentIndex();
    auto ra = (ia >= 0 && ia < cases_.size()) ? rstReader(cases_[ia].smspec) : nullptr;
    auto rb = (ib >= 0 && ib < cases_.size()) ? rstReader(cases_[ib].smspec) : nullptr;
    const int cell = histCell_->value() - 1;
    if (!ra || !rb || histProp_->currentText().isEmpty() || cellTimes_.isEmpty()) {
        histInfo_->setText(QStringLiteral("pick two cases with a restart (UNRST) file"));
        present();
        return;
    }

    // The chosen cell of the chosen field, at every common date. One array
    // read per date per case - a look, not a sweep; the full comparison is
    // what streams everything.
    const std::string key = histProp_->currentText().toStdString();
    QVector<double> when, va, vb;
    for (const QDateTime& d : std::as_const(cellTimes_)) {
        try {
            const auto& a = ra->getRestartData<float>(key, cellDatesA_.value(d));
            const auto& b = rb->getRestartData<float>(key, cellDatesB_.value(d));
            if (cell >= int(a.size()) || cell >= int(b.size())) continue;
            when << double(d.toMSecsSinceEpoch());
            va << double(a[cell]);
            vb << double(b[cell]);
        } catch (...) {}
    }
    if (when.isEmpty()) {
        histInfo_->setText(QStringLiteral("no common report date carries %1")
                               .arg(histProp_->currentText()));
        present();
        return;
    }

    const DiffTol tol{ absTol_->value(), relTol_->value() };
    int nBad = 0, worstAt = 0;
    double maxAbs = 0.0;
    for (int i = 0; i < when.size(); ++i) {
        const double d = std::abs(va[i] - vb[i]);
        if (d > maxAbs) { maxAbs = d; worstAt = i; }
        if (diffIsSignificant(va[i], vb[i], tol)) ++nBad;
    }
    const QDateTime worstDate = QDateTime::fromMSecsSinceEpoch(qint64(when[worstAt]),
                                                               QTimeZone::utc());
    histInfo_->setText(maxAbs == 0.0
        ? QStringLiteral("identical at all %1 common dates").arg(when.size())
        : QStringLiteral("%1 of %2 dates outside tolerance;  max |A-B| %3 at %4 "
                         "(A %5, B %6)")
              .arg(nBad).arg(when.size()).arg(maxAbs, 0, 'g', 4)
              .arg(worstDate.toString(QStringLiteral("yyyy-MM-dd hh:mm")))
              .arg(va[worstAt], 0, 'g', 6).arg(vb[worstAt], 0, 'g', 6));

    chart->setTitle(QStringLiteral("%1  -  cell %2")
                        .arg(histProp_->currentText()).arg(cell + 1));

    // Lines with a marker on every sample, the way the summary plots mark
    // theirs: report steps are few and exactly where a value was written is
    // the point of looking.
    const QColor colA(0x1f, 0x77, 0xb4), colB(0xd6, 0x27, 0x28);
    auto* la = new QLineSeries; la->setName(QStringLiteral("A"));
    la->setPen(QPen(colA, 2.0));
    auto* lb = new QLineSeries; lb->setName(QStringLiteral("B"));
    lb->setPen(QPen(colB, 2.0, Qt::DashLine));
    for (int i = 0; i < when.size(); ++i) {
        la->append(when[i], va[i]);
        lb->append(when[i], vb[i]);
    }
    chart->addSeries(la); chart->addSeries(lb);
    const bool hmark = histMarkers_->isChecked();
    histMarkerSize_->setEnabled(hmark);
    if (hmark) {
        const double sz = histMarkerSize_->value();
        auto* ma = new QScatterSeries;
        ma->setMarkerSize(sz); ma->setColor(colA); ma->setBorderColor(Qt::white);
        // Hollow, for the same reason as in the cell view: B lands on top of
        // A wherever the runs agree, and a filled B would hide it.
        auto* mb = new QScatterSeries;
        mb->setMarkerSize(sz);
        mb->setBrush(Qt::NoBrush);
        QPen bpen(colB);
        bpen.setWidthF(1.5);
        mb->setPen(bpen);
        mb->setMarkerShape(QScatterSeries::MarkerShapeRectangle);
        for (int i = 0; i < when.size(); ++i) {
            ma->append(when[i], va[i]);
            mb->append(when[i], vb[i]);
        }
        chart->addSeries(ma); chart->addSeries(mb);
        // The markers stand for curves the legend already names.
        for (auto* m : chart->legend()->markers(ma)) m->setVisible(false);
        for (auto* m : chart->legend()->markers(mb)) m->setVisible(false);
    }
    chart->legend()->setAlignment(Qt::AlignBottom);

    auto* ax = new QDateTimeAxis;
    auto* ay = new QValueAxis;
    chart->addAxis(ax, Qt::AlignBottom);
    chart->addAxis(ay, Qt::AlignLeft);
    for (auto* ser : chart->series()) { ser->attachAxis(ax); ser->attachAxis(ay); }

    // Ranges by hand, as everywhere in this panel: attachAxis keeps the first
    // series' range, and B off the scale is the failure mode of a comparison.
    double lo = va[0], hi = va[0];
    for (int i = 0; i < when.size(); ++i) {
        lo = std::min({ lo, va[i], vb[i] });
        hi = std::max({ hi, va[i], vb[i] });
    }
    if (hi > lo) { ay->setRange(lo, hi); ay->applyNiceNumbers(); }
    else {
        const double pad = (hi == 0.0) ? 1.0 : std::max(1e-30, std::abs(hi) * 0.1);
        ay->setRange(hi - pad, hi + pad);
    }
    ax->setRange(QDateTime::fromMSecsSinceEpoch(qint64(when.first()), QTimeZone::utc()),
                 QDateTime::fromMSecsSinceEpoch(qint64(when.last()), QTimeZone::utc()));
    const double days = (when.last() - when.first()) / 86400.0e3;
    ax->setFormat(days <= 3.0    ? QStringLiteral("d MMM hh:mm")
                : days <= 92.0   ? QStringLiteral("d MMM")
                : days <= 1100.0 ? QStringLiteral("MMM yy")
                                 : QStringLiteral("yyyy"));
    present();
}

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
    cellCasesChanged();
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
    heat_->setResult(&result_, metric_->currentIndex(), onlyBad_->isChecked());
}

} // namespace flowgui
