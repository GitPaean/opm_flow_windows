/*
  Copyright (C) 2026 SINTEF Digital

  SummaryPlotWidget implementation. Part of the opm_flow_windows harness;
  GPL v3+ (see repository LICENSE).
*/
#include "SummaryPlotWidget.h"

#include "CasePath.h"
#include "GuiPaths.h"

#include <opm/io/eclipse/ESmry.hpp>
#include <opm/io/eclipse/EclFile.hpp>
#include <opm/io/eclipse/EclUtil.hpp>

#include <QCheckBox>
#include <QChart>
#include <QChartView>
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeAxis>
#include <QDoubleSpinBox>
#include <QDir>
#include <QTimeZone>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextStream>
#include <QLabel>
#include <QLineEdit>
#include <QLegendMarker>
#include <QLineSeries>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QPen>
#include <QPolygonF>
#include <QScatterSeries>
#include <QShortcut>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QShowEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QCursor>
#include <QToolTip>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QValueAxis>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using Opm::EclIO::ESmry;
using Cat  = Opm::EclIO::SummaryNode::Category;
using Type = Opm::EclIO::SummaryNode::Type;

namespace {

// Subplot layouts offered, rows x columns. 4x4 is the end of the ladder:
// beyond that a subplot is smaller than its own axis labels on any screen
// anyone actually has, and the grid stops being plots and becomes sparklines.
// The subplot grid is picked as a SHAPE, not from a list of presets: what a
// wide screen wants is 2x5 or 3x6, and a list long enough to hold those is a
// list nobody reads. More columns than rows on offer for the same reason.
//
// Where the grid stops rather than where it should be used: 4x4 is already
// past the point where a date axis fits on a 1920 screen (the tick count
// thins to suit, see plotChart), and the far end is for a wall of screen or a
// deliberately small-multiples look. It is not our business to say no.
constexpr int kMaxRows = 6;
constexpr int kMaxCols = 8;
constexpr int kMaxCharts = kMaxRows * kMaxCols;

// Hover a cell to choose that many rows and columns, the way office suites
// pick a table size - the shape is the point, so show it rather than name it.
class LayoutPicker : public QWidget
{
public:
    using Chosen = std::function<void(int rows, int cols)>;

    LayoutPicker(int rows, int cols, Chosen chosen, QWidget* parent = nullptr)
        : QWidget(parent), rows_(rows), cols_(cols), chosen_(std::move(chosen))
    {
        setMouseTracking(true);
        setFixedSize(kMargin * 2 + cols_ * kCell, kMargin * 2 + rows_ * kCell + kText);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        for (int r = 0; r < rows_; ++r) {
            for (int c = 0; c < cols_; ++c) {
                const QRect cell(kMargin + c * kCell, kMargin + r * kCell,
                                 kCell - 2, kCell - 2);
                const bool on = r <= hoverRow_ && c <= hoverCol_;
                p.fillRect(cell, on ? QColor(0x2f, 0x6f, 0xd0) : QColor(0xff, 0xff, 0xff));
                p.setPen(QColor(on ? 0x1a : 0xb0, on ? 0x50 : 0xb6, on ? 0xa0 : 0xbc));
                p.drawRect(cell);
            }
        }
        p.setPen(QColor(0x22, 0x26, 0x2b));
        const QRect text(0, kMargin + rows_ * kCell, width(), kText);
        const int n = (hoverRow_ + 1) * (hoverCol_ + 1);
        p.drawText(text, Qt::AlignCenter,
                   hoverRow_ < 0 ? QStringLiteral("pick a grid")
                   : n == 1      ? QStringLiteral("1 chart")
                                 : QStringLiteral("%1 x %2  (%3 charts)")
                                       .arg(hoverRow_ + 1).arg(hoverCol_ + 1).arg(n));
    }
    void mouseMoveEvent(QMouseEvent* ev) override
    {
        const int c = (ev->position().x() - kMargin) / kCell;
        const int r = (ev->position().y() - kMargin) / kCell;
        hoverRow_ = std::clamp(r, 0, rows_ - 1);
        hoverCol_ = std::clamp(c, 0, cols_ - 1);
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (hoverRow_ >= 0 && chosen_) chosen_(hoverRow_ + 1, hoverCol_ + 1);
    }
    void leaveEvent(QEvent*) override { hoverRow_ = hoverCol_ = -1; update(); }

private:
    static constexpr int kCell = 18, kMargin = 6, kText = 20;
    int rows_, cols_;
    int hoverRow_ = -1, hoverCol_ = -1;
    Chosen chosen_;
};

// The legend's size before any scaling; plotChart() scales from here.
constexpr double kLegendPointSize = 10.5;

const int RoleVecIndex = Qt::UserRole + 1;   // leaf item -> index into vecs_
const int RoleCaseLabel = Qt::UserRole + 2;  // case item -> its current name
const int RoleCaseBase  = Qt::UserRole + 3;  // ... -> its name before tagging
const int RoleCaseCustom = Qt::UserRole + 4; // ... -> the user typed the name
const int RoleCaseSeq   = Qt::UserRole + 5;  // ... -> when it was loaded

// legend placement (values stored in legendBox_)
enum LegendPos { LegendBottom, LegendTop, LegendLeft, LegendRight,
                 LegendInTL, LegendInTR, LegendInBL, LegendInBR, LegendOff };

// Curve colours: the Okabe-Ito qualitative palette, which stays readable for
// the common forms of colour blindness and in greyscale print. Colour keys the
// VECTOR; the dash pattern below keys the CASE, so a comparison is legible
// even printed in black and white.
const QColor kCurveColors[] = {
    QColor(0x00, 0x72, 0xB2), QColor(0xD5, 0x5E, 0x00), QColor(0x00, 0x9E, 0x73),
    QColor(0xCC, 0x79, 0xA7), QColor(0xE6, 0x9F, 0x00), QColor(0x56, 0xB4, 0xE9),
    QColor(0x8a, 0x6d, 0x3b), QColor(0x33, 0x33, 0x33),
};
constexpr int kCurveColorCount = int(sizeof(kCurveColors) / sizeof(kCurveColors[0]));

const Qt::PenStyle kCaseDashes[] = { Qt::SolidLine, Qt::DashLine,
                                     Qt::DotLine,   Qt::DashDotLine };
constexpr int kCaseDashCount = int(sizeof(kCaseDashes) / sizeof(kCaseDashes[0]));

// Object name of the empty stand-in series that carries a curve's legend
// entry (see plotChart); its marker is the one the legend actually shows.
const QString kLegendSample = QStringLiteral("flow-gui.legend-sample");

// The pen a legend sample is drawn with: the curve's colour and case dash,
// but heavier - Qt draws the sample with the SERIES' pen, so at curve width
// the legend is left with a hairline beside the label. Qt also measures a
// dash pattern in multiples of the pen width, so a heavier pen on its own
// would stretch the dashes past the (font-height sized) sample and leave
// every case looking solid; pin the pattern to lengths in pixels instead,
// and cut the caps flat so they do not fill the gaps back in.
QPen legendPen(const QPen& curve, qreal width)
{
    QPen p(curve);
    p.setWidthF(width);
    p.setCapStyle(Qt::FlatCap);
    const qreal on = 5.0 / width, off = 3.0 / width, dot = 1.5 / width;
    switch (curve.style()) {
    case Qt::DashLine:    p.setDashPattern({on, off});           break;
    case Qt::DotLine:     p.setDashPattern({dot, off});          break;
    case Qt::DashDotLine: p.setDashPattern({on, off, dot, off}); break;
    default:              break;                 // solid: nothing to rescale
    }
    return p;
}

// "WECON-02 [tuning_fix]" -> "WECON-02", but only when the bracketed part is
// really a tag this widget added: a directory on the case's own path. A name
// the user typed that happens to end in brackets is left alone.
QString untaggedLabel(const QString& shown, const QString& smspecPath)
{
    if (!shown.endsWith(QLatin1Char(']'))) return shown;
    const int open = shown.lastIndexOf(QStringLiteral(" ["));
    if (open <= 0) return shown;
    const QString tag = shown.mid(open + 2, shown.size() - open - 3);
    const QStringList dirs = QFileInfo(smspecPath).absolutePath()
                                 .split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& part : dirs)
        if (tag == part || tag.endsWith(QLatin1Char('/') + part))
            return shown.left(open);
    return shown;
}

// The scatter shape a case is marked with, drawn as an image for its legend
// entry: Qt paints a series' "light marker" over that series' legend sample,
// which is how the entry can show a line AND the marker on it. The stand-in
// series this is set on carries no points, so it only ever shows up in the
// legend - the curve itself keeps the real (vector) scatter overlay.
//
// `box` is the whole sample, `size` the marker drawn in the middle of it.
QImage sampleMarker(QScatterSeries::MarkerShape shape, qreal box, qreal size,
                    const QColor& colour, bool filled)
{
    // Qt scales this image to the sample; render it well above screen size so
    // it holds up in a 300 dpi PNG and a 600 dpi PDF.
    const qreal dpr = 6.0;
    QImage img(QSize(qRound(box * dpr), qRound(box * dpr)),
               QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    img.setDevicePixelRatio(dpr);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(colour, 1.6));
    // Hollow after the first case, exactly as on the curve - and here the
    // sample line shows through the middle the same way.
    p.setBrush(filled ? QBrush(colour) : QBrush(Qt::transparent));

    const QRectF r((box - size) / 2, (box - size) / 2, size, size);
    // Points of a regular n-gon on that rect, first vertex pointing up.
    auto ngon = [&r, size](int n) {
        constexpr qreal pi = 3.14159265358979323846;
        QPolygonF poly;
        const QPointF c = r.center();
        for (int i = 0; i < n; ++i) {
            const qreal a = -pi / 2 + i * 2 * pi / n;
            poly << QPointF(c.x() + size / 2 * std::cos(a),
                            c.y() + size / 2 * std::sin(a));
        }
        return poly;
    };
    switch (shape) {
    case QScatterSeries::MarkerShapeCircle:           p.drawEllipse(r);      break;
    case QScatterSeries::MarkerShapeTriangle:         p.drawPolygon(ngon(3)); break;
    case QScatterSeries::MarkerShapeRotatedRectangle: p.drawPolygon(ngon(4)); break;
    case QScatterSeries::MarkerShapePentagon:         p.drawPolygon(ngon(5)); break;
    case QScatterSeries::MarkerShapeStar: {
        constexpr qreal pi = 3.14159265358979323846;
        QPolygonF star;
        const QPointF c = r.center();
        for (int i = 0; i < 10; ++i) {
            const qreal rad = size / 2 * ((i % 2) ? 0.45 : 1.0);
            const qreal a = -pi / 2 + i * pi / 5;
            star << QPointF(c.x() + rad * std::cos(a), c.y() + rad * std::sin(a));
        }
        p.drawPolygon(star);
        break;
    }
    default:                                          p.drawRect(r);         break;
    }
    return img;
}

QString categoryName(Cat c)
{
    switch (c) {
    case Cat::Well:          return QStringLiteral("Well");
    case Cat::Group:         return QStringLiteral("Group");
    case Cat::Field:         return QStringLiteral("Field");
    case Cat::Region:        return QStringLiteral("Region");
    case Cat::Block:         return QStringLiteral("Block");
    case Cat::Connection:    return QStringLiteral("Connection");
    case Cat::Completion:    return QStringLiteral("Completion");
    case Cat::Segment:       return QStringLiteral("Segment");
    case Cat::Aquifer:       return QStringLiteral("Aquifer");
    case Cat::Node:          return QStringLiteral("Network node");
    case Cat::Miscellaneous: return QStringLiteral("Miscellaneous");
    }
    return QStringLiteral("Other");
}

QString typeName(Type t)
{
    switch (t) {
    case Type::Rate:      return QStringLiteral("Rate");
    case Type::Total:     return QStringLiteral("Total");
    case Type::Ratio:     return QStringLiteral("Ratio");
    case Type::Pressure:  return QStringLiteral("Pressure");
    case Type::Count:     return QStringLiteral("Count");
    case Type::Mode:      return QStringLiteral("Mode");
    case Type::ProdIndex: return QStringLiteral("Prod. index");
    case Type::Undefined: return QStringLiteral("Other");
    }
    return QStringLiteral("Other");
}

// categories that carry a scope prefix letter on the keyword (WOPR -> OPR)
bool hasScopeLetter(Cat c)
{
    return c != Cat::Miscellaneous;
}

// ECLIPSE natural cell number -> 1-based I,J,K (mirrors ESmry's own
// ijk_from_global_index); falls back to the plain number without grid dims.
QString ijkLabel(int number, int nx, int ny)
{
    if (nx <= 0 || ny <= 0 || number <= 0) return QString::number(number);
    int g = number - 1;
    const int i = 1 + (g % nx);  g /= nx;
    const int j = 1 + (g % ny);
    const int k = 1 + (g / ny);
    return QStringLiteral("%1,%2,%3").arg(i).arg(j).arg(k);
}

// Split a node's item into up to two levels: `main` (well/group name, region
// number, block cell, ...) and `sub` (the cell / completion / segment within
// a well for the compound categories). Field and Miscellaneous vectors have
// no item (their number field is 0, not an identifier).
void splitItem(const Opm::EclIO::SummaryNode& n, int nx, int ny,
               QString& main, QString& sub)
{
    main.clear(); sub.clear();
    const bool haveName = !n.wgname.empty() && n.wgname != ":+:+:+:+";
    const QString name  = haveName ? QString::fromStdString(n.wgname) : QString();
    const bool haveNum  = n.number != Opm::EclIO::SummaryNode::default_number;
    const QString num   = haveNum ? QString::number(n.number) : QString();

    switch (n.category) {
    case Cat::Well: case Cat::Group: case Cat::Node:
        main = name; return;
    case Cat::Region:
        // inter-region flow vectors (R?F..., e.g. ROFR) carry a packed
        // region pair; display it as "r1-r2" like ESmry does
        if (haveNum && n.keyword.size() > 2 && n.keyword[2] == 'F') {
            const auto [r1, r2] = Opm::EclIO::splitSummaryNumber(n.number);
            main = QStringLiteral("%1-%2").arg(r1).arg(r2);
            return;
        }
        main = num; return;
    case Cat::Aquifer:
        main = num; return;
    case Cat::Block:
        if (haveNum) main = ijkLabel(n.number, nx, ny);
        return;
    case Cat::Connection:
        // per-well AND per-cell: two levels, cell shown as grid indices
        main = name;
        if (haveNum) sub = ijkLabel(n.number, nx, ny);
        if (main.isEmpty()) { main = sub; sub.clear(); }
        return;
    case Cat::Completion: case Cat::Segment:
        // per-well numbered (completion/segment id, not a cell)
        main = name;
        sub  = num;
        if (main.isEmpty()) { main = sub; sub.clear(); }
        return;
    case Cat::Field: case Cat::Miscellaneous:
    default:
        return;
    }
}

// Strict weak ordering over a mix of item styles: numeric tuples
// ("7", "12,22,7", "2-3") form one partition sorted component-wise, ahead of
// the textual partition (well names) sorted locale-aware.
void sortItems(QStringList& items)
{
    auto asTuple = [](const QString& s) -> QVector<int> {
        QVector<int> t;
        const QStringList parts = s.split(QRegularExpression(QStringLiteral("[,-]")));
        for (const QString& p : parts) {
            bool ok = false;
            const int v = p.toInt(&ok);
            if (!ok) return {};
            t.push_back(v);
        }
        return t;
    };
    std::sort(items.begin(), items.end(),
              [&asTuple](const QString& a, const QString& b) {
        const QVector<int> ta = asTuple(a), tb = asTuple(b);
        if (ta.isEmpty() != tb.isEmpty()) return !ta.isEmpty();  // tuples first
        if (!ta.isEmpty())
            return std::lexicographical_compare(ta.begin(), ta.end(),
                                                tb.begin(), tb.end());
        return a.localeAwareCompare(b) < 0;
    });
}

} // namespace

