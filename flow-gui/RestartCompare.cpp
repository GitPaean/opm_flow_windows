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
#include <QLineSeries>
#include <QLogValueAxis>
#include <QMap>
#include <QTimeZone>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QGridLayout>
#include <QValueAxis>
#include <QThread>
#include <QTimer>
#include <QToolTip>
#include <QValueAxis>
#include <QVBoxLayout>

#include <opm/io/eclipse/EGrid.hpp>
#include <opm/io/eclipse/ESmry.hpp>
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
// The field vectors a comparison usually starts from, grouped the way an
// engineer thinks of them. Curated by hand and deliberately field-level only:
// well-by-well questions belong in the Summary Plots tab, where every vector
// of every case is there to pick. What a deck carries beyond this list is not
// lost - it appears under "other field vectors" below the curated groups,
// which is where a compositional run's own F* vectors land.
struct CuratedVec { const char* key; const char* name; };
struct CuratedGroup { const char* title; std::initializer_list<CuratedVec> vecs; };
static const CuratedGroup kCuratedField[] = {
    { "Production rates",
      { { "FOPR", "Oil Production Rate" },
        { "FWPR", "Water Production Rate" },
        { "FGPR", "Gas Production Rate" },
        { "FLPR", "Liquid Production Rate" },
        { "FVPR", "Reservoir Voidage Production Rate" } } },
    { "Production totals",
      { { "FOPT", "Oil Production Total" },
        { "FWPT", "Water Production Total" },
        { "FGPT", "Gas Production Total" },
        { "FLPT", "Liquid Production Total" } } },
    { "Injection",
      { { "FWIR", "Water Injection Rate" },
        { "FWIT", "Water Injection Total" },
        { "FGIR", "Gas Injection Rate" },
        { "FGIT", "Gas Injection Total" } } },
    { "Pressure & in place",
      { { "FPR",  "Average Reservoir Pressure" },
        { "FOIP", "Oil In Place" },
        { "FGIP", "Gas In Place" },
        { "FWIP", "Water In Place" } } },
    { "Ratios & fractions",
      { { "FWCT", "Water Cut" },
        { "FGOR", "Gas-Oil Ratio" } } },
};
// Ticked by default where the cases have them: the first look should already
// be a picture, not a list of unticked boxes.
static const char* kDefaultTicked[] = { "FOPR", "FWPR", "FPR", "FWCT" };

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

    // The summary-vector view: the comparison most days actually need. The
    // restart views keep answering the deeper "do the cells agree" question.
    vecTree_ = new QTreeWidget;
    vecTree_->setHeaderHidden(true);
    vecTree_->setMaximumWidth(280);
    vecTree_->setMinimumWidth(190);
    sumHead_ = new QLabel;
    sumHead_->setTextFormat(Qt::RichText);
    auto* sumScroll = new QScrollArea;
    sumScroll->setWidgetResizable(true);
    sumScroll->setFrameShape(QFrame::NoFrame);
    auto* gridHost = new QWidget;
    sumGrid_ = new QGridLayout(gridHost);
    sumGrid_->setContentsMargins(0, 0, 0, 0);
    sumScroll->setWidget(gridHost);
    sumView_ = new QWidget;
    {
        auto* right = new QVBoxLayout;
        right->setContentsMargins(0, 0, 0, 0);
        right->addWidget(sumHead_);
        right->addWidget(sumScroll, 1);
        auto* srow = new QHBoxLayout(sumView_);
        srow->setContentsMargins(4, 4, 4, 4);
        srow->addWidget(vecTree_);
        srow->addLayout(right, 1);
    }

    views_ = new QTabWidget;
    views_->addTab(sumView_, QStringLiteral("Summary vectors"));
    views_->addTab(heat_, QStringLiteral("Restart overview"));
    views_->addTab(chartView_, QStringLiteral("Restart curves"));

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

    lower_ = new QSplitter(Qt::Horizontal);
    lower_->addWidget(table_);
    lower_->addWidget(detailBox);
    lower_->setStretchFactor(0, 2);
    lower_->setStretchFactor(1, 3);

    auto* split = new QSplitter(Qt::Vertical);
    split->addWidget(views_);
    split->addWidget(lower_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    // Explicit sizes: stretch factors alone let the tables' size hints squeeze
    // the views to a sliver once they have a few hundred rows in them.
    split->setSizes({ 460, 320 });
    lower_->setSizes({ 380, 620 });
    // The tables under the views belong to the restart comparison; while the
    // summary view is up they would only take its height.
    lower_->setVisible(false);

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
            [this](int) { syncCaseTips(); summaryCasesChanged(); });
    connect(caseB_, &QComboBox::currentIndexChanged, this,
            [this](int) { syncCaseTips(); summaryCasesChanged(); });
    connect(views_, &QTabWidget::currentChanged, this, [this](int idx) {
        lower_->setVisible(idx != views_->indexOf(sumView_));
        if (idx == views_->indexOf(sumView_) && sumDirty_) { rebuildVectorList(); replotSummary(); }
    });
    connect(vecTree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem*, int) {
        if (!sumBuilding_) replotSummary();
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
    if (views_ && views_->currentWidget() == sumView_ && sumDirty_) {
        rebuildVectorList();
        replotSummary();
    }
}