// --- friendly quantity names (body code -> name) ---------------------------
// Keyed by the scope-independent BODY code (keyword with the leading scope
// letter removed, e.g. WOPR/FOPR/GOPR -> OPR). Miscellaneous vectors are keyed
// by their full keyword. Unknown codes fall back to the raw keyword, and a
// trailing 'H' is understood as the "(History)" variant.
QString SummaryPlotWidget::friendlyName(const QString& keyword, Cat cat)
{
    static const QHash<QString, QString> table = {
        {QStringLiteral("OPR"), QStringLiteral("Oil Production Rate")},
        {QStringLiteral("WPR"), QStringLiteral("Water Production Rate")},
        {QStringLiteral("GPR"), QStringLiteral("Gas Production Rate")},
        {QStringLiteral("LPR"), QStringLiteral("Liquid Production Rate")},
        {QStringLiteral("VPR"), QStringLiteral("Reservoir Volume Production Rate")},
        {QStringLiteral("GPRF"), QStringLiteral("Free Gas Production Rate")},
        {QStringLiteral("GPRS"), QStringLiteral("Solution Gas Production Rate")},
        {QStringLiteral("OPRF"), QStringLiteral("Free Oil Production Rate")},
        {QStringLiteral("OPRS"), QStringLiteral("Solution Oil Production Rate")},
        {QStringLiteral("OPT"), QStringLiteral("Oil Production Total")},
        {QStringLiteral("WPT"), QStringLiteral("Water Production Total")},
        {QStringLiteral("GPT"), QStringLiteral("Gas Production Total")},
        {QStringLiteral("LPT"), QStringLiteral("Liquid Production Total")},
        {QStringLiteral("VPT"), QStringLiteral("Reservoir Volume Production Total")},
        {QStringLiteral("GPTF"), QStringLiteral("Free Gas Production Total")},
        {QStringLiteral("GPTS"), QStringLiteral("Solution Gas Production Total")},
        {QStringLiteral("OPTF"), QStringLiteral("Free Oil Production Total")},
        {QStringLiteral("OPTS"), QStringLiteral("Solution Oil Production Total")},
        {QStringLiteral("OIR"), QStringLiteral("Oil Injection Rate")},
        {QStringLiteral("WIR"), QStringLiteral("Water Injection Rate")},
        {QStringLiteral("GIR"), QStringLiteral("Gas Injection Rate")},
        {QStringLiteral("VIR"), QStringLiteral("Reservoir Volume Injection Rate")},
        {QStringLiteral("OIT"), QStringLiteral("Oil Injection Total")},
        {QStringLiteral("WIT"), QStringLiteral("Water Injection Total")},
        {QStringLiteral("GIT"), QStringLiteral("Gas Injection Total")},
        {QStringLiteral("VIT"), QStringLiteral("Reservoir Volume Injection Total")},
        {QStringLiteral("OPP"), QStringLiteral("Oil Production Potential")},
        {QStringLiteral("WPP"), QStringLiteral("Water Production Potential")},
        {QStringLiteral("GPP"), QStringLiteral("Gas Production Potential")},
        {QStringLiteral("LPP"), QStringLiteral("Liquid Production Potential")},
        {QStringLiteral("WCT"), QStringLiteral("Water Cut")},
        {QStringLiteral("GOR"), QStringLiteral("Gas-Oil Ratio")},
        {QStringLiteral("OGR"), QStringLiteral("Oil-Gas Ratio")},
        {QStringLiteral("WGR"), QStringLiteral("Water-Gas Ratio")},
        {QStringLiteral("GLR"), QStringLiteral("Gas-Liquid Ratio")},
        {QStringLiteral("WOR"), QStringLiteral("Water-Oil Ratio")},
        {QStringLiteral("BHP"), QStringLiteral("Bottom Hole Pressure")},
        {QStringLiteral("THP"), QStringLiteral("Tubing Head Pressure")},
        {QStringLiteral("PR"), QStringLiteral("Average Reservoir Pressure")},
        {QStringLiteral("BP"), QStringLiteral("Well Block Pressure")},
        {QStringLiteral("BP4"), QStringLiteral("Well Block Pressure (Four-Point Average)")},
        {QStringLiteral("BP5"), QStringLiteral("Well Block Pressure (Five-Point Average)")},
        {QStringLiteral("BP9"), QStringLiteral("Well Block Pressure (Nine-Point Average)")},
        {QStringLiteral("PI"), QStringLiteral("Productivity Index")},
        {QStringLiteral("PIO"), QStringLiteral("Oil Productivity Index")},
        {QStringLiteral("PIG"), QStringLiteral("Gas Productivity Index")},
        {QStringLiteral("PIW"), QStringLiteral("Water Productivity Index")},
        {QStringLiteral("PIL"), QStringLiteral("Liquid Productivity Index")},
        {QStringLiteral("PI1"), QStringLiteral("Productivity Index Based on Well Block Pressure")},
        {QStringLiteral("II"), QStringLiteral("Injectivity Index")},
        {QStringLiteral("IIO"), QStringLiteral("Oil Injectivity Index")},
        {QStringLiteral("IIW"), QStringLiteral("Water Injectivity Index")},
        {QStringLiteral("IIG"), QStringLiteral("Gas Injectivity Index")},
        {QStringLiteral("IIL"), QStringLiteral("Liquid Injectivity Index")},
        {QStringLiteral("OIP"), QStringLiteral("Oil In Place")},
        {QStringLiteral("WIP"), QStringLiteral("Water In Place")},
        {QStringLiteral("GIP"), QStringLiteral("Gas In Place")},
        {QStringLiteral("NIP"), QStringLiteral("Solvent In Place")},
        {QStringLiteral("SIP"), QStringLiteral("Salt In Place")},
        {QStringLiteral("OIPL"), QStringLiteral("Oil In Place (Liquid Phase)")},
        {QStringLiteral("OIPG"), QStringLiteral("Oil In Place (Vaporized in Gas Phase)")},
        {QStringLiteral("GIPL"), QStringLiteral("Gas In Place (Dissolved in Liquid Phase)")},
        {QStringLiteral("GIPG"), QStringLiteral("Gas In Place (Free in Gas Phase)")},
        {QStringLiteral("OE"), QStringLiteral("Oil Recovery Efficiency")},
        {QStringLiteral("OSAT"), QStringLiteral("Oil Saturation")},
        {QStringLiteral("WSAT"), QStringLiteral("Water Saturation")},
        {QStringLiteral("GSAT"), QStringLiteral("Gas Saturation")},
        {QStringLiteral("RS"), QStringLiteral("Solution Gas-Oil Ratio")},
        {QStringLiteral("RV"), QStringLiteral("Vapor Oil-Gas Ratio")},
        {QStringLiteral("PBUB"), QStringLiteral("Bubble Point Pressure")},
        {QStringLiteral("PDEW"), QStringLiteral("Dew Point Pressure")},
        {QStringLiteral("TEMP"), QStringLiteral("Temperature")},
        {QStringLiteral("MWPR"), QStringLiteral("Number of Producing Wells")},
        {QStringLiteral("MWIR"), QStringLiteral("Number of Injecting Wells")},
        {QStringLiteral("MWPT"), QStringLiteral("Total Number of Production Wells")},
        {QStringLiteral("MWIT"), QStringLiteral("Total Number of Injection Wells")},
        {QStringLiteral("MWPA"), QStringLiteral("Number of Abandoned Production Wells")},
        {QStringLiteral("MWIA"), QStringLiteral("Number of Abandoned Injection Wells")},
        {QStringLiteral("MWPP"), QStringLiteral("Number of Producers on Pressure Control")},
        {QStringLiteral("MWIP"), QStringLiteral("Number of Injectors on Pressure Control")},
        {QStringLiteral("MWPG"), QStringLiteral("Number of Producers on Group Control")},
        {QStringLiteral("MWIG"), QStringLiteral("Number of Injectors on Group Control")},
        {QStringLiteral("MWPU"), QStringLiteral("Number of Unused Production Wells")},
        {QStringLiteral("MWIU"), QStringLiteral("Number of Unused Injection Wells")},
        {QStringLiteral("STAT"), QStringLiteral("Well Status")},
        {QStringLiteral("MCTL"), QStringLiteral("Well Control Mode")},
        {QStringLiteral("AQR"), QStringLiteral("Aquifer Influx Rate")},
        {QStringLiteral("AQT"), QStringLiteral("Cumulative Aquifer Influx")},
        {QStringLiteral("AQP"), QStringLiteral("Aquifer Pressure")},
        {QStringLiteral("GSR"), QStringLiteral("Gas Sales Rate")},
        {QStringLiteral("GST"), QStringLiteral("Gas Sales Total")},
        {QStringLiteral("OFR"), QStringLiteral("Inter-Region Oil Flow Rate")},
        {QStringLiteral("WFR"), QStringLiteral("Inter-Region Water Flow Rate")},
        {QStringLiteral("GFR"), QStringLiteral("Inter-Region Gas Flow Rate")},
        {QStringLiteral("OPRH"), QStringLiteral("Oil Production Rate History")},
        {QStringLiteral("WPRH"), QStringLiteral("Water Production Rate History")},
        {QStringLiteral("GPRH"), QStringLiteral("Gas Production Rate History")},
        {QStringLiteral("LPRH"), QStringLiteral("Liquid Production Rate History")},
        {QStringLiteral("OPTH"), QStringLiteral("Oil Production Total History")},
        {QStringLiteral("WPTH"), QStringLiteral("Water Production Total History")},
        {QStringLiteral("GPTH"), QStringLiteral("Gas Production Total History")},
        {QStringLiteral("LPTH"), QStringLiteral("Liquid Production Total History")},
        {QStringLiteral("OIRH"), QStringLiteral("Oil Injection Rate History")},
        {QStringLiteral("OITH"), QStringLiteral("Oil Injection Total History")},
        {QStringLiteral("WIRH"), QStringLiteral("Water Injection Rate History")},
        {QStringLiteral("WITH"), QStringLiteral("Water Injection Total History")},
        {QStringLiteral("GIRH"), QStringLiteral("Gas Injection Rate History")},
        {QStringLiteral("GITH"), QStringLiteral("Gas Injection Total History")},
        {QStringLiteral("BHPH"), QStringLiteral("Bottom Hole Pressure History")},
        {QStringLiteral("THPH"), QStringLiteral("Tubing Head Pressure History")},
        {QStringLiteral("WCTH"), QStringLiteral("Water Cut History")},
        {QStringLiteral("GORH"), QStringLiteral("Gas-Oil Ratio History")},
        {QStringLiteral("GLRH"), QStringLiteral("Gas-Liquid Ratio History")},
        {QStringLiteral("WORH"), QStringLiteral("Water-Oil Ratio History")},
        {QStringLiteral("OGRH"), QStringLiteral("Oil-Gas Ratio History")},
        {QStringLiteral("WGRH"), QStringLiteral("Water-Gas Ratio History")},
        {QStringLiteral("TCPU"), QStringLiteral("CPU Time")},
        {QStringLiteral("TCPUTS"), QStringLiteral("CPU Time Per Timestep")},
        {QStringLiteral("TCPUDAY"), QStringLiteral("CPU Time Per Simulation Day")},
        {QStringLiteral("TIME"), QStringLiteral("Simulation Time")},
        {QStringLiteral("YEARS"), QStringLiteral("Simulation Time in Years")},
        {QStringLiteral("ELAPSED"), QStringLiteral("Elapsed Wall-Clock Time")},
        {QStringLiteral("TIMESTEP"), QStringLiteral("Timestep Length")},
        {QStringLiteral("NEWTON"), QStringLiteral("Number of Newton Iterations")},
        {QStringLiteral("NLINEARS"), QStringLiteral("Number of Linear Iterations")},
        {QStringLiteral("MLINEARS"), QStringLiteral("Average Linear Iterations Per Timestep")},
        {QStringLiteral("MSUMLINS"), QStringLiteral("Cumulative Linear Iterations")},
        {QStringLiteral("MSUMNEWT"), QStringLiteral("Cumulative Newton Iterations")},
        {QStringLiteral("STEPTYPE"), QStringLiteral("Step Type")},
    };

    QString body = keyword;
    if (hasScopeLetter(cat) && body.size() > 1)
        body = body.mid(1);

    auto it = table.constFind(body);
    if (it != table.constEnd()) return it.value();

    // history variant not listed explicitly: OPRH -> "Oil Production Rate History"
    if (body.endsWith(QLatin1Char('H')) && body.size() > 1) {
        it = table.constFind(body.left(body.size() - 1));
        if (it != table.constEnd()) return it.value() + QStringLiteral(" History");
    }
    // miscellaneous vectors keyed by full keyword
    it = table.constFind(keyword);
    if (it != table.constEnd()) return it.value();

    return QString();   // unknown -> caller shows the raw keyword
}

// ---------------------------------------------------------------------------
SummaryPlotWidget::SummaryPlotWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* top = new QVBoxLayout(this);

    // --- toolbar row ---------------------------------------------------------
    {
        auto* row = new QHBoxLayout;
        auto* bbrowse  = new QPushButton(QStringLiteral("Open SMSPEC..."));
        auto* brefresh = new QPushButton(QStringLiteral("Refresh"));
        autoRef_ = new QCheckBox(QStringLiteral("auto-refresh (10 s)"));
        dateAxis_ = new QCheckBox(QStringLiteral("date axis"));
        dateAxis_->setChecked(true);   // calendar dates by default
        markers_  = new QCheckBox(QStringLiteral("markers"));
        markers_->setToolTip(QStringLiteral("mark the data points on each curve"));
        lineWidthSpin_ = new QDoubleSpinBox;
        lineWidthSpin_->setRange(0.5, 8.0);
        lineWidthSpin_->setSingleStep(0.5);
        lineWidthSpin_->setValue(2.0);
        lineWidthSpin_->setDecimals(1);
        lineWidthSpin_->setSuffix(QStringLiteral(" pt"));
        lineWidthSpin_->setToolTip(QStringLiteral("curve line width"));
        markerSizeSpin_ = new QDoubleSpinBox;
        markerSizeSpin_->setRange(2.0, 20.0);
        markerSizeSpin_->setSingleStep(0.5);
        markerSizeSpin_->setValue(7.5);
        markerSizeSpin_->setDecimals(1);
        markerSizeSpin_->setSuffix(QStringLiteral(" px"));
        markerSizeSpin_->setToolTip(QStringLiteral("data-point marker size (when markers are on)"));
        markerEverySpin_ = new QSpinBox;
        markerEverySpin_->setRange(1, 1000);
        markerEverySpin_->setValue(1);
        markerEverySpin_->setPrefix(QStringLiteral("every "));
        markerEverySpin_->setToolTip(QStringLiteral(
            "mark every n-th data point; 1 (the default) marks them all, so "
            "the markers are exactly the samples in the summary file"));
        // The legend is the one part that does not have to follow the curves:
        // a figure for a paper often wants it a size up (or out of the way).
        legendScaleSpin_ = new QDoubleSpinBox;
        legendScaleSpin_->setRange(0.4, 3.0);
        legendScaleSpin_->setSingleStep(0.1);
        legendScaleSpin_->setValue(1.0);
        legendScaleSpin_->setDecimals(1);
        legendScaleSpin_->setPrefix(QStringLiteral("x"));
        legendScaleSpin_->setToolTip(QStringLiteral(
            "legend size, as a factor of the normal one"));
        // Curves drawn for a full-window chart are far too heavy once the same
        // widths land in a quarter-sized subplot, and a legend sized for one
        // chart swallows four. Everything that is drawn ON the plot follows
        // the plot's own size unless this is turned off.
        autoScale_ = new QCheckBox(QStringLiteral("scale with plot"));
        autoScale_->setChecked(true);
        autoScale_->setToolTip(QStringLiteral(
            "line width, marker size and legend size follow the size of the "
            "chart they are drawn in, so a 2x2 layout is not four heavy plots; "
            "off pins them to exactly the values set here"));
        // A zoom belongs to one subplot, so undoing it should be able to as
        // well: the button clears them all (what it always did), the arrow
        // beside it offers the focused one on its own.
        auto* bzoom = new QToolButton;
        bzoom->setText(QStringLiteral("Reset zoom"));
        bzoom->setPopupMode(QToolButton::MenuButtonPopup);
        bzoom->setToolTip(QStringLiteral(
            "back to the full range - every subplot; the arrow resets only the "
            "focused one"));
        auto* zoomMenu = new QMenu(bzoom);
        zoomMenu->addAction(QStringLiteral("All subplots"), this,
                            [this] { resetZoom(false); });
        zoomMenu->addAction(QStringLiteral("This subplot only"), this,
                            [this] { resetZoom(true); });
        bzoom->setMenu(zoomMenu);
        auto* bpng  = new QPushButton(QStringLiteral("Save figure..."));
        auto* bcsv  = new QPushButton(QStringLiteral("Save CSV..."));
        bcsv->setToolTip(QStringLiteral("export the plotted vectors of every checked case"));
        layoutBtn_ = new QToolButton;
        layoutBtn_->setPopupMode(QToolButton::InstantPopup);
        layoutBtn_->setToolTip(QStringLiteral(
            "the subplot grid - hover the cells to pick rows x columns; click a "
            "subplot to focus it, and the vector tree then selects what that "
            "subplot shows"));
        {
            auto* menu = new QMenu(layoutBtn_);
            auto* act = new QWidgetAction(menu);
            act->setDefaultWidget(new LayoutPicker(
                kMaxRows, kMaxCols, [this, menu](int r, int c) {
                    menu->hide();
                    setLayoutGrid(r, c);
                }, menu));
            menu->addAction(act);
            layoutBtn_->setMenu(menu);
        }
        row->addWidget(bbrowse);
        row->addWidget(brefresh);
        row->addWidget(autoRef_);
        row->addWidget(dateAxis_);
        row->addWidget(markers_);
        row->addWidget(new QLabel(QStringLiteral("Line:")));
        row->addWidget(lineWidthSpin_);
        row->addWidget(new QLabel(QStringLiteral("Marker:")));
        row->addWidget(markerSizeSpin_);
        row->addWidget(markerEverySpin_);
        row->addWidget(new QLabel(QStringLiteral("Legend:")));
        row->addWidget(legendScaleSpin_);
        row->addWidget(autoScale_);
        row->addStretch(1);
        // Legend placement: docked to an edge, or floating in a corner of the
        // plot area (the usual choice for a figure in a paper).
        legendBox_ = new QComboBox;
        legendBox_->addItem(QStringLiteral("Legend: bottom"),       LegendBottom);
        legendBox_->addItem(QStringLiteral("Legend: top"),          LegendTop);
        legendBox_->addItem(QStringLiteral("Legend: left"),         LegendLeft);
        legendBox_->addItem(QStringLiteral("Legend: right"),        LegendRight);
        legendBox_->addItem(QStringLiteral("Legend: inside top-left"),     LegendInTL);
        legendBox_->addItem(QStringLiteral("Legend: inside top-right"),    LegendInTR);
        legendBox_->addItem(QStringLiteral("Legend: inside bottom-left"),  LegendInBL);
        legendBox_->addItem(QStringLiteral("Legend: inside bottom-right"), LegendInBR);
        legendBox_->addItem(QStringLiteral("Legend: hidden"),        LegendOff);
        legendBox_->setToolTip(QStringLiteral(
            "where the legend sits; the inside positions float it over the plot "
            "area on a translucent background, and a floating legend can be "
            "dragged anywhere with the mouse"));
        row->addWidget(legendBox_);
        connect(legendBox_, &QComboBox::currentIndexChanged, this, [this](int) {
            legendPos_.fill(QPointF());     // a placement choice overrides a drag
            for (auto* c : std::as_const(charts_)) placeLegend(c);
        });
        row->addWidget(new QLabel(QStringLiteral("Layout:")));
        row->addWidget(layoutBtn_);
        row->addWidget(bzoom);
        row->addWidget(bpng);
        row->addWidget(bcsv);
        top->addLayout(row);


        connect(bbrowse,  &QPushButton::clicked, this, [this] { browseCase(); });
        connect(brefresh, &QPushButton::clicked, this, [this] { reload(true); });
        connect(dateAxis_, &QCheckBox::toggled, this, [this](bool) { replot(); });
        connect(markers_,  &QCheckBox::toggled, this, [this](bool) { replot(); });
        connect(lineWidthSpin_, &QDoubleSpinBox::valueChanged, this, [this](double) { replot(); });
        connect(markerSizeSpin_, &QDoubleSpinBox::valueChanged, this, [this](double) { replot(); });
        connect(markerEverySpin_, &QSpinBox::valueChanged, this, [this](int) { replot(); });
        connect(legendScaleSpin_, &QDoubleSpinBox::valueChanged, this, [this](double) { replot(); });
        connect(autoScale_, &QCheckBox::toggled, this, [this](bool) { replot(); });
        connect(bzoom, &QToolButton::clicked, this, [this] { resetZoom(false); });
        connect(bpng,  &QPushButton::clicked, this, [this] { savePng(); });
        connect(bcsv,  &QPushButton::clicked, this, [this] { saveCsv(); });
        timer_ = new QTimer(this);
        timer_->setInterval(10000);
        connect(timer_, &QTimer::timeout, this, [this] { reload(true); });
        connect(autoRef_, &QCheckBox::toggled, this, [this](bool on) {
            if (on) timer_->start(); else timer_->stop();
        });
    }

    // --- filter row ----------------------------------------------------------
    {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("Category:")));
        catBox_ = new QComboBox;  row->addWidget(catBox_);
        row->addWidget(new QLabel(QStringLiteral("Type:")));
        typeBox_ = new QComboBox; row->addWidget(typeBox_);
        row->addWidget(new QLabel(QStringLiteral("Item:")));
        itemBox_ = new QComboBox; itemBox_->setMinimumWidth(120); row->addWidget(itemBox_);
        subLabel_ = new QLabel(QStringLiteral("Cell:"));
        subItemBox_ = new QComboBox; subItemBox_->setMinimumWidth(110);
        subLabel_->hide(); subItemBox_->hide();
        row->addWidget(subLabel_); row->addWidget(subItemBox_);
        filter_ = new QLineEdit;
        filter_->setPlaceholderText(QStringLiteral(
            "search or wildcard filter, e.g.  WBHP:B*, WOPR*  (comma = or)"));
        filter_->setClearButtonEnabled(true);
        filter_->setToolTip(QStringLiteral(
            "plain text matches anywhere (keyword, item or quantity name);\n"
            "with * or ? the comma-separated patterns match the KEYWORD:ITEM "
            "key, e.g. WBHP:B*, WOPR*"));
        row->addWidget(filter_, 1);
        top->addLayout(row);

        // Cascade: category -> item -> cell; each level repopulates the next.
        connect(catBox_,  &QComboBox::currentIndexChanged, this,
                [this](int) { populateItemBox(); rebuildTree({}); });
        connect(typeBox_, &QComboBox::currentIndexChanged, this, [this](int) { rebuildTree({}); });
        connect(itemBox_, &QComboBox::currentIndexChanged, this,
                [this](int) { populateSubItemBox(); rebuildTree({}); });
        connect(subItemBox_, &QComboBox::currentIndexChanged, this, [this](int) { rebuildTree({}); });
        connect(filter_,  &QLineEdit::textChanged,         this, [this] { rebuildTree({}); });
    }

    // --- cases + tree + chart ------------------------------------------------
    auto* split = mainSplit_ = new QSplitter;
    {
        auto* left = new QWidget;
        auto* ll = new QVBoxLayout(left);
        ll->setContentsMargins(0, 0, 0, 0);

        // Case manager: checked cases are plotted; the highlighted row is the
        // ACTIVE case whose vectors fill the tree below.
        auto* crow = new QHBoxLayout;
        crow->addWidget(new QLabel(QStringLiteral("Cases  (checked = plotted):")), 1);
        // The list order is the plot order - of the curves, their colours and
        // dashes, the legend and the CSV columns - so it is worth being able
        // to set it: sort it, or move one case at a time.
        auto* bsort = new QToolButton;
        bsort->setText(QStringLiteral("Sort"));
        bsort->setPopupMode(QToolButton::InstantPopup);
        bsort->setToolTip(QStringLiteral("order the case list - which is the "
                                         "order the curves are drawn in"));
        auto* sortMenu = new QMenu(bsort);
        sortMenu->addAction(QStringLiteral("By name (A-Z)"), this,
                            [this] { sortCases(SortNameAsc); });
        sortMenu->addAction(QStringLiteral("By name (Z-A)"), this,
                            [this] { sortCases(SortNameDesc); });
        sortMenu->addAction(QStringLiteral("Checked first"), this,
                            [this] { sortCases(SortCheckedFirst); });
        sortMenu->addAction(QStringLiteral("Order loaded"), this,
                            [this] { sortCases(SortLoadOrder); });
        bsort->setMenu(sortMenu);
        crow->addWidget(bsort);
        auto* bup = new QToolButton;
        bup->setArrowType(Qt::UpArrow);
        bup->setToolTip(QStringLiteral("move the highlighted case up (Ctrl+Up)"));
        crow->addWidget(bup);
        auto* bdown = new QToolButton;
        bdown->setArrowType(Qt::DownArrow);
        bdown->setToolTip(QStringLiteral("move the highlighted case down (Ctrl+Down)"));
        crow->addWidget(bdown);
        auto* brename = new QPushButton(QStringLiteral("Rename"));
        brename->setToolTip(QStringLiteral(
            "rename the highlighted case - the new name is what the plot "
            "legend shows (double-click the case, or F2)"));
        crow->addWidget(brename);
        auto* bremove = new QPushButton(QStringLiteral("Remove"));
        bremove->setToolTip(QStringLiteral("remove the highlighted case from the list"));
        crow->addWidget(bremove);
        ll->addLayout(crow);

        caseList_ = new QListWidget;
        caseList_->setMinimumHeight(64);
        caseList_->setSelectionMode(QAbstractItemView::SingleSelection);
        // Drag a case to where you want it in the plot; the buttons above do
        // the same for anyone who would rather click.
        caseList_->setDragDropMode(QAbstractItemView::InternalMove);
        caseList_->setDefaultDropAction(Qt::MoveAction);
        caseList_->viewport()->installEventFilter(this);   // to catch the drop
        // double-click / F2 edits the name in place
        caseList_->setEditTriggers(QAbstractItemView::DoubleClicked
                                   | QAbstractItemView::EditKeyPressed);

        tree_ = new QTreeWidget;
        tree_->setHeaderHidden(true);
        tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        tree_->setUniformRowHeights(true);

        // The case list used to be pinned to 110 px however many cases were
        // loaded; on a splitter it can be dragged open to show them all.
        caseSplit_ = new QSplitter(Qt::Vertical);
        caseSplit_->addWidget(caseList_);
        caseSplit_->addWidget(tree_);
        caseSplit_->setStretchFactor(0, 0);
        caseSplit_->setStretchFactor(1, 1);
        caseSplit_->setSizes({ 110, 500 });
        ll->addWidget(caseSplit_, 1);
        split->addWidget(left);

        connect(bup,   &QToolButton::clicked, this, [this] { moveCase(-1); });
        connect(bdown, &QToolButton::clicked, this, [this] { moveCase(1); });
        auto* upKey = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Up), caseList_);
        upKey->setContext(Qt::WidgetShortcut);
        connect(upKey, &QShortcut::activated, this, [this] { moveCase(-1); });
        auto* downKey = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Down), caseList_);
        downKey->setContext(Qt::WidgetShortcut);
        connect(downKey, &QShortcut::activated, this, [this] { moveCase(1); });

        connect(bremove, &QPushButton::clicked, this, [this] { removeCurrentCase(); });
        connect(brename, &QPushButton::clicked, this, [this] {
            if (auto* it = caseList_->currentItem()) caseList_->editItem(it);
            else setStatus(QStringLiteral("highlight a case in the list first"));
        });
        connect(caseList_, &QListWidget::currentItemChanged, this,
                [this](QListWidgetItem*, QListWidgetItem*) { reload(false); });
        // itemChanged covers both the check boxes and an in-place rename.
        connect(caseList_, &QListWidget::itemChanged, this,
                [this](QListWidgetItem* it) { caseItemChanged(it); });

        // A pool of charts in a grid; applyChartLayout() shows the first
        // rows x cols of them, and the pool grows to whatever a layout asks
        // for (see ensureCharts). Each chart keeps its own vector selection
        // (chartSel_); a click focuses a subplot and the tree then edits that
        // one, and Ctrl+drag swaps one subplot with another.
        resizeTimer_ = new QTimer(this);
        resizeTimer_->setSingleShot(true);
        resizeTimer_->setInterval(180);
        connect(resizeTimer_, &QTimer::timeout, this, [this] { replot(); });

        chartArea_ = new QWidget;
        chartArea_->installEventFilter(this);
        chartGrid_ = new QGridLayout(chartArea_);
        chartGrid_->setContentsMargins(0, 0, 0, 0);
        chartGrid_->setSpacing(2);
        ensureCharts(1);
        chartGrid_->addWidget(chartViews_[0], 0, 0);
        split->addWidget(chartArea_);
        split->setStretchFactor(0, 0);
        split->setStretchFactor(1, 1);
        split->setSizes({ 320, 680 });
        top->addWidget(split, 1);

        connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
            if (syncingTree_) return;      // programmatic reselect, not the user
            QStringList keys;
            for (auto* it : tree_->selectedItems()) {
                const QVariant k = it->data(0, RoleVecIndex);
                if (k.isValid()) keys << k.toString();
            }
            if (focusChart_ >= 0 && focusChart_ < chartSel_.size())
                chartSel_[focusChart_] = keys;
            replot();
        });
    }

    setLayoutGrid(1, 1);        // labels the button, lays the single chart out

    status_ = new QLabel;
    top->addWidget(status_);
    setStatus(QStringLiteral("run a job (or open an SMSPEC) to plot summary vectors"));
}

SummaryPlotWidget::~SummaryPlotWidget() = default;

void SummaryPlotWidget::setStatus(const QString& s) { status_->setText(s); }