std::shared_ptr<Opm::EclIO::ESmry> RestartComparePanel::summaryReader(const QString& smspec)
{
    if (smspec.isEmpty()) return nullptr;
    // Same staleness currency as the plots tab: a re-run of the case rewrites
    // these files, and comparing yesterday's read of them would be worse than
    // the reopen it saves.
    QString stamp;
    for (const QString& ext : { QStringLiteral("SMSPEC"), QStringLiteral("UNSMRY"),
                                QStringLiteral("ESMRY") }) {
        QString f = smspec; f.chop(6); f += ext;
        const QFileInfo fi(f);
        if (fi.exists())
            stamp += QStringLiteral("%1|%2;").arg(fi.lastModified().toMSecsSinceEpoch())
                                             .arg(fi.size());
    }
    auto it = sumReaders_.find(smspec);
    if (it != sumReaders_.end() && it->second.stamp == stamp && it->second.smry)
        return it->second.smry;
    std::shared_ptr<Opm::EclIO::ESmry> r;
    try {
        r = std::make_shared<Opm::EclIO::ESmry>(smspec.toStdString());
    } catch (...) { r = nullptr; }
    sumReaders_[smspec] = SumReader{ r, stamp };
    return r;
}

void RestartComparePanel::summaryCasesChanged()
{
    sumDirty_ = true;
    if (isVisible() && views_ && views_->currentWidget() == sumView_) {
        rebuildVectorList();
        replotSummary();
    }
}

void RestartComparePanel::rebuildVectorList()
{
    sumDirty_ = false;

    // Keep the user's ticks across a rebuild; only a first-ever fill applies
    // the defaults, so unticking FOPR is not undone by switching case B.
    QHash<QString, bool> was;
    for (int i = 0; i < vecTree_->topLevelItemCount(); ++i) {
        auto* g = vecTree_->topLevelItem(i);
        for (int j = 0; j < g->childCount(); ++j)
            was.insert(g->child(j)->text(0).section(QLatin1Char(' '), 0, 0),
                       g->child(j)->checkState(0) == Qt::Checked);
    }
    const bool firstFill = was.isEmpty();

    sumBuilding_ = true;
    vecTree_->clear();

    const int ia = caseA_->currentIndex(), ib = caseB_->currentIndex();
    auto ra = (ia >= 0 && ia < cases_.size()) ? summaryReader(cases_[ia].smspec) : nullptr;
    auto rb = (ib >= 0 && ib < cases_.size()) ? summaryReader(cases_[ib].smspec) : nullptr;
    if (!ra || !rb) { sumBuilding_ = false; return; }

    // What both sides carry: comparing needs the vector on each.
    QSet<QString> both;
    for (const auto& k : ra->keywordList())
        if (!k.empty() && k.front() == 'F') both.insert(QString::fromStdString(k));
    QSet<QString> keep;
    for (const auto& k : rb->keywordList()) {
        const QString q = QString::fromStdString(k);
        if (both.contains(q)) keep.insert(q);
    }

    auto ticked = [&](const QString& key) {
        if (!firstFill) return was.value(key, false);
        for (const char* d : kDefaultTicked) if (key == QLatin1String(d)) return true;
        return false;
    };
    auto addLeaf = [&](QTreeWidgetItem* parent, const QString& key, const QString& name) {
        auto* it = new QTreeWidgetItem(parent);
        it->setText(0, name.isEmpty() ? key : key + QStringLiteral("  -  ") + name);
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        it->setCheckState(0, ticked(key) ? Qt::Checked : Qt::Unchecked);
        try {
            it->setToolTip(0, QString::fromStdString(ra->get_unit(key.toStdString())));
        } catch (...) {}
    };

    QSet<QString> placed;
    for (const auto& grp : kCuratedField) {
        QVector<const CuratedVec*> have;
        for (const auto& v : grp.vecs)
            if (keep.contains(QLatin1String(v.key))) have.push_back(&v);
        if (have.isEmpty()) continue;
        auto* g = new QTreeWidgetItem(vecTree_);
        g->setText(0, QLatin1String(grp.title));
        g->setFlags(Qt::ItemIsEnabled);
        QFont f = g->font(0); f.setBold(true); g->setFont(0, f);
        for (const auto* v : have) {
            addLeaf(g, QLatin1String(v->key), QLatin1String(v->name));
            placed.insert(QLatin1String(v->key));
        }
        g->setExpanded(true);
    }

    // The rest of what the two decks share - a compositional run's field
    // vectors live here, since the curated groups above are black-oil ones.
    QStringList rest;
    for (const QString& k : keep) if (!placed.contains(k)) rest << k;
    rest.sort();
    if (!rest.isEmpty()) {
        auto* g = new QTreeWidgetItem(vecTree_);
        g->setText(0, QStringLiteral("other field vectors"));
        g->setFlags(Qt::ItemIsEnabled);
        QFont f = g->font(0); f.setBold(true); g->setFont(0, f);
        for (const QString& k : rest) addLeaf(g, k, QString());
        g->setExpanded(rest.size() <= 12);
    }
    sumBuilding_ = false;
}