void SummaryPlotWidget::savePng()
{
    QString suggested = activeLabel();
    if (suggested.isEmpty()) suggested = QStringLiteral("summary");
    // PDF keeps the curves and the text as vectors, which is what a figure in
    // a paper wants; the PNG is rendered at 3x and tagged 300 dpi rather than
    // grabbed off the screen, so it survives being printed.
    const QString pdfFilter = QStringLiteral("PDF, vector - for manuscripts (*.pdf)");
    const QString pngFilter = QStringLiteral("PNG, 300 dpi (*.png)");
    const QString pngScreen = QStringLiteral("PNG, screen resolution (*.png)");
    QString chosen = pdfFilter;
    const QString f = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save chart"),
        QDir(flowgui::startDir(QStringLiteral("figure")))
            .filePath(suggested + QStringLiteral(".pdf")),
        pdfFilter + QStringLiteral(";;") + pngFilter + QStringLiteral(";;") + pngScreen,
        &chosen);
    if (f.isEmpty()) return;
    flowgui::rememberDir(QStringLiteral("figure"), f);

    const bool wantPdf = chosen == pdfFilter
        || (chosen != pngFilter && chosen != pngScreen
            && f.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive));
    const QSize ws = chartArea_->size();
    if (ws.isEmpty()) { setStatus(QStringLiteral("nothing to save")); return; }
    bool ok = false;

    if (wantPdf) {
        // Page sized to the figure itself (7 inches wide), no margins, so it
        // drops straight into \includegraphics without cropping.
        const double aspect = double(ws.height()) / double(ws.width());
        QPdfWriter pdf(f);
        pdf.setResolution(600);
        pdf.setPageSize(QPageSize(QSizeF(7.0, 7.0 * aspect), QPageSize::Inch,
                                  QStringLiteral("figure"),
                                  QPageSize::ExactMatch));
        pdf.setPageMargins(QMarginsF(0, 0, 0, 0));
        QPainter p;
        if (p.begin(&pdf)) {
            p.setRenderHint(QPainter::Antialiasing);
            const double sc = std::min(double(pdf.width())  / ws.width(),
                                       double(pdf.height()) / ws.height());
            p.scale(sc, sc);
            chartArea_->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
            p.end();
            ok = true;
        }
    } else {
        const int scale = (chosen == pngScreen) ? 1 : 3;
        QImage img(ws * scale, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::white);
        // 300 dpi in dots per metre, so Word/LaTeX place it at the right size
        const int dpm = int(std::lround(300.0 / 0.0254)) * (scale == 1 ? 0 : 1);
        if (dpm > 0) { img.setDotsPerMeterX(dpm); img.setDotsPerMeterY(dpm); }
        QPainter p;
        if (p.begin(&img)) {
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(scale, scale);
            chartArea_->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
            p.end();
            ok = img.save(f);
        }
    }
    setStatus(ok ? QStringLiteral("chart saved to %1").arg(QDir::toNativeSeparators(f))
                 : QStringLiteral("could not save %1").arg(QDir::toNativeSeparators(f)));
}

QString SummaryPlotWidget::activePath() const
{
    auto* it = caseList_->currentItem();
    return it ? it->data(Qt::UserRole).toString() : QString();
}

QString SummaryPlotWidget::activeLabel() const
{
    auto* it = caseList_->currentItem();
    return it ? it->text() : QString();
}

void SummaryPlotWidget::addCase(const QString& label, const QString& rawPath,
                                bool checked)
{
    // One spelling per case: the same run arrives written several ways (see
    // CasePath.h), and comparing raw strings would register it twice.
    const QString smspecPath = flowgui::normalizeCasePath(rawPath);
    for (int i = 0; i < caseList_->count(); ++i)
        if (flowgui::sameCasePath(caseList_->item(i)->data(Qt::UserRole).toString(),
                                  smspecPath)) return;

    auto* it = new QListWidgetItem(label);
    it->setData(Qt::UserRole, smspecPath);
    it->setData(RoleCaseSeq, caseSeq_++);   // to sort back into load order
    it->setData(RoleCaseBase, label);    // the case's own name, before tagging
    it->setData(RoleCaseLabel, label);   // to tell a rename from a check toggle
    it->setToolTip(smspecPath);
    it->setFlags(it->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
    it->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    caseList_->blockSignals(true);       // no premature replot from itemChanged
    caseList_->addItem(it);
    caseList_->blockSignals(false);
    relabelCases();                      // tag this one and its twins, if any
    if (caseList_->count() == 1) caseList_->setCurrentItem(it);
    emit caseAdded(it->text(), smspecPath);
}

void SummaryPlotWidget::setCaseLabel(const QString& smspecPath, const QString& label)
{
    if (label.isEmpty()) return;
    for (int i = 0; i < caseList_->count(); ++i) {
        auto* it = caseList_->item(i);
        if (!flowgui::sameCasePath(it->data(Qt::UserRole).toString(), smspecPath)) continue;
        it->setData(RoleCaseBase, label);
        it->setData(RoleCaseCustom, true);
        relabelCases();                  // may free a tag on the cases it left
        return;
    }
}

// Name every case by what actually tells it apart: its own name while that is
// unique, otherwise the name plus the part of its path that separates it from
// the cases sharing that name.
//
// This runs over the WHOLE list after every add, remove or rename rather than
// tagging each newcomer as it arrives: a case that shows up once its twin has
// already been tagged collides with nothing, and incremental tagging left it
// bare - the one case in the list saying nothing about which run it is.
void SummaryPlotWidget::relabelCases()
{
    const int n = caseList_->count();
    if (n == 0) return;

    // The directory components of each case, deepest last.
    auto dirParts = [this](int row) {
        const QString path = caseList_->item(row)->data(Qt::UserRole).toString();
        QStringList parts = QFileInfo(path).absolutePath()
                                .split(QLatin1Char('/'), Qt::SkipEmptyParts);
        return parts;
    };

    QVector<QString> text(n);
    QHash<QString, QList<int>> byName;
    for (int i = 0; i < n; ++i) {
        auto* it = caseList_->item(i);
        const QString base = it->data(RoleCaseBase).toString();
        text[i] = base;                       // plain name unless ambiguous
        // A name the user typed is theirs: it is never tagged, and it does
        // not drag another case into a tag either.
        if (!it->data(RoleCaseCustom).toBool()) byName[base].append(i);
    }

    for (auto g = byName.cbegin(); g != byName.cend(); ++g) {
        const QList<int>& rows = g.value();
        if (rows.size() < 2) continue;        // nothing to tell apart

        QVector<QStringList> parts;
        for (int r : rows) parts.append(dirParts(r));

        // Trailing components shared by every one of them carry no
        // information - the default output directory is "<deck>_run" for all
        // of them, and saying so three times says nothing. Drop those first,
        // keeping at least one component each.
        int common = 0;
        for (;;) {
            bool same = true;
            for (const QStringList& p : parts) {
                if (p.size() - common < 2) { same = false; break; }
                if (p[p.size() - 1 - common] != parts[0][parts[0].size() - 1 - common])
                    { same = false; break; }
            }
            if (!same) break;
            ++common;
        }

        // Then the shallowest tail that is different for every case; every
        // member gets the same depth, so the tags read as alternatives.
        auto tailOf = [&](const QStringList& p, int depth) {
            const int end = p.size() - common;
            return QStringList(p.mid(std::max(0, end - depth), std::min(depth, end)))
                       .join(QLatin1Char('/'));
        };
        int depth = 1;
        const int maxDepth = 4;
        for (; depth < maxDepth; ++depth) {
            QSet<QString> seen;
            bool unique = true;
            for (const QStringList& p : parts) {
                const QString t = tailOf(p, depth);
                if (t.isEmpty() || seen.contains(t)) { unique = false; break; }
                seen.insert(t);
            }
            if (unique) break;
        }
        for (int k = 0; k < rows.size(); ++k) {
            const QString tag = tailOf(parts[k], depth);
            if (!tag.isEmpty())
                text[rows[k]] = g.key() + QStringLiteral(" [") + tag + QLatin1Char(']');
        }
    }

    // Last resort - two cases that still read the same (paths too alike, or a
    // typed name that happens to clash) get a counter, so the legend can
    // never show one name for two curves.
    QSet<QString> used;
    for (int i = 0; i < n; ++i) {
        QString t = text[i];
        for (int k = 2; used.contains(t); ++k)
            t = text[i] + QStringLiteral(" (%1)").arg(k);
        used.insert(t);
        text[i] = t;
    }

    for (int i = 0; i < n; ++i) {
        auto* it = caseList_->item(i);
        if (it->text() == text[i]) continue;
        caseList_->blockSignals(true);        // not a rename by the user
        it->setText(text[i]);
        caseList_->blockSignals(false);
        it->setData(RoleCaseLabel, text[i]);
        emit caseRenamed(it->data(Qt::UserRole).toString(), text[i]);
    }
}

void SummaryPlotWidget::caseItemChanged(QListWidgetItem* it)
{
    if (!it) return;
    const QString prev = it->data(RoleCaseLabel).toString();
    QString now = it->text().trimmed();
    if (now == prev) { replot(); return; }        // a check toggle, not a rename

    if (now.isEmpty()) now = prev;                // refuse to blank a name
    // Keep names unique: the legend shows "case | vector", so duplicates
    // would be impossible to tell apart.
    QString unique = now;
    for (int n = 2; ; ++n) {
        bool taken = false;
        for (int i = 0; i < caseList_->count(); ++i)
            if (caseList_->item(i) != it && caseList_->item(i)->text() == unique) {
                taken = true;
                break;
            }
        if (!taken) break;
        unique = now + QStringLiteral(" (%1)").arg(n);
    }
    if (unique != it->text()) {
        caseList_->blockSignals(true);            // no recursion via itemChanged
        it->setText(unique);
        caseList_->blockSignals(false);
    }
    it->setData(RoleCaseLabel, unique);
    // The name is the user's from here on: it is never tagged, and the cases
    // it used to be confused with may no longer need their own tag.
    it->setData(RoleCaseBase, unique);
    it->setData(RoleCaseCustom, true);
    emit caseRenamed(it->data(Qt::UserRole).toString(), unique);
    relabelCases();
    setStatus(QStringLiteral("case renamed to \"%1\"").arg(unique));
    replot();                                     // legend picks up the new name
}

QList<SummaryPlotWidget::CaseInfo> SummaryPlotWidget::caseInfos() const
{
    QList<CaseInfo> out;
    for (int i = 0; i < caseList_->count(); ++i) {
        const auto* it = caseList_->item(i);
        out.push_back({ it->text(), it->data(Qt::UserRole).toString(),
                        it->checkState() == Qt::Checked });
    }
    return out;
}

// ---------------------------------------------------------------------------
// Session state. Paths go out with forward slashes so a project file written
// on one platform still opens on the other.
QJsonObject SummaryPlotWidget::uiState() const
{
    QJsonObject o;

    QJsonArray cases;
    for (int i = 0; i < caseList_->count(); ++i) {
        const auto* it = caseList_->item(i);
        QJsonObject e;
        e[QStringLiteral("label")]   = it->text();     // as shown, tag and all
        // ... and the name behind it, so restoring re-derives the tags from
        // the list that is actually there rather than freezing today's.
        e[QStringLiteral("base")]    = it->data(RoleCaseBase).toString();
        if (it->data(RoleCaseCustom).toBool()) e[QStringLiteral("custom")] = true;
        e[QStringLiteral("path")]    =
            QDir::fromNativeSeparators(it->data(Qt::UserRole).toString());
        e[QStringLiteral("checked")] = it->checkState() == Qt::Checked;
        cases.append(e);
    }
    o[QStringLiteral("cases")]  = cases;
    o[QStringLiteral("active")] = QDir::fromNativeSeparators(activePath());

    // What each subplot shows, subplot by subplot (not only the focused one).
    // Only as many subplots as are in use, or hold a selection: sixteen
    // arrays, twelve of them empty, is noise in a project file.
    int used = visibleCharts_;
    for (int i = 0; i < chartSel_.size(); ++i)
        if (!chartSel_[i].isEmpty()) used = std::max(used, i + 1);
    QJsonArray sel;
    for (int i = 0; i < used && i < chartSel_.size(); ++i) {
        QJsonArray a;
        for (const QString& k : chartSel_[i]) a.append(k);
        sel.append(a);
    }
    o[QStringLiteral("selections")] = sel;
    o[QStringLiteral("layoutRows")] = layoutRows_;
    o[QStringLiteral("layoutCols")] = layoutCols_;
    // ... and the subplot COUNT as well, which is all a 0.7.0 state carried:
    // one written here still opens in a build that only knows 1 / 2 / 4.
    o[QStringLiteral("layout")] = visibleCharts_;
    o[QStringLiteral("focus")]  = focusChart_;

    // A dragged legend, as the fraction of the chart rect it was left at.
    QJsonArray lpos;
    for (const QPointF& p : legendPos_) {
        QJsonArray xy;                       // empty = never dragged
        if (!p.isNull()) { xy.append(p.x()); xy.append(p.y()); }
        lpos.append(xy);
    }
    o[QStringLiteral("legendPos")] = lpos;
    o[QStringLiteral("legend")]    = legendBox_ ? legendBox_->currentIndex() : 0;

    o[QStringLiteral("dateAxis")]    = dateAxis_ && dateAxis_->isChecked();
    o[QStringLiteral("markers")]     = markers_  && markers_->isChecked();
    o[QStringLiteral("autoRefresh")] = autoRef_  && autoRef_->isChecked();
    if (autoScale_)       o[QStringLiteral("autoScale")]   = autoScale_->isChecked();
    if (legendScaleSpin_) o[QStringLiteral("legendScale")] = legendScaleSpin_->value();
    if (lineWidthSpin_)   o[QStringLiteral("lineWidth")]   = lineWidthSpin_->value();
    if (markerSizeSpin_)  o[QStringLiteral("markerSize")]  = markerSizeSpin_->value();
    if (markerEverySpin_) o[QStringLiteral("markerEvery")] = markerEverySpin_->value();

    // Filters by TEXT: the boxes list only what the loaded case actually has,
    // so an index would mean something else next time.
    if (catBox_)     o[QStringLiteral("category")] = catBox_->currentText();
    if (typeBox_)    o[QStringLiteral("type")]     = typeBox_->currentText();
    if (itemBox_)    o[QStringLiteral("item")]     = itemBox_->currentText();
    if (subItemBox_) o[QStringLiteral("subItem")]  = subItemBox_->currentText();
    if (filter_)     o[QStringLiteral("filter")]   = filter_->text();

    // How the panel is split - the case list can be dragged open to show more
    // cases, and that is worth keeping. Qt's own splitter state rather than a
    // list of pixel heights: it holds the PROPORTIONS, so a session restored
    // into a window of another size still looks like the one that was saved.
    auto stateOf = [](QSplitter* sp) {
        return sp ? QString::fromLatin1(sp->saveState().toBase64()) : QString();
    };
    o[QStringLiteral("caseSplit")] = stateOf(caseSplit_);
    o[QStringLiteral("mainSplit")] = stateOf(mainSplit_);
    return o;
}

void SummaryPlotWidget::restoreUiState(const QJsonObject& state)
{
    if (state.isEmpty()) return;
    auto has = [&state](const char* k) { return state.contains(QLatin1String(k)); };
    auto val = [&state](const char* k) { return state.value(QLatin1String(k)); };

    // Drawing options first: they are cheap, and everything restored below is
    // then plotted with them already in force instead of being replotted.
    if (dateAxis_ && has("dateAxis")) dateAxis_->setChecked(val("dateAxis").toBool(true));
    if (markers_  && has("markers"))  markers_->setChecked(val("markers").toBool(false));
    if (autoScale_       && has("autoScale"))   autoScale_->setChecked(val("autoScale").toBool(true));
    if (legendScaleSpin_ && has("legendScale")) legendScaleSpin_->setValue(val("legendScale").toDouble(1.0));
    if (lineWidthSpin_   && has("lineWidth"))   lineWidthSpin_->setValue(val("lineWidth").toDouble(2.0));
    if (markerSizeSpin_  && has("markerSize"))  markerSizeSpin_->setValue(val("markerSize").toDouble(7.5));
    if (markerEverySpin_ && has("markerEvery")) markerEverySpin_->setValue(val("markerEvery").toInt(1));
    if (legendBox_ && has("legend")) {
        const int i = val("legend").toInt(0);
        if (i >= 0 && i < legendBox_->count()) legendBox_->setCurrentIndex(i);
    }

    // Cases: skip the ones whose files are gone, and make the same one active
    // (that is the case whose vectors fill the tree).
    if (has("cases")) {
        clearCases();
        const QJsonArray cases = val("cases").toArray();
        for (const auto& v : cases) {
            const QJsonObject e = v.toObject();
            const QString p =
                QDir::toNativeSeparators(e.value(QStringLiteral("path")).toString());
            if (p.isEmpty() || !QFileInfo::exists(p)) continue;
            const QString label = e.value(QStringLiteral("label")).toString();
            const bool custom = e.value(QStringLiteral("custom")).toBool(false);
            // Older states stored only the shown name; the tag in it is ours
            // to re-derive, so take it off again (only when it really is one
            // - a directory on the case's own path).
            QString base = e.value(QStringLiteral("base")).toString();
            if (base.isEmpty()) base = untaggedLabel(label, p);
            addCase(base, p, e.value(QStringLiteral("checked")).toBool(true));
            if (custom) setCaseLabel(p, label);
        }
        const QString active = QDir::toNativeSeparators(val("active").toString());
        if (!active.isEmpty()) activateCase(active);
    }

    // Filters after the case: the boxes are (re)filled from its vectors, and
    // the cascade repopulates each level as the one above it is set.
    auto pick = [](QComboBox* box, const QString& text) {
        if (!box || text.isEmpty()) return;
        const int i = box->findText(text);
        if (i >= 0) box->setCurrentIndex(i);
    };
    pick(catBox_,  val("category").toString());
    pick(typeBox_, val("type").toString());
    pick(itemBox_, val("item").toString());
    pick(subItemBox_, val("subItem").toString());
    if (filter_ && has("filter")) filter_->setText(val("filter").toString());

    // Layout BEFORE the per-subplot selections: shrinking the layout shuffles
    // them (it keeps the focused subplot), which would scramble what is being
    // restored here.
    {
        // A state from before the grid was rows x columns has only the count.
        int rows = val("layoutRows").toInt(0), cols = val("layoutCols").toInt(0);
        if (rows < 1 || cols < 1) {
            switch (val("layout").toInt(1)) {
            case 2:  rows = 2; cols = 1; break;
            case 4:  rows = 2; cols = 2; break;
            default: rows = 1; cols = 1; break;
            }
        }
        setLayoutGrid(rows, cols);
    }
    if (has("selections")) {
        const QJsonArray sel = val("selections").toArray();
        for (int i = 0; i < sel.size() && i < chartSel_.size(); ++i) {
            QStringList keys;
            const QJsonArray a = sel[i].toArray();
            for (const auto& k : a) keys << k.toString();
            chartSel_[i] = keys;
        }
    }
    if (has("legendPos")) {
        const QJsonArray lpos = val("legendPos").toArray();
        for (int i = 0; i < lpos.size() && i < legendPos_.size(); ++i) {
            const QJsonArray xy = lpos[i].toArray();
            legendPos_[i] = xy.size() == 2 ? QPointF(xy[0].toDouble(), xy[1].toDouble())
                                           : QPointF();
        }
    }
    auto restoreSplit = [](QSplitter* sp, const QString& state) {
        if (sp && !state.isEmpty())
            sp->restoreState(QByteArray::fromBase64(state.toLatin1()));
    };
    restoreSplit(caseSplit_, val("caseSplit").toString());
    restoreSplit(mainSplit_, val("mainSplit").toString());

    setFocusChart(std::clamp(val("focus").toInt(0), 0, visibleCharts_ - 1));
    replot();

    // Last: a running refresh timer would otherwise reload while the state is
    // still being put back.
    if (autoRef_ && has("autoRefresh")) autoRef_->setChecked(val("autoRefresh").toBool(false));
}

void SummaryPlotWidget::clearCases()
{
    caseList_->blockSignals(true);
    caseList_->clear();
    caseList_->blockSignals(false);
    clearActiveCase();
}

void SummaryPlotWidget::activateCase(const QString& smspecPath)
{
    for (int i = 0; i < caseList_->count(); ++i)
        if (flowgui::sameCasePath(caseList_->item(i)->data(Qt::UserRole).toString(),
                                  smspecPath)) {
            caseList_->setCurrentItem(caseList_->item(i));  // triggers reload
            return;
        }
}

void SummaryPlotWidget::caseFinished(const QString& rawPath)
{
    // The job's output path is spelled its own way; match and key the reader
    // map by the one spelling the list stores.
    const QString smspecPath = flowgui::normalizeCasePath(rawPath);
    if (flowgui::sameCasePath(activePath(), smspecPath)) { reload(true); return; }
    others_.erase(smspecPath);   // drop a possibly stale comparison reader
    replot();
}

void SummaryPlotWidget::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    if (!smry_ && QFileInfo::exists(activePath())) reload(false);
}

void SummaryPlotWidget::removeCurrentCase()
{
    const int row = caseList_->currentRow();
    if (row < 0) return;
    QListWidgetItem* it = caseList_->takeItem(row);   // fires currentItemChanged
    if (it) {
        const QString path = it->data(Qt::UserRole).toString();
        others_.erase(path);
        delete it;
        emit caseRemoved(path);
    }
    if (caseList_->count() == 0) { clearActiveCase(); return; }
    relabelCases();    // a case left alone with its name drops the tag again
    replot();          // plotted set may have changed even if active did not
}

void SummaryPlotWidget::clearActiveCase()
{
    smry_.reset();
    others_.clear();
    vecs_.clear();
    tree_->clear();
    rebuildFilters();
    replot();
    setStatus(QStringLiteral("no case loaded - run a job or open an SMSPEC"));
}

void SummaryPlotWidget::browseCase()
{
    const QString f = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open summary specification"),
        flowgui::startDir(QStringLiteral("smspec"), activePath()),
        QStringLiteral("Summary spec (*.SMSPEC);;All files (*)"));
    if (!f.isEmpty()) {
        flowgui::rememberDir(QStringLiteral("smspec"), f);
        addCase(QFileInfo(f).completeBaseName(), f);
        activateCase(f);
    }
}

// ---------------------------------------------------------------------------
void SummaryPlotWidget::reload(bool keepSelection)
{
    const QString path = activePath();
    if (path.isEmpty()) return;
    if (!QFileInfo::exists(path)) {
        setStatus(QStringLiteral("waiting for %1 ...").arg(path));
        return;
    }

    QStringList reselect;
    if (keepSelection)
        for (auto* it : tree_->selectedItems())
            if (it->data(0, RoleVecIndex).isValid())
                reselect << it->data(0, RoleVecIndex).toString();   // key stored below

    // opm-common resolves the file against the working directory even though
    // this path is absolute, so a deleted working directory would make every
    // case unreadable. Step out of it before trying.
    flowgui::ensureWorkingDirectory();

    // Re-open for a fresh snapshot; while flow is still writing, a read can
    // transiently fail - keep the previous data and try again next refresh.
    std::unique_ptr<ESmry> next;
    try {
        next = std::make_unique<ESmry>(path.toStdString());
    } catch (const std::exception& e) {
        // Keeping the old data is right for a REFRESH of the case already
        // shown. For a different case it would leave the plot showing one run
        // under another one's name, which is worse than showing nothing: drop
        // it, and say which case failed rather than just that one did.
        if (!flowgui::sameCasePath(smryPath_, path)) {
            smry_.reset();
            smryPath_.clear();
            vecs_.clear();
            tree_->clear();
            rebuildFilters();
            replot();
        }
        setStatus(QStringLiteral("could not read %1 (still being written?): %2")
                      .arg(activeLabel(), QString::fromLocal8Bit(e.what())));
        return;
    }
    smry_ = std::move(next);
    smryPath_ = path;
    others_.clear();   // comparison readers reopen lazily on the next replot

    // Grid dimensions from the SMSPEC's DIMENS array (needed to show block /
    // connection cells as I,J,K); tolerated to fail -> plain numbers.
    nx_ = ny_ = nz_ = 0;
    try {
        Opm::EclIO::EclFile spec(path.toStdString());
        if (spec.hasKey("DIMENS")) {
            const auto dims = spec.get<int>("DIMENS");
            if (dims.size() >= 4) { nx_ = dims[1]; ny_ = dims[2]; nz_ = dims[3]; }
        }
    } catch (...) {}

    // Parse every summary node into a plottable Vec.
    vecs_.clear();
    for (const auto& node : smry_->summaryNodeList()) {
        Vec v;
        v.node    = node;
        v.keyword = QString::fromStdString(node.keyword);
        splitItem(node, nx_, ny_, v.itemMain, v.itemSub);
        v.item    = v.itemSub.isEmpty() ? v.itemMain
                                        : v.itemMain + QLatin1Char(':') + v.itemSub;
        v.cat     = node.category;
        v.type    = node.type;
        v.key     = v.item.isEmpty() ? v.keyword : (v.keyword + QLatin1Char(':') + v.item);
        try { v.unit = QString::fromStdString(smry_->get_unit(node)).trimmed(); }
        catch (...) { v.unit.clear(); }
        vecs_.push_back(v);
    }

    rebuildFilters();
    rebuildTree(reselect);

    setStatus(QStringLiteral("%1: %2 vectors, %3 timesteps")
                  .arg(QFileInfo(path).completeBaseName())
                  .arg(vecs_.size())
                  .arg(int(smry_->numberOfTimeSteps())));
}

void SummaryPlotWidget::rebuildFilters()
{
    // Category and Type boxes list only the values actually present.
    const QString prevCat  = catBox_->currentText();
    const QString prevType = typeBox_->currentText();

    QSet<int> cats, types;
    for (const auto& v : vecs_) { cats.insert(int(v.cat)); types.insert(int(v.type)); }

    catBox_->blockSignals(true);
    catBox_->clear();
    catBox_->addItem(QStringLiteral("All"), -1);
    static const Cat order[] = { Cat::Field, Cat::Well, Cat::Group, Cat::Region,
        Cat::Block, Cat::Connection, Cat::Completion, Cat::Segment, Cat::Aquifer,
        Cat::Node, Cat::Miscellaneous };
    for (Cat c : order)
        if (cats.contains(int(c))) catBox_->addItem(categoryName(c), int(c));
    int ci = catBox_->findText(prevCat);
    catBox_->setCurrentIndex(ci < 0 ? 0 : ci);
    catBox_->blockSignals(false);

    typeBox_->blockSignals(true);
    typeBox_->clear();
    typeBox_->addItem(QStringLiteral("All"), -1);
    static const Type torder[] = { Type::Rate, Type::Total, Type::Ratio,
        Type::Pressure, Type::Count, Type::Mode, Type::ProdIndex, Type::Undefined };
    for (Type t : torder)
        if (types.contains(int(t))) typeBox_->addItem(typeName(t), int(t));
    int ti = typeBox_->findText(prevType);
    typeBox_->setCurrentIndex(ti < 0 ? 0 : ti);
    typeBox_->blockSignals(false);

    populateItemBox();
}

// Item box lists the FIRST-LEVEL items (well/group names, region numbers,
// block cells, ...) of the selected category, "All items" first. The second
// level (cells of a well, for Connection/Completion/Segment) lives in the
// cascading Cell box, populated by populateSubItemBox().
void SummaryPlotWidget::populateItemBox()
{
    const QString prevItem = itemBox_->currentText();
    const int selCat = catBox_->currentData().toInt();
    QSet<QString> items;
    for (const auto& v : vecs_)
        if ((selCat < 0 || int(v.cat) == selCat) && !v.itemMain.isEmpty())
            items.insert(v.itemMain);
    QStringList sorted(items.begin(), items.end());
    sortItems(sorted);
    itemBox_->blockSignals(true);
    itemBox_->clear();
    itemBox_->addItem(QStringLiteral("All items"), QString());
    for (const QString& s : sorted) itemBox_->addItem(s, s);
    int ii = itemBox_->findText(prevItem);
    itemBox_->setCurrentIndex(ii < 0 ? 0 : ii);
    itemBox_->blockSignals(false);

    populateSubItemBox();
}

// Cell box: visible only when the current category selection carries
// two-level items (connections/completions/segments); enabled once a
// specific first-level item (well) is chosen, listing that well's cells.
void SummaryPlotWidget::populateSubItemBox()
{
    const int     selCat  = catBox_->currentData().toInt();
    const QString selMain = itemBox_->currentData().toString();
    const QString prevSub = subItemBox_->currentText();

    bool anySub = false;
    QSet<QString> subs;
    for (const auto& v : vecs_) {
        if (selCat >= 0 && int(v.cat) != selCat) continue;
        if (v.itemSub.isEmpty()) continue;
        anySub = true;
        if (!selMain.isEmpty() && v.itemMain == selMain) subs.insert(v.itemSub);
    }

    subItemBox_->blockSignals(true);
    subItemBox_->clear();
    if (!anySub) {
        subLabel_->hide(); subItemBox_->hide();
    } else {
        subLabel_->show(); subItemBox_->show();
        subItemBox_->addItem(QStringLiteral("All"), QString());
        if (selMain.isEmpty()) {
            subItemBox_->setEnabled(false);   // pick a well first
        } else {
            subItemBox_->setEnabled(true);
            QStringList sorted(subs.begin(), subs.end());
            sortItems(sorted);
            for (const QString& s : sorted) subItemBox_->addItem(s, s);
            int si = subItemBox_->findText(prevSub);
            subItemBox_->setCurrentIndex(si < 0 ? 0 : si);
        }
    }
    subItemBox_->blockSignals(false);
}