void RestartComparePanel::replotSummary()
{
    // Wholesale: a handful of small charts is cheap to rebuild, and partial
    // updates would have to track which chart belongs to which vector.
    while (QLayoutItem* it = sumGrid_->takeAt(0)) {
        delete it->widget();
        delete it;
    }

    const int ia = caseA_->currentIndex(), ib = caseB_->currentIndex();
    auto ra = (ia >= 0 && ia < cases_.size()) ? summaryReader(cases_[ia].smspec) : nullptr;
    auto rb = (ib >= 0 && ib < cases_.size()) ? summaryReader(cases_[ib].smspec) : nullptr;
    if (!ra || !rb) {
        sumHead_->setText(QStringLiteral("pick two cases with summary output "
                                         "(run a job, or Add case...)"));
        return;
    }
    const QColor colA(0x1f, 0x77, 0xb4), colB(0xd6, 0x27, 0x28);
    sumHead_->setText(QStringLiteral(
        "<span style=\"color:%1\"><b>&#9644;</b> A  %2</span>&nbsp;&nbsp;&nbsp;"
        "<span style=\"color:%3\"><b>&#9644; &#9644;</b> B  %4</span>")
        .arg(colA.name(), caseA_->currentText().toHtmlEscaped(),
             colB.name(), caseB_->currentText().toHtmlEscaped()));

    // The ticked vectors, in list order.
    QStringList picks;
    for (int i = 0; i < vecTree_->topLevelItemCount(); ++i) {
        auto* g = vecTree_->topLevelItem(i);
        for (int j = 0; j < g->childCount(); ++j)
            if (g->child(j)->checkState(0) == Qt::Checked)
                picks << g->child(j)->text(0).section(QLatin1Char(' '), 0, 0);
    }
    if (picks.isEmpty()) {
        auto* hint = new QLabel(QStringLiteral("tick vectors on the left to compare them"));
        hint->setAlignment(Qt::AlignCenter);
        sumGrid_->addWidget(hint, 0, 0);
        return;
    }

    auto msDates = [](Opm::EclIO::ESmry& r) {
        QVector<qint64> out;
        try {
            for (const auto& tp : r.dates())
                out.push_back(qint64(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         tp.time_since_epoch()).count()));
        } catch (...) {}
        return out;
    };
    const QVector<qint64> ta = msDates(*ra), tb = msDates(*rb);

    const int cols = picks.size() > 1 ? 2 : 1;
    for (int n = 0; n < picks.size(); ++n) {
        const std::string key = picks[n].toStdString();
        std::vector<float> va, vb;
        try { va = ra->get(key); } catch (...) {}
        try { vb = rb->get(key); } catch (...) {}
        QString unit;
        try { unit = QString::fromStdString(ra->get_unit(key)); } catch (...) {}

        auto* chart = new QChart;
        chart->setBackgroundRoundness(0);
        chart->setDropShadowEnabled(false);
        chart->legend()->hide();
        chart->setTitle(picks[n] + (unit.isEmpty() ? QString()
                                                   : QStringLiteral("  [%1]").arg(unit)));

        auto* sa = new QLineSeries; sa->setPen(QPen(colA, 2.0));
        auto* sb = new QLineSeries; sb->setPen(QPen(colB, 2.0, Qt::DashLine));
        // A compositional run writes NaN where a ratio has no meaning yet
        // (FWCT before any water moves); one NaN point poisons the whole
        // axis range, so non-finite samples are left out of the drawing.
        for (int i = 0; i < int(va.size()) && i < ta.size(); ++i)
            if (std::isfinite(va[i])) sa->append(double(ta[i]), double(va[i]));
        for (int i = 0; i < int(vb.size()) && i < tb.size(); ++i)
            if (std::isfinite(vb[i])) sb->append(double(tb[i]), double(vb[i]));
        chart->addSeries(sa);
        chart->addSeries(sb);

        auto* ax = new QDateTimeAxis;
        auto* ay = new QValueAxis;
        chart->addAxis(ax, Qt::AlignBottom);
        chart->addAxis(ay, Qt::AlignLeft);
        sa->attachAxis(ax); sa->attachAxis(ay);
        sb->attachAxis(ax); sb->attachAxis(ay);

        // The ranges, by hand. With axes added manually, attachAxis leaves
        // the range where the FIRST series put it - so when the two runs sit
        // apart (devel's FPR near zero, current's at 152 bar), B is off the
        // scale and simply invisible, which on a comparison chart is the one
        // thing that must not happen. Union over both, and over finite
        // samples only.
        double ymin = 0, ymax = 0, xmin = 0, xmax = 0;
        bool any = false;
        auto fold = [&](const QVector<qint64>& t, const std::vector<float>& v) {
            for (int i = 0; i < int(v.size()) && i < t.size(); ++i) {
                if (!std::isfinite(v[i])) continue;
                if (!any) { ymin = ymax = v[i]; xmin = xmax = double(t[i]); any = true; continue; }
                ymin = std::min(ymin, double(v[i])); ymax = std::max(ymax, double(v[i]));
                xmin = std::min(xmin, double(t[i])); xmax = std::max(xmax, double(t[i]));
            }
        };
        fold(ta, va);
        fold(tb, vb);
        if (any) {
            if (ymax > ymin) {
                ay->setRange(ymin, ymax);
                ay->applyNiceNumbers();
            } else {
                // A flat line (FOPR of a case with no producing wells is zero
                // throughout) gets a band to sit in the middle of; nice-
                // numbering a [v,v] range computes over log10(0) and warns.
                const double pad = std::max(1.0, std::abs(ymax) * 0.1);
                ay->setRange(ymax - pad, ymax + pad);
            }
            ax->setRange(QDateTime::fromMSecsSinceEpoch(qint64(xmin), QTimeZone::utc()),
                         QDateTime::fromMSecsSinceEpoch(qint64(xmax), QTimeZone::utc()));
            // Label dates at the size the span needs: a run of days labelled
            // "MMM yy" is five ticks reading Jan 16.
            const double days = (xmax - xmin) / 86400.0e3;
            ax->setFormat(days <= 3.0    ? QStringLiteral("d MMM hh:mm")
                        : days <= 92.0   ? QStringLiteral("d MMM")
                        : days <= 1100.0 ? QStringLiteral("MMM yy")
                                         : QStringLiteral("yyyy"));
        }

        // Where the two runs sit furthest apart - paired by exact report
        // date, the same rule the restart comparison uses: interpolating
        // between different steppings would put a number on a difference
        // that is partly the interpolation's.
        QHash<qint64, float> atB;
        for (int i = 0; i < int(vb.size()) && i < tb.size(); ++i)
            if (std::isfinite(vb[i])) atB.insert(tb[i], vb[i]);
        double worst = 0.0; qint64 when = 0; int paired = 0;
        for (int i = 0; i < int(va.size()) && i < ta.size(); ++i) {
            if (!std::isfinite(va[i])) continue;
            const auto it = atB.constFind(ta[i]);
            if (it == atB.constEnd()) continue;
            ++paired;
            const double d = std::abs(double(va[i]) - double(*it));
            if (d > worst) { worst = d; when = ta[i]; }
        }
        QString capText;
        if (paired == 0)
            capText = QStringLiteral("no common report dates");
        else if (worst == 0.0)
            capText = QStringLiteral("identical at all %1 common dates").arg(paired);
        else
            capText = QStringLiteral("max |A-B|  %1 %2  at %3")
                          .arg(worst, 0, 'g', 4).arg(unit,
                               QDateTime::fromMSecsSinceEpoch(when, QTimeZone::utc())
                                   .toString(QStringLiteral("d MMM yyyy")));
        auto* cap = new QLabel(capText);
        cap->setAlignment(Qt::AlignHCenter);
        cap->setStyleSheet(QStringLiteral("color:#555b61;"));

        auto* cell = new QWidget;
        auto* cl = new QVBoxLayout(cell);
        cl->setContentsMargins(0, 0, 0, 6);
        auto* cv = new QChartView(chart);
        cv->setRenderHint(QPainter::Antialiasing);
        cv->setMinimumHeight(230);
        cl->addWidget(cv, 1);
        cl->addWidget(cap);
        sumGrid_->addWidget(cell, n / cols, n % cols);
    }
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
    summaryCasesChanged();
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