void SummaryPlotWidget::rebuildTree(const QStringList& reselect)
{
    const int     selCat  = catBox_->currentData().toInt();
    const int     selType = typeBox_->currentData().toInt();
    const QString selItem = itemBox_->currentData().toString();
    const QString selSub  = (subItemBox_->isVisible() && subItemBox_->isEnabled())
                                ? subItemBox_->currentData().toString() : QString();
    const QString search  = filter_->text().trimmed();

    // The search box: comma-separated terms, any of which may match ("or").
    // A term with * or ? is a wildcard pattern over the KEYWORD:ITEM key
    // (qsummary-style, e.g. WBHP:B*); a plain term matches as substring in
    // the key or the friendly quantity name.
    QVector<QRegularExpression> wilds;
    QStringList substrings;
    for (const QString& t : search.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString term = t.trimmed();
        if (term.isEmpty()) continue;
        if (term.contains(QLatin1Char('*')) || term.contains(QLatin1Char('?')))
            wilds.push_back(QRegularExpression::fromWildcard(term, Qt::CaseInsensitive));
        else
            substrings << term;
    }

    // Preserve the FOCUSED subplot's selection across any rebuild (filter
    // change or refresh): keys that survive the new filter stay selected in
    // the tree; hidden ones stay in chartSel_ and keep plotting.
    QSet<QString> keep(reselect.begin(), reselect.end());
    if (focusChart_ >= 0 && focusChart_ < chartSel_.size())
        for (const QString& k : std::as_const(chartSel_[focusChart_]))
            keep.insert(k);

    // ... and the view: which keyword groups are expanded, and the scroll
    // position, so a refresh does not collapse the list.
    QSet<QString> expanded;
    const bool hadItems = tree_->topLevelItemCount() > 0;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto* it = tree_->topLevelItem(i);
        if (it->childCount() > 0 && it->isExpanded()) expanded.insert(it->text(0));
    }
    const int scrollPos = tree_->verticalScrollBar()->value();

    tree_->blockSignals(true);
    tree_->clear();

    // Group the filtered vectors by keyword; each group's children are the
    // matching items. A group with a single member is shown as a leaf.
    QHash<QString, QList<int>> byKeyword;   // keyword -> indices into vecs_
    QStringList keywordOrder;
    for (int i = 0; i < vecs_.size(); ++i) {
        const Vec& v = vecs_[i];
        if (selCat  >= 0 && int(v.cat)  != selCat)  continue;
        if (selType >= 0 && int(v.type) != selType) continue;
        if (!selItem.isEmpty() && v.itemMain != selItem) continue;
        if (!selSub.isEmpty()  && v.itemSub  != selSub)  continue;
        if (!wilds.isEmpty() || !substrings.isEmpty()) {
            bool hit = false;
            for (const auto& re : wilds)
                if (re.match(v.key).hasMatch()) { hit = true; break; }
            if (!hit) {
                const QString fn = friendlyName(v.keyword, v.cat);
                for (const QString& s : substrings)
                    if (v.key.contains(s, Qt::CaseInsensitive) ||
                        fn.contains(s, Qt::CaseInsensitive)) { hit = true; break; }
            }
            if (!hit) continue;
        }
        if (!byKeyword.contains(v.keyword)) keywordOrder << v.keyword;
        byKeyword[v.keyword] << i;
    }
    keywordOrder.sort();

    for (const QString& kw : std::as_const(keywordOrder)) {
        const QList<int>& idxs = byKeyword[kw];
        const Vec& first = vecs_[idxs.first()];
        const QString fn = friendlyName(kw, first.cat);
        const QString kwLabel = fn.isEmpty() ? kw : (kw + QStringLiteral("  -  ") + fn);

        if (idxs.size() == 1) {
            const Vec& v = vecs_[idxs.first()];
            auto* leaf = new QTreeWidgetItem(tree_);
            leaf->setText(0, v.item.isEmpty() ? kwLabel
                                              : (kwLabel + QStringLiteral("  [") + v.item + QLatin1Char(']')));
            leaf->setData(0, RoleVecIndex, v.key);
            leaf->setData(0, RoleVecIndex + 1, idxs.first());
            if (keep.contains(v.key)) leaf->setSelected(true);
        } else {
            auto* grp = new QTreeWidgetItem(tree_);
            grp->setText(0, kwLabel);
            grp->setFlags(grp->flags() & ~Qt::ItemIsSelectable);   // select children, not the group
            for (int idx : idxs) {
                const Vec& v = vecs_[idx];
                auto* leaf = new QTreeWidgetItem(grp);
                leaf->setText(0, v.item.isEmpty() ? kw : v.item);
                leaf->setData(0, RoleVecIndex, v.key);
                leaf->setData(0, RoleVecIndex + 1, idx);
                if (keep.contains(v.key)) leaf->setSelected(true);
            }
            if (expanded.contains(kwLabel)) grp->setExpanded(true);
        }
    }
    tree_->blockSignals(false);
    if (hadItems) tree_->verticalScrollBar()->setValue(scrollPos);

    // Selection was set with signals blocked (or cleared by clear()); sync the
    // chart to whatever is now selected.
    replot();
}

// The cases to plot: every CHECKED case in the list. The active one uses
// the already-open reader; others open lazily and are skipped silently
// while unreadable (e.g. still being written).
std::vector<std::pair<QString, Opm::EclIO::ESmry*>> SummaryPlotWidget::checkedCases()
{
    std::vector<std::pair<QString, Opm::EclIO::ESmry*>> plotCases;
    checkedCount_ = 0;
    unreadable_.clear();
    const QString active = activePath();
    flowgui::ensureWorkingDirectory();      // see reload()
    for (int i = 0; i < caseList_->count(); ++i) {
        auto* item = caseList_->item(i);
        if (item->checkState() != Qt::Checked) continue;
        ++checkedCount_;
        const QString p = item->data(Qt::UserRole).toString();
        if (p == active) {
            if (smry_) plotCases.push_back({ item->text(), smry_.get() });
            else       unreadable_ << item->text();
            continue;
        }
        auto it = others_.find(p);
        if (it == others_.end()) {
            std::unique_ptr<Opm::EclIO::ESmry> s;
            try { s = std::make_unique<Opm::EclIO::ESmry>(p.toStdString()); }
            // A checked case that cannot be read is not plotted, and silence
            // there reads as "this run has no such curve": name it instead.
            catch (...) { unreadable_ << item->text(); continue; }
            it = others_.emplace(p, std::move(s)).first;
        }
        if (it->second) plotCases.push_back({ item->text(), it->second.get() });
        else            unreadable_ << item->text();
    }
    return plotCases;
}

// Show this many rows and columns of subplots, and say so on the button.
void SummaryPlotWidget::setLayoutGrid(int rows, int cols)
{
    rows = std::clamp(rows, 1, kMaxRows);
    cols = std::clamp(cols, 1, kMaxCols);
    applyChartLayout(rows, cols);
    if (layoutBtn_)
        layoutBtn_->setText(rows * cols == 1
                                ? QStringLiteral("Layout: 1 chart")
                                : QStringLiteral("Layout: %1 x %2").arg(rows).arg(cols));
    replot();
}

// Build charts up to `n` of them. Called for whatever layout is asked for,
// so a session that never leaves one chart never pays for sixteen - and a
// grid nobody expected is a matter of adding more, not of a limit.
void SummaryPlotWidget::ensureCharts(int n)
{
    n = std::min(n, kMaxCharts);
    while (charts_.size() < n) {
        auto* c = new QChart;
        c->legend()->setVisible(true);
        c->legend()->setAlignment(Qt::AlignBottom);
        styleChart(c);
        // a floating legend must follow the plot area as the chart resizes
        connect(c, &QChart::plotAreaChanged, this,
                [this, c](const QRectF&) { placeLegend(c); });
        auto* v = new QChartView(c, chartArea_);
        v->setRenderHint(QPainter::Antialiasing);
        v->setRubberBand(QChartView::RectangleRubberBand);   // drag to zoom
        // size hints vary with legend content; ignore them so the grid
        // splits the area into equal-sized subplots
        v->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        v->installEventFilter(this);
        v->viewport()->installEventFilter(this);
        v->setMouseTracking(true);      // to show the drag cursor on hover
        v->hide();
        charts_.push_back(c);
        chartViews_.push_back(v);
        chartSel_.push_back({});
        zoomSnap_.push_back({});
        legendPos_.push_back(QPointF());
    }
}

double SummaryPlotWidget::plotScale(QChart* chart) const
{
    if (!autoScale_ || !autoScale_->isChecked()) return 1.0;
    QSize s;
    const int idx = charts_.indexOf(chart);
    if (idx >= 0 && idx < chartViews_.size()) s = chartViews_[idx]->size();
    if (s.isEmpty() && chartArea_) s = chartArea_->size();
    if (s.isEmpty()) return 1.0;
    // 1.0 at about the size of one chart in a normal window. Area, not width,
    // so a 2x2 layout (a quarter of the area) lands near half - which is what
    // a line width or a font size has to do to look the same.
    constexpr double refArea = 1100.0 * 700.0;
    const double f = std::sqrt(double(s.width()) * double(s.height()) / refArea);
    // Bounded at both ends: a maximised window should not draw fat curves,
    // and a tiny one still has to be readable.
    return std::clamp(f, 0.55, 1.25);
}

// -- the order of the list is the order of the curves -----------------------
void SummaryPlotWidget::caseOrderChanged()
{
    QStringList paths;
    for (int i = 0; i < caseList_->count(); ++i)
        paths << caseList_->item(i)->data(Qt::UserRole).toString();
    emit caseOrder(paths);
    replot();
}

void SummaryPlotWidget::moveCase(int delta)
{
    const int row = caseList_->currentRow();
    const int to = row + delta;
    if (row < 0 || to < 0 || to >= caseList_->count()) return;
    // takeItem/insertItem move the item itself, so its path, tag and check
    // state travel with it.
    const QSignalBlocker block(caseList_);      // not a case switch
    QListWidgetItem* it = caseList_->takeItem(row);
    caseList_->insertItem(to, it);
    caseList_->setCurrentItem(it);
    caseOrderChanged();
}

void SummaryPlotWidget::sortCases(int mode)
{
    const int n = caseList_->count();
    if (n < 2) return;
    QListWidgetItem* current = caseList_->currentItem();

    std::vector<QListWidgetItem*> items;
    items.reserve(n);
    {
        const QSignalBlocker block(caseList_);
        while (caseList_->count()) items.push_back(caseList_->takeItem(0));
    }
    auto seq = [](QListWidgetItem* it) { return it->data(RoleCaseSeq).toInt(); };
    switch (mode) {
    case SortNameDesc:
        std::stable_sort(items.begin(), items.end(), [](auto* a, auto* b) {
            return a->text().compare(b->text(), Qt::CaseInsensitive) > 0; });
        break;
    case SortCheckedFirst:
        // What is plotted first, in name order within each group - the rest
        // stays in the list, just out of the way.
        std::stable_sort(items.begin(), items.end(), [](auto* a, auto* b) {
            const bool ca = a->checkState() == Qt::Checked;
            const bool cb = b->checkState() == Qt::Checked;
            if (ca != cb) return ca;
            return a->text().compare(b->text(), Qt::CaseInsensitive) < 0; });
        break;
    case SortLoadOrder:
        std::stable_sort(items.begin(), items.end(),
                         [&seq](auto* a, auto* b) { return seq(a) < seq(b); });
        break;
    default:
        std::stable_sort(items.begin(), items.end(), [](auto* a, auto* b) {
            return a->text().compare(b->text(), Qt::CaseInsensitive) < 0; });
        break;
    }
    {
        const QSignalBlocker block(caseList_);
        for (auto* it : items) caseList_->addItem(it);
        if (current) caseList_->setCurrentItem(current);   // same ACTIVE case
    }
    caseOrderChanged();
}

void SummaryPlotWidget::applyChartLayout(int rows, int cols)
{
    rows = std::clamp(rows, 1, kMaxRows);
    cols = std::clamp(cols, 1, kMaxCols);
    const int n = std::min(rows * cols, kMaxCharts);
    ensureCharts(n);
    if (n == visibleCharts_ && rows == layoutRows_) return;

    // Shrinking below the focused subplot: the focused one survives, in the
    // last still-visible slot (its curves and kept zoom move with it).
    if (focusChart_ >= n) {
        std::swap(chartSel_[focusChart_], chartSel_[n - 1]);
        std::swap(zoomSnap_[focusChart_], zoomSnap_[n - 1]);
        focusChart_ = n - 1;
    }
    for (auto* v : chartViews_) chartGrid_->removeWidget(v);
    for (int i = 0; i < chartViews_.size(); ++i) {
        if (i < n) {
            chartGrid_->addWidget(chartViews_[i], i / cols, i % cols);
            chartViews_[i]->show();
        } else {
            chartViews_[i]->hide();
        }
    }
    // equal-sized subplots: stretch the used rows/columns evenly
    for (int r = 0; r < kMaxRows; ++r) chartGrid_->setRowStretch(r, r < rows ? 1 : 0);
    for (int c = 0; c < kMaxCols; ++c) chartGrid_->setColumnStretch(c, c < cols ? 1 : 0);
    visibleCharts_ = n;
    layoutRows_ = rows;
    layoutCols_ = cols;
    setFocusChart(focusChart_);   // re-mirror the tree, refresh the frames
}

// Back to the natural ranges: every subplot, or only the focused one.
void SummaryPlotWidget::resetZoom(bool focusedOnly)
{
    for (int i = 0; i < visibleCharts_ && i < charts_.size(); ++i) {
        if (focusedOnly && i != focusChart_) continue;
        charts_[i]->zoomReset();
        zoomSnap_[i] = ZoomSnap();       // forget the kept view
    }
    replot();
    if (focusedOnly && visibleCharts_ > 1)
        setStatus(QStringLiteral("subplot %1 back to its full range")
                      .arg(focusChart_ + 1));
}

// Which visible subplot is under this point on screen, or -1.
int SummaryPlotWidget::chartAt(const QPoint& globalPos) const
{
    for (int i = 0; i < visibleCharts_ && i < chartViews_.size(); ++i) {
        QWidget* v = chartViews_[i];
        if (v->isVisible() && v->rect().contains(v->mapFromGlobal(globalPos))) return i;
    }
    return -1;
}

// Exchange what two subplots show. Everything that belongs to a subplot moves
// with it - its vectors, the zoom it was left at and where its legend was
// dragged - so the swap is of the plots, not just of the curves.
void SummaryPlotWidget::swapCharts(int a, int b)
{
    if (a < 0 || b < 0 || a == b || a >= chartSel_.size() || b >= chartSel_.size()) {
        if (a >= 0) setStatus(QStringLiteral("subplot %1 stayed where it was").arg(a + 1));
        return;
    }
    std::swap(chartSel_[a], chartSel_[b]);
    std::swap(zoomSnap_[a], zoomSnap_[b]);
    std::swap(legendPos_[a], legendPos_[b]);
    focusChart_ = b;                       // the tree follows what was dragged
    replot();
    setFocusChart(b);
    setStatus(QStringLiteral("subplots %1 and %2 swapped").arg(a + 1).arg(b + 1));
}

void SummaryPlotWidget::setFocusChart(int i)
{
    if (i < 0 || i >= chartSel_.size()) return;
    focusChart_ = i;
    // Mirror the subplot's selection in the tree (its visible part) so the
    // user sees and edits what this subplot shows.
    const QSet<QString> want(chartSel_[i].begin(), chartSel_[i].end());
    syncingTree_ = true;
    tree_->blockSignals(true);
    QTreeWidgetItemIterator it(tree_);
    while (*it) {
        const QVariant k = (*it)->data(0, RoleVecIndex);
        if (k.isValid()) (*it)->setSelected(want.contains(k.toString()));
        ++it;
    }
    tree_->blockSignals(false);
    syncingTree_ = false;
    updateChartFrames();
    if (visibleCharts_ > 1)
        setStatus(QStringLiteral("subplot %1 focused - the vector tree now edits it")
                      .arg(i + 1));
}

void SummaryPlotWidget::updateChartFrames()
{
    for (int i = 0; i < chartViews_.size(); ++i) {
        if (visibleCharts_ <= 1)
            chartViews_[i]->setStyleSheet(QString());   // single chart: no frame
        else if (i == focusChart_)
            chartViews_[i]->setStyleSheet(
                QStringLiteral("border: 2px solid #1565c0"));
        else
            chartViews_[i]->setStyleSheet(
                QStringLiteral("border: 1px solid #b8bfc6"));
    }
}

QPointF SummaryPlotWidget::chartPos(int idx, const QPoint& viewportPos) const
{
    if (idx < 0 || idx >= charts_.size()) return {};
    // viewport -> scene -> chart-local, which is where legend geometry lives
    return charts_[idx]->mapFromScene(chartViews_[idx]->mapToScene(viewportPos));
}

bool SummaryPlotWidget::eventFilter(QObject* obj, QEvent* ev)
{
    // A case was dragged to a new place in the list: Qt moves the item, and
    // the plot follows once it has finished doing so.
    if (caseList_ && obj == caseList_->viewport() && ev->type() == QEvent::Drop) {
        QTimer::singleShot(0, this, [this] { caseOrderChanged(); });
        return QWidget::eventFilter(obj, ev);
    }
    // The charts were resized, so what is drawn on them has a new size to
    // follow. Coalesced: a drag of the window edge is a stream of these.
    if (chartArea_ && obj == chartArea_ && ev->type() == QEvent::Resize) {
        if (autoScale_ && autoScale_->isChecked() && resizeTimer_) resizeTimer_->start();
        return QWidget::eventFilter(obj, ev);
    }
    // which subplot the event belongs to
    int idx = -1;
    for (int i = 0; i < chartViews_.size() && i < visibleCharts_; ++i)
        if (obj == chartViews_[i] || obj == chartViews_[i]->viewport()) { idx = i; break; }

    if (idx >= 0) {
        auto* lg = charts_[idx]->legend();
        const bool floating = lg->isVisible() && !lg->isAttachedToChart();
        auto* me = static_cast<QMouseEvent*>(ev);

        switch (ev->type()) {
        case QEvent::MouseButtonPress:
            if (visibleCharts_ > 1 && idx != focusChart_) setFocusChart(idx);
            // Ctrl+drag picks a subplot up to swap it with another - the order
            // of a figure is a presentation decision, and rebuilding two
            // selections by hand to reorder them is not one. Consumed, so the
            // rubber band does not start underneath the drag.
            if (visibleCharts_ > 1 && me->button() == Qt::LeftButton
                && (me->modifiers() & Qt::ControlModifier)) {
                swapDrag_ = idx;
                chartViews_[idx]->viewport()->setCursor(Qt::ClosedHandCursor);
                setStatus(QStringLiteral("drop on another subplot to swap them"));
                return true;
            }
            // Grab a floating legend: consume the press so the rubber-band
            // zoom does not start underneath the drag.
            if (floating && me->button() == Qt::LeftButton) {
                const QPointF p = chartPos(idx, me->pos());
                if (lg->geometry().contains(p)) {
                    legendDrag_ = idx;
                    legendGrab_ = p - lg->geometry().topLeft();
                    chartViews_[idx]->viewport()->setCursor(Qt::ClosedHandCursor);
                    return true;
                }
            }
            break;
        case QEvent::MouseMove:
            if (swapDrag_ >= 0) return true;
            if (legendDrag_ == idx) {
                const QRectF r = charts_[idx]->rect();
                const QSizeF sz = lg->geometry().size();
                QPointF tl = chartPos(idx, me->pos()) - legendGrab_;
                tl.setX(std::clamp(tl.x(), r.left(), std::max(r.left(), r.right()  - sz.width())));
                tl.setY(std::clamp(tl.y(), r.top(),  std::max(r.top(),  r.bottom() - sz.height())));
                lg->setGeometry(QRectF(tl, sz));
                if (r.width() > 0 && r.height() > 0)
                    legendPos_[idx] = QPointF((tl.x() - r.left()) / r.width(),
                                              (tl.y() - r.top())  / r.height());
                return true;
            }
            if (floating && !(me->buttons() & Qt::LeftButton))   // hover affordance
                chartViews_[idx]->viewport()->setCursor(
                    lg->geometry().contains(chartPos(idx, me->pos()))
                        ? Qt::OpenHandCursor : Qt::ArrowCursor);
            break;
        case QEvent::MouseButtonRelease:
            if (swapDrag_ >= 0) {
                const int from = swapDrag_;
                swapDrag_ = -1;
                chartViews_[from]->viewport()->setCursor(Qt::ArrowCursor);
                swapCharts(from, chartAt(me->globalPosition().toPoint()));
                return true;
            }
            if (legendDrag_ == idx) {
                legendDrag_ = -1;
                chartViews_[idx]->viewport()->setCursor(Qt::OpenHandCursor);
                return true;
            }
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(obj, ev);   // otherwise let zooming work
}

void SummaryPlotWidget::replot()
{
    applyChartLayout(layoutRows_, layoutCols_);

    for (int i = 0; i < charts_.size(); ++i) {
        QChart* c = charts_[i];
        // a rubber-band zoom is in effect: remember it so the refresh does
        // not yank the view (sticky until Reset zoom)
        if (c->isZoomed()) zoomSnap_[i] = captureZoom(c);
        c->removeAllSeries();          // series are deleted by Qt
        const auto oldAxes = c->axes();
        for (auto* a : oldAxes) {
            c->removeAxis(a);          // removeAxis returns ownership: delete
            delete a;
        }
        c->setTitle(QString());
    }
    if (!smry_) {
        // The vector list comes from the ACTIVE case, so nothing can be drawn
        // while that one is unreadable - not even the other checked cases.
        // Saying so beats an empty plot with no explanation.
        if (!activePath().isEmpty())
            setStatus(QStringLiteral("%1 could not be read - highlight another "
                                     "case in the list to plot that one")
                          .arg(activeLabel()));
        return;
    }

    const auto plotCases = checkedCases();

    QHash<QString, int> byKey;
    for (int i = 0; i < vecs_.size(); ++i) byKey.insert(vecs_[i].key, i);

    const bool useDates = dateAxis_ && dateAxis_->isChecked();
    int skipped = 0;
    for (int c = 0; c < visibleCharts_; ++c) {
        QList<int> sel;
        for (const QString& k : std::as_const(chartSel_[c])) {
            const auto it = byKey.constFind(k);
            if (it != byKey.constEnd()) sel << it.value();
        }
        skipped += plotChart(charts_[c], sel, QString(), plotCases);
        if (zoomSnap_[c].valid) {
            if (zoomSnap_[c].dates == useDates) applyZoom(charts_[c], zoomSnap_[c]);
            else zoomSnap_[c] = ZoomSnap();    // axis type changed: let go
        }
    }
    QStringList notes;
    // A checked case that could not be read is simply absent from the plot,
    // which looks exactly like a run that has no such curve. Name it.
    if (!unreadable_.isEmpty())
        notes << QStringLiteral("not plotted (could not be read): %1")
                     .arg(unreadable_.join(QStringLiteral(", ")));
    if (skipped > 0)
        notes << QStringLiteral("%1 selected vector(s) not shown - a chart mixes "
                                "at most two units").arg(skipped);
    if (!notes.isEmpty()) setStatus(notes.join(QStringLiteral("; ")));
}

SummaryPlotWidget::ZoomSnap SummaryPlotWidget::captureZoom(QChart* chart) const
{
    ZoomSnap z;
    const auto hs = chart->axes(Qt::Horizontal);
    if (hs.isEmpty()) return z;
    if (auto* da = qobject_cast<QDateTimeAxis*>(hs.first())) {
        z.dates = true;
        z.xmin  = double(da->min().toMSecsSinceEpoch());
        z.xmax  = double(da->max().toMSecsSinceEpoch());
    } else if (auto* va = qobject_cast<QValueAxis*>(hs.first())) {
        z.xmin = va->min();
        z.xmax = va->max();
    } else {
        return z;
    }
    const auto vs = chart->axes(Qt::Vertical);   // [left, right?] in add order
    if (vs.size() >= 1)
        if (auto* l = qobject_cast<QValueAxis*>(vs[0])) {
            z.hasL = true; z.lmin = l->min(); z.lmax = l->max();
        }
    if (vs.size() >= 2)
        if (auto* r = qobject_cast<QValueAxis*>(vs[1])) {
            z.hasR = true; z.rmin = r->min(); z.rmax = r->max();
        }
    z.valid = true;
    return z;
}

void SummaryPlotWidget::applyZoom(QChart* chart, const ZoomSnap& z)
{
    if (!z.valid) return;
    const auto hs = chart->axes(Qt::Horizontal);
    if (hs.isEmpty()) return;                    // nothing plotted
    if (z.dates) {
        auto* da = qobject_cast<QDateTimeAxis*>(hs.first());
        if (!da) return;
        da->setRange(QDateTime::fromMSecsSinceEpoch(qint64(z.xmin), QTimeZone::utc()),
                     QDateTime::fromMSecsSinceEpoch(qint64(z.xmax), QTimeZone::utc()));
    } else {
        auto* va = qobject_cast<QValueAxis*>(hs.first());
        if (!va) return;
        va->setRange(z.xmin, z.xmax);
    }
    const auto vs = chart->axes(Qt::Vertical);
    if (z.hasL && vs.size() >= 1)
        if (auto* l = qobject_cast<QValueAxis*>(vs[0])) l->setRange(z.lmin, z.lmax);
    if (z.hasR && vs.size() >= 2)
        if (auto* r = qobject_cast<QValueAxis*>(vs[1])) r->setRange(z.rmin, z.rmax);
}

void SummaryPlotWidget::styleChart(QChart* chart)
{
    // Flat white figure: no drop shadow, no rounded frame, tight margins -
    // what a plot dropped into a paper should look like.
    chart->setBackgroundRoundness(0);
    chart->setDropShadowEnabled(false);
    chart->setBackgroundBrush(QBrush(Qt::white));
    chart->setPlotAreaBackgroundVisible(false);
    // NB: do not shrink QChart::margins here - the axis labels and titles are
    // laid out inside those margins, and squeezing them drops the axes.
    QFont tf = chart->titleFont();
    tf.setPointSizeF(10.5);
    tf.setBold(true);
    chart->setTitleFont(tf);
    chart->setTitleBrush(QBrush(QColor(0x22, 0x26, 0x2b)));
    // The legend is what a reader consults to tell the curves apart, so it is
    // set a size above the axis labels and bold, not as fine print. The line
    // sample scales with this font too (Qt draws it 0.75 font heights long).
    QFont lf = chart->legend()->font();
    lf.setPointSizeF(kLegendPointSize);
    lf.setBold(true);
    chart->legend()->setFont(lf);
    chart->legend()->setLabelColor(QColor(0x22, 0x26, 0x2b));
    chart->legend()->setMarkerShape(QLegend::MarkerShapeFromSeries);   // show the dashes
}

void SummaryPlotWidget::styleAxis(QAbstractAxis* axis)
{
    if (!axis) return;
    QFont f = axis->labelsFont();
    f.setPointSizeF(9.0);
    axis->setLabelsFont(f);
    QFont t = axis->titleFont();
    t.setPointSizeF(9.5);
    t.setBold(true);
    axis->setTitleFont(t);
    axis->setLabelsColor(QColor(0x33, 0x38, 0x3d));
    axis->setTitleBrush(QBrush(QColor(0x22, 0x26, 0x2b)));
    axis->setLinePenColor(QColor(0x55, 0x5b, 0x61));
    axis->setGridLineColor(QColor(0xdc, 0xe0, 0xe4));   // light, unobtrusive
    axis->setMinorGridLineVisible(false);
}

void SummaryPlotWidget::placeLegend(QChart* chart)
{
    if (!chart || !legendBox_) return;
    auto* lg = chart->legend();
    const int mode = legendBox_->currentData().toInt();
    if (mode == LegendOff) { lg->setVisible(false); return; }
    lg->setVisible(true);

    if (mode <= LegendRight) {                      // docked to an edge
        lg->attachToChart();
        lg->setBackgroundVisible(false);
        switch (mode) {
            case LegendTop:   lg->setAlignment(Qt::AlignTop);    break;
            case LegendLeft:  lg->setAlignment(Qt::AlignLeft);   break;
            case LegendRight: lg->setAlignment(Qt::AlignRight);  break;
            default:          lg->setAlignment(Qt::AlignBottom); break;
        }
        return;
    }

    // Floating inside the plot area, on a translucent plate so curves
    // underneath stay readable.
    lg->detachFromChart();
    lg->setBackgroundVisible(true);
    lg->setBrush(QBrush(QColor(255, 255, 255, 235)));
    lg->setPen(QPen(QColor(0xb4, 0xba, 0xc0)));
    lg->setZValue(100.0);        // above the curves, which would else cross it
    const QRectF pa = chart->plotArea();
    if (pa.isEmpty()) return;
    const qreal m = 8.0;                            // inset from the axes
    const qreal maxW = std::max(40.0, pa.width()  - 2 * m);
    const qreal maxH = std::max(30.0, pa.height() - 2 * m);
    // One entry per row, and size the plate from the entries themselves:
    // a detached legend's own size hint reports a box too short for its rows
    // and the top entries end up clipped.
    lg->setAlignment(Qt::AlignLeft);                // vertical arrangement
    int rows = 0;
    qreal textW = 0;
    const QFontMetricsF fm(lg->font());
    const auto markers = lg->markers();
    for (auto* mk : markers) {
        // Count the stand-in series carrying the entries, by their NAME - not
        // by marker visibility: once the legend itself is hidden its markers
        // report invisible too, so counting those would latch the legend off
        // for good.
        if (mk->series()->objectName() != kLegendSample) continue;
        ++rows;
        textW = std::max(textW, fm.horizontalAdvance(mk->label()));
    }
    if (rows == 0) { lg->setVisible(false); return; }
    // Qt lays a legend row out at roughly the font height plus the marker
    // padding (~20px); too tight a plate makes it spill into a second column
    // that the plate then clips. Erring tall only costs a little whitespace.
    const qreal rowH = fm.height() + 20.0;
    const qreal swatch = 34.0;                      // colour/dash sample + gap
    QSizeF sz(std::min(textW + swatch + 24.0, maxW),
              std::min(rows * rowH + 14.0,    maxH));
    if (sz.width() <= 0 || sz.height() <= 0) return;
    const bool left = (mode == LegendInTL || mode == LegendInBL);
    const bool top  = (mode == LegendInTL || mode == LegendInTR);
    qreal x = left ? pa.left() + m : pa.right()  - m - sz.width();
    qreal y = top  ? pa.top()  + m : pa.bottom() - m - sz.height();

    // A legend the user dragged keeps that spot instead of the corner; the
    // position is a fraction of the chart rect, so it holds across resizes.
    const int idx = charts_.indexOf(chart);
    if (idx >= 0 && idx < legendPos_.size() && !legendPos_[idx].isNull()) {
        const QRectF r = chart->rect();
        x = r.left() + legendPos_[idx].x() * r.width();
        y = r.top()  + legendPos_[idx].y() * r.height();
        x = std::clamp(x, r.left(), std::max(r.left(), r.right()  - sz.width()));
        y = std::clamp(y, r.top(),  std::max(r.top(),  r.bottom() - sz.height()));
    }
    lg->setGeometry(QRectF(QPointF(x, y), sz));
    lg->update();
}

int SummaryPlotWidget::plotChart(QChart* chart, const QList<int>& sel,
    const QString& title,
    const std::vector<std::pair<QString, Opm::EclIO::ESmry*>>& plotCases)
{
    // The legend follows the chart too, times whatever the legend box asks
    // for; styleChart() set the unscaled size when the chart was built.
    {
        const double legendScale =
            (legendScaleSpin_ ? legendScaleSpin_->value() : 1.0) * plotScale(chart);
        QFont lf = chart->legend()->font();
        lf.setPointSizeF(std::clamp(kLegendPointSize * legendScale, 4.0, 30.0));
        lf.setBold(true);
        chart->legend()->setFont(lf);
    }
    if (sel.isEmpty() || plotCases.empty()) {
        chart->setTitle(!title.isEmpty() ? title : activeLabel());
        return 0;
    }
    // Name the case in every entry as soon as more than one is CHECKED - not
    // only when more than one could be read. With a case missing, an unnamed
    // curve leaves the reader guessing which of the checked runs is on screen,
    // which is exactly when the answer matters.
    const bool multi = std::max<int>(checkedCount_, int(plotCases.size())) > 1;

    // Two Y axes at most, keyed by unit (from the active case). The left axis
    // carries the first distinct unit (or a generic "value" axis when the
    // selection has none); a second distinct unit gets the right axis. Series
    // with a third unit are not plotted and are reported in the status line.
    QString unitL, unitR;
    bool haveL = false, haveR = false;
    for (int i : sel) {
        const QString u = vecs_[i].unit;
        if (u.isEmpty()) continue;
        if      (!haveL)            { unitL = u; haveL = true; }
        else if (u != unitL && !haveR) { unitR = u; haveR = true; }
    }
    const bool genericLeft = !haveL;
    auto axisFor = [&](const QString& u) -> int {   // 0 left, 1 right, -1 skip
        if (genericLeft) return 0;
        if (u == unitL)  return 0;
        if (haveR && u == unitR) return 1;
        return -1;
    };

    const bool useDates = dateAxis_ && dateAxis_->isChecked();
    const bool showPts  = markers_ && markers_->isChecked();
    // Everything drawn on the chart is sized for THIS chart: the same figure
    // in a 2x2 layout gets thinner curves, smaller markers and a smaller
    // legend, in the same proportions.
    const double scale  = plotScale(chart);
    const double lineW  = (lineWidthSpin_  ? lineWidthSpin_->value()  : 2.0) * scale;
    // Width of the legend's line sample: heavier than the curve, since it is
    // read at a couple of font heights rather than across the plot - but
    // never thinner than the curve it stands for.
    const double legendW = std::max(lineW, std::clamp(lineW * 1.8, 3.0, 6.0));
    const double markerS = (markerSizeSpin_ ? markerSizeSpin_->value() : 7.5) * scale;
    // 1 = mark every data point (the default: markers are the data)
    const int markerEvery = std::max(1, markerEverySpin_ ? markerEverySpin_->value() : 1);

    // What colour means depends on which dimension actually varies. Comparing
    // ONE vector across several cases - the usual comparison - colour has to
    // separate the CASES; keying it to the vector would paint every curve the
    // same. With several vectors, colour keys the vector and the dash the
    // case, so both dimensions stay readable.
    const bool colourByCase = multi && sel.size() == 1;

    // How many ticks a subplot this size can label. Qt ELIDES a label that
    // does not fit rather than dropping the tick, so in a dense grid a date
    // turns into "2018-1..." unless there are fewer of them. The labels
    // themselves stay at full size - they are the last thing that should
    // shrink.
    const int vidx = charts_.indexOf(chart);
    const QSize vsz = (vidx >= 0 && vidx < chartViews_.size())
                          ? chartViews_[vidx]->size() : QSize(900, 600);
    const int xTicks = std::clamp(vsz.width()  / (useDates ? 170 : 130), 2, 6);
    const int yTicks = std::clamp(vsz.height() / 90, 2, 6);

    QAbstractAxis* ax = nullptr;
    if (useDates) {
        auto* a = new QDateTimeAxis;
        a->setFormat(QStringLiteral("yyyy-MM-dd"));
        a->setTitleText(QStringLiteral("date"));
        a->setTickCount(xTicks);
        ax = a;
    } else {
        auto* a = new QValueAxis;
        a->setTitleText(QStringLiteral("time [days]"));
        a->setTickCount(xTicks);
        ax = a;
    }
    chart->addAxis(ax, Qt::AlignBottom);
    QValueAxis* ayL = new QValueAxis;
    ayL->setTitleText(genericLeft ? QStringLiteral("value") : unitL);
    chart->addAxis(ayL, Qt::AlignLeft);
    QValueAxis* ayR = nullptr;
    if (haveR) {
        ayR = new QValueAxis; ayR->setTitleText(unitR);
        chart->addAxis(ayR, Qt::AlignRight);
    }
    styleAxis(ax); styleAxis(ayL); styleAxis(ayR);

    double lmin = 0, lmax = 0, rmin = 0, rmax = 0; bool lset = false, rset = false;
    double xmin = 0, xmax = 0; bool xset = false;
    int skipped = 0;

    // Marker shapes cycle PER CASE, so overlapping runs stay distinguishable
    // by shape even where the curves coincide.
    static const QScatterSeries::MarkerShape kShapes[] = {
        QScatterSeries::MarkerShapeCircle,
        QScatterSeries::MarkerShapeRectangle,
        QScatterSeries::MarkerShapeTriangle,
        QScatterSeries::MarkerShapeStar,
        QScatterSeries::MarkerShapePentagon,
        QScatterSeries::MarkerShapeRotatedRectangle,
    };
    constexpr int kShapeCount = int(sizeof(kShapes) / sizeof(kShapes[0]));

    for (int ci = 0; ci < int(plotCases.size()); ++ci) {
        const auto& pc = plotCases[ci];    // (label, reader)
        std::vector<float> time;
        try {
            if (pc.second->hasKey("TIME")) time = pc.second->get(std::string("TIME"));
        } catch (...) {}
        if (time.empty()) continue;

        double startMs = 0.0;
        if (useDates) {
            try {
                const auto tp = pc.second->startdate();
                startMs = double(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     tp.time_since_epoch()).count());
            } catch (...) {}
        }
        auto xval = [&](float days) {
            return useDates ? startMs + double(days) * 86400.0e3 : double(days);
        };
        const bool isActive = (pc.second == smry_.get());

        for (int si = 0; si < sel.size(); ++si) {
            const int i = sel[si];
            const Vec& v = vecs_[i];
            const int side = axisFor(v.unit);
            if (side < 0) { if (isActive) ++skipped; continue; }

            std::vector<float> data;
            try {
                const std::string key = v.key.toStdString();
                if (pc.second->hasKey(key))     data = pc.second->get(key);
                else if (isActive)              data = pc.second->get(v.node);
                else                            continue;   // vector absent in this case
            } catch (...) { continue; }

            auto* s = new QLineSeries;
            s->setName(multi ? pc.first + QStringLiteral(" | ") + v.key : v.key);
            // dash always keys the case (so a print in grey still separates
            // them); colour keys whichever dimension carries the information
            QPen pen(kCurveColors[(colourByCase ? ci : si) % kCurveColorCount]);
            pen.setWidthF(lineW);
            pen.setStyle(kCaseDashes[ci % kCaseDashCount]);
            pen.setCosmetic(true);
            s->setPen(pen);
            const size_t n = std::min(time.size(), data.size());
            for (size_t k = 0; k < n; ++k) {
                const double x = xval(time[k]);
                s->append(x, data[k]);
                xmin = xset ? std::min(xmin, x) : x;
                xmax = xset ? std::max(xmax, x) : x;
                xset = true;
                if (side == 1) {
                    rmin = rset ? std::min<double>(rmin, data[k]) : data[k];
                    rmax = rset ? std::max<double>(rmax, data[k]) : data[k];
                    rset = true;
                } else {
                    lmin = lset ? std::min<double>(lmin, data[k]) : data[k];
                    lmax = lset ? std::max<double>(lmax, data[k]) : data[k];
                    lset = true;
                }
            }
            chart->addSeries(s);
            s->attachAxis(ax);
            s->attachAxis(side == 1 ? ayR : ayL);

            // Hand the legend entry to an empty stand-in drawn with a heavier
            // pen: Qt draws a sample with the series' own pen, so the curve's
            // own entry can only ever be as thin as the curve. The stand-in
            // holds no points, so it adds nothing to the plot area - just a
            // sample carrying the curve's colour and case dash.
            auto* sample = new QLineSeries;
            sample->setObjectName(kLegendSample);
            sample->setName(s->name());
            sample->setPen(legendPen(pen, legendW));
            if (showPts) {
                // Markers are on, so the entry shows what the curve shows: the
                // case's shape riding on the sample line. Qt sizes a sample
                // carrying a marker from the series' marker size, which also
                // buys the line a little more length to show its dash in.
                const qreal box  = std::clamp(2.6 * markerS, 20.0, 30.0);
                const qreal size = std::min(markerS, box - 3.0);
                sample->setMarkerSize(box);
                sample->setLightMarker(
                    sampleMarker(kShapes[ci % kShapeCount], box, size,
                                 pen.color(), ci == 0));
            }
            chart->addSeries(sample);
            sample->attachAxis(ax);
            sample->attachAxis(side == 1 ? ayR : ayL);
            const auto own = chart->legend()->markers(s);
            for (auto* m : own) m->setVisible(false);

            if (showPts) {
                // Overlay a scatter with a per-case shape in the line's colour;
                // keep it out of the legend (the line represents both).
                //
                // Every data point is marked by default - a marker is a real
                // sample from the summary, so leaving some out would misreport
                // where the data actually is. "Every" thins that down on a
                // dense curve, by whole data points, so the markers that are
                // drawn are still exactly samples.
                const QList<QPointF> pts = s->points();
                QList<QPointF> marks;
                marks.reserve(int(pts.size()) / markerEvery + 1);
                for (int k = 0; k < pts.size(); k += markerEvery)
                    marks.append(pts[k]);

                auto* sc = new QScatterSeries;
                sc->replace(marks);
                sc->setMarkerShape(kShapes[ci % kShapeCount]);
                sc->setMarkerSize(markerS);
                chart->addSeries(sc);
                sc->attachAxis(ax);
                sc->attachAxis(side == 1 ? ayR : ayL);
                const QColor col = s->color();
                // First case filled, the rest hollow: where markers land on
                // each other the one underneath shows through the ring.
                sc->setBrush(ci == 0 ? QBrush(col) : QBrush(Qt::transparent));
                sc->setPen(QPen(col, 1.6));
                sc->setBorderColor(col);
                const auto lms = chart->legend()->markers(sc);
                for (auto* m : lms) m->setVisible(false);

                // Hovering a marker reports the sample it stands for.
                const QString name = s->name();
                const QString unit = v.unit;
                connect(sc, &QScatterSeries::hovered, this,
                        [name, unit, useDates](const QPointF& p, bool on) {
                    if (!on) { QToolTip::hideText(); return; }
                    const QString when = useDates
                        ? QDateTime::fromMSecsSinceEpoch(qint64(p.x()), QTimeZone::utc())
                              .toString(QStringLiteral("yyyy-MM-dd"))
                        : QStringLiteral("%1 days").arg(p.x(), 0, 'f', 2);
                    QToolTip::showText(QCursor::pos(),
                        QStringLiteral("%1\n%2\n%3%4").arg(name, when)
                            .arg(p.y(), 0, 'g', 6)
                            .arg(unit.isEmpty() ? QString() : QLatin1Char(' ') + unit));
                });
            }
        }
    }

    if (xset) {
        if (useDates) {
            static_cast<QDateTimeAxis*>(ax)->setRange(
                QDateTime::fromMSecsSinceEpoch(qint64(xmin), QTimeZone::utc()),
                QDateTime::fromMSecsSinceEpoch(qint64(xmax), QTimeZone::utc()));
        } else {
            static_cast<QValueAxis*>(ax)->setRange(xmin, xmax);
        }
    }

    // Round tick values ("6 000 000", not "5581102.4"): applyNiceNumbers()
    // widens the range to the next round step and picks a matching tick count.
    auto pad = [yTicks](QValueAxis* a, double lo, double hi) {
        a->setTickCount(yTicks);
        if (hi > lo) a->setRange(lo - 0.05 * (hi - lo), hi + 0.05 * (hi - lo));
        else         a->setRange(lo - 1.0, hi + 1.0);
        a->applyNiceNumbers();
        // Drop the pointless ".0" the default format leaves on round ticks,
        // and go scientific once the digits would run away with the margin.
        const double m = std::max(std::fabs(a->min()), std::fabs(a->max()));
        if      (m >= 1e7)  a->setLabelFormat(QStringLiteral("%.3g"));
        else if (m >= 1000) a->setLabelFormat(QStringLiteral("%.0f"));
    };
    if (lset) pad(ayL, lmin, lmax);
    if (ayR && rset) pad(ayR, rmin, rmax);
    // The title counts what is DRAWN, and says so when that is not every case
    // that was asked for.
    QString shown = plotCases.size() > 1
        ? QStringLiteral("%1 cases").arg(plotCases.size())
        : plotCases.front().first;
    if (checkedCount_ > int(plotCases.size()))
        shown += QStringLiteral("  (%1 of %2 checked cases)")
                     .arg(plotCases.size()).arg(checkedCount_);
    chart->setTitle(!title.isEmpty() ? title : shown);
    placeLegend(chart);      // the legend just changed size
    return skipped;
}

// Export the plotted vectors (tree selection or expression, all subplots
// merged) of every checked case as CSV: per case a TIME column followed by
// that case's curves, blocks side by side, short columns padded with blanks.
void SummaryPlotWidget::saveCsv()
{
    if (!smry_) { setStatus(QStringLiteral("no case loaded - nothing to export")); return; }
    // Union of every visible subplot's selection, first-seen order.
    QHash<QString, int> byKey;
    for (int i = 0; i < vecs_.size(); ++i) byKey.insert(vecs_[i].key, i);
    QList<int> sel;
    QSet<int> seen;
    for (int c = 0; c < visibleCharts_ && c < chartSel_.size(); ++c)
        for (const QString& k : std::as_const(chartSel_[c])) {
            const auto it = byKey.constFind(k);
            if (it != byKey.constEnd() && !seen.contains(it.value())) {
                seen.insert(it.value());
                sel << it.value();
            }
        }
    if (sel.isEmpty()) {
        setStatus(QStringLiteral("nothing plotted - select vectors in the tree first"));
        return;
    }
    const auto plotCases = checkedCases();
    if (plotCases.empty()) {
        setStatus(QStringLiteral("no case is checked - nothing to export"));
        return;
    }

    QString suggested = activeLabel();
    if (suggested.isEmpty()) suggested = QStringLiteral("summary");
    const QString f = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export plotted vectors as CSV"),
        QDir(flowgui::startDir(QStringLiteral("csv")))
            .filePath(suggested + QStringLiteral(".csv")),
        QStringLiteral("CSV (*.csv)"));
    if (f.isEmpty()) return;
    flowgui::rememberDir(QStringLiteral("csv"), f);

    struct Col { QString header; std::vector<float> data; };
    std::vector<Col> cols;
    size_t rows = 0;
    for (const auto& pc : plotCases) {
        std::vector<float> time;
        try {
            if (pc.second->hasKey("TIME")) time = pc.second->get(std::string("TIME"));
        } catch (...) {}
        if (time.empty()) continue;
        cols.push_back({ QStringLiteral("TIME [days] (%1)").arg(pc.first),
                         std::move(time) });
        rows = std::max(rows, cols.back().data.size());
        const bool isActive = (pc.second == smry_.get());
        for (int i : sel) {
            const Vec& v = vecs_[i];
            std::vector<float> data;
            try {
                const std::string key = v.key.toStdString();
                if (pc.second->hasKey(key)) data = pc.second->get(key);
                else if (isActive)          data = pc.second->get(v.node);
                else                        continue;   // absent in this case
            } catch (...) { continue; }
            QString h = QStringLiteral("%1 (%2)").arg(v.key, pc.first);
            if (!v.unit.isEmpty()) h += QStringLiteral(" [%1]").arg(v.unit);
            cols.push_back({ h, std::move(data) });
            rows = std::max(rows, cols.back().data.size());
        }
    }
    if (cols.empty()) {
        setStatus(QStringLiteral("no data to export (cases still being written?)"));
        return;
    }

    QFile out(f);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus(QStringLiteral("could not write %1").arg(QDir::toNativeSeparators(f)));
        return;
    }
    QTextStream ts(&out);
    QStringList hdr;
    for (const auto& c : cols) {
        QString h = c.header;
        h.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        hdr << QLatin1Char('"') + h + QLatin1Char('"');
    }
    ts << hdr.join(QLatin1Char(',')) << '\n';
    for (size_t r = 0; r < rows; ++r) {
        QStringList row;
        for (const auto& c : cols)
            row << (r < c.data.size() ? QString::number(c.data[r], 'g', 9)
                                      : QString());
        ts << row.join(QLatin1Char(',')) << '\n';
    }
    out.close();
    setStatus(QStringLiteral("exported %1 column(s) x %2 row(s) to %3")
        .arg(int(cols.size())).arg(qsizetype(rows)).arg(QDir::toNativeSeparators(f)));
}
