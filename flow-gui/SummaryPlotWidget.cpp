/*
  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  SummaryPlotWidget implementation. Part of the opm_flow_windows harness;
  GPL v3+ (see repository LICENSE).
*/
#include "SummaryPlotWidget.h"

#include "FlowLayout.h"

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
#include <QFontMetrics>
#include <QMargins>
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
#include <QMessageBox>
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
#include <memory>
#include <limits>
#include <exception>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Auto-refresh cadence: fast while the plotted files change, then two steps
// down. Six fast ticks of nothing (a minute) is a settled case, twelve is one
// nobody is writing at all.
static constexpr int kFastRefreshMs  = 10000;
static constexpr int kSlowRefreshMs  = 30000;
static constexpr int kQuietRefreshMs = 60000;
static constexpr int kIdleTicksSlow  = 6;
static constexpr int kIdleTicksQuiet = 12;


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
// ---------------------------------------------------------------------------
// Arithmetic over summary vectors: WBP:B-1H - WBHP:B-1H for a drawdown,
// FOPT/FGPT for a ratio, WWCT*100 for a percentage.
//
// A recursive-descent parser over + - * / and brackets, with summary keys and
// numbers as the leaves. It is re-parsed on every use rather than kept as a
// compiled form: the expressions are a line long and the cost next to reading
// a UNSMRY is nothing, and it saves handing ownership of a tree around.
//
// The lexer's treatment of '-' is the part worth spelling out, because a well
// is called B-1H and a difference is also written with a minus. A hyphen is
// part of the name when it sits INSIDE the item of a key with no space around
// it, and is the operator everywhere else. So "WBP:B-1H - WBHP:B-1H" is a
// difference of two wells and "FOPR-FWPR" a difference of two field vectors;
// the only thing that cannot be written without spaces is a difference of two
// hyphenated names, and that is what the error message says.
namespace expr {

struct Node {
    enum Kind { Num, Key, Bin, Neg } kind = Num;
    double  num = 0.0;
    QString key;
    QChar   op;
    std::unique_ptr<Node> a, b;
};
using Ptr = std::unique_ptr<Node>;

class Parser
{
public:
    Parser(const QString& t) : s_(t) {}

    Ptr parse(QString* err)
    {
        skip();
        Ptr r = expression();
        if (!r) { *err = err_; return nullptr; }
        skip();
        if (i_ < s_.size()) {
            *err = QStringLiteral("unexpected '%1' at position %2")
                       .arg(s_.mid(i_, 8)).arg(i_ + 1);
            return nullptr;
        }
        return r;
    }

private:
    void skip() { while (i_ < s_.size() && s_[i_].isSpace()) ++i_; }
    QChar peek() const { return i_ < s_.size() ? s_[i_] : QChar(); }

    static bool nameChar(QChar c) { return c.isLetterOrNumber() || c == u'_'; }
    static bool itemChar(QChar c)
    {
        return nameChar(c) || c == u',' || c == u'.' || c == u'+';
    }

    Ptr expression()
    {
        Ptr a = term();
        if (!a) return nullptr;
        for (;;) {
            skip();
            const QChar c = peek();
            if (c != u'+' && c != u'-') return a;
            ++i_;
            Ptr b = term();
            if (!b) return nullptr;
            auto n = std::make_unique<Node>();
            n->kind = Node::Bin; n->op = c;
            n->a = std::move(a); n->b = std::move(b);
            a = std::move(n);
        }
    }

    Ptr term()
    {
        Ptr a = unary();
        if (!a) return nullptr;
        for (;;) {
            skip();
            const QChar c = peek();
            if (c != u'*' && c != u'/') return a;
            ++i_;
            Ptr b = unary();
            if (!b) return nullptr;
            auto n = std::make_unique<Node>();
            n->kind = Node::Bin; n->op = c;
            n->a = std::move(a); n->b = std::move(b);
            a = std::move(n);
        }
    }

    Ptr unary()
    {
        skip();
        if (peek() == u'-') {
            ++i_;
            Ptr a = unary();
            if (!a) return nullptr;
            auto n = std::make_unique<Node>();
            n->kind = Node::Neg; n->a = std::move(a);
            return n;
        }
        return primary();
    }

    Ptr primary()
    {
        skip();
        if (i_ >= s_.size()) { err_ = QStringLiteral("the expression ends early"); return nullptr; }
        const QChar c = s_[i_];
        if (c == u'(') {
            ++i_;
            Ptr a = expression();
            if (!a) return nullptr;
            skip();
            if (peek() != u')') { err_ = QStringLiteral("a '(' is never closed"); return nullptr; }
            ++i_;
            return a;
        }
        if (c.isDigit() || c == u'.') return number();
        if (c.isLetter()) return key();
        err_ = QStringLiteral("'%1' is not a number, a summary key or a bracket").arg(c);
        return nullptr;
    }

    Ptr number()
    {
        const int start = i_;
        while (i_ < s_.size() && (s_[i_].isDigit() || s_[i_] == u'.')) ++i_;
        if (i_ < s_.size() && (s_[i_] == u'e' || s_[i_] == u'E')) {
            const int save = i_;
            ++i_;
            if (i_ < s_.size() && (s_[i_] == u'+' || s_[i_] == u'-')) ++i_;
            if (i_ < s_.size() && s_[i_].isDigit()) { while (i_ < s_.size() && s_[i_].isDigit()) ++i_; }
            else i_ = save;
        }
        bool ok = false;
        const double d = s_.mid(start, i_ - start).toDouble(&ok);
        if (!ok) { err_ = QStringLiteral("'%1' is not a number").arg(s_.mid(start, i_ - start)); return nullptr; }
        auto n = std::make_unique<Node>();
        n->kind = Node::Num; n->num = d;
        return n;
    }

    Ptr key()
    {
        const int start = i_;
        while (i_ < s_.size() && nameChar(s_[i_])) ++i_;
        if (i_ < s_.size() && s_[i_] == u':') {
            ++i_;
            // The item. A hyphen belongs to the name only when a name character
            // follows it, so a trailing "B-1H- WBHP" ends the item at the H.
            while (i_ < s_.size()
                   && (itemChar(s_[i_])
                       || (s_[i_] == u'-' && i_ + 1 < s_.size() && nameChar(s_[i_ + 1]))))
                ++i_;
        }
        auto n = std::make_unique<Node>();
        n->kind = Node::Key;
        n->key  = s_.mid(start, i_ - start).toUpper();
        return n;
    }

    const QString s_;
    int     i_ = 0;
    QString err_ = QStringLiteral("the expression is empty");
};

inline Ptr parse(const QString& text, QString* err)
{
    QString local;
    return Parser(text).parse(err ? err : &local);
}

inline void keysIn(const Node& n, QStringList& out)
{
    switch (n.kind) {
        case Node::Key: if (!out.contains(n.key)) out << n.key; break;
        case Node::Neg: keysIn(*n.a, out); break;
        case Node::Bin: keysIn(*n.a, out); keysIn(*n.b, out); break;
        default: break;
    }
}

// Elementwise over n samples; a number broadcasts.
inline double at(const Node& n, const QHash<QString, const std::vector<float>*>& data,
                 std::size_t k)
{
    switch (n.kind) {
        case Node::Num: return n.num;
        case Node::Key: {
            const auto it = data.constFind(n.key);
            return it != data.constEnd() && k < (*it)->size() ? double((**it)[k]) : 0.0;
        }
        case Node::Neg: return -at(*n.a, data, k);
        case Node::Bin: {
            const double x = at(*n.a, data, k), y = at(*n.b, data, k);
            switch (n.op.unicode()) {
                case u'+': return x + y;
                case u'-': return x - y;
                case u'*': return x * y;
                // A division by zero would put an inf on the plot and take the
                // axis with it; it reads better as a gap in the curve.
                default:   return y == 0.0 ? std::numeric_limits<double>::quiet_NaN() : x / y;
            }
        }
    }
    return 0.0;
}

// What the result is measured in, as far as it can be worked out. A difference
// of like things keeps their unit; a ratio of like things has none; anything
// else is written out as the combination it is.
inline QString unitOf(const Node& n, const std::function<QString(const QString&)>& unit)
{
    switch (n.kind) {
        case Node::Num: return {};
        case Node::Key: return unit(n.key);
        case Node::Neg: return unitOf(*n.a, unit);
        case Node::Bin: {
            const QString x = unitOf(*n.a, unit), y = unitOf(*n.b, unit);
            if (n.op == u'+' || n.op == u'-') return x == y ? x : QString();
            if (n.op == u'*') {
                if (x.isEmpty()) return y;
                if (y.isEmpty()) return x;
                return x + QLatin1Char('*') + y;
            }
            if (x == y) return {};                       // a ratio of like things
            if (y.isEmpty()) return x;
            if (x.isEmpty()) return QStringLiteral("1/") + y;
            return x + QLatin1Char('/') + y;
        }
    }
    return {};
}

} // namespace expr

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

// Pick a tick-label format from the magnitude the axis actually spans. Fixed
// notation reads best while the digits stay few; outside that band it fails at
// both ends. Large, it runs away with the margin. Small, it collapses into a
// row of zeros - a rate around 6e-8 labels as "0.00000060", where every tick
// looks alike and the exponent, the one thing worth reading, is left for the
// reader to count. Scientific is the honest form there.
void applyTickFormat(QValueAxis* a)
{
    if (!a) return;
    const double m = std::max(std::fabs(a->min()), std::fabs(a->max()));
    if      (m == 0.0)  a->setLabelFormat(QStringLiteral("%.0f"));
    else if (m >= 1e7)  a->setLabelFormat(QStringLiteral("%.3g"));
    else if (m >= 1000) a->setLabelFormat(QStringLiteral("%.0f"));
    else if (m < 1e-3)  a->setLabelFormat(QStringLiteral("%.1e"));
}

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

// Side of the box the legend sample's marker is drawn in. Qt lays a legend row
// out at max(marker, font) plus its own padding, so this - not the font alone -
// is what sets the row height whenever markers are on. plotChart() draws the
// sample with it and placeLegend() sizes the plate from it; they have to agree,
// or the plate comes out shorter than the rows and Qt reflows the entries into
// a second column that the plate then clips.
//
// The legend's own scale factor applies here as well as to the font. Without
// it the 20px floor below is absolute, and since it exceeds the font height
// at any legend scale worth using, it - not the text - is what the row height
// comes out of: turning the scale down then shrank the text inside rows that
// would not move, which is the whole complaint the scale spin is meant to
// answer. Scaled, the sample shrinks with the text it stands beside.
qreal legendMarkerBox(double markerSize, double legendScale)
{
    return std::clamp(2.6 * markerSize, 20.0, 30.0) * legendScale;
}

// Width of the curve for case `i` of `n`, tapering from the full width down
// to half it. Cases are drawn in order, so where two of them agree the later
// one covers the earlier exactly and the plot cannot say whether the runs
// match or whether one is missing - the reading that matters most when the
// point of the figure is a comparison. Drawn a little thinner each time, the
// earlier cases show as a ribbon around the later ones: agreement reads as a
// band of every colour, and a divergence still separates cleanly.
//
// The taper is spread over however many cases there are rather than stepped
// by a fixed amount, so two runs differ as clearly as six do and the last is
// never thinned away to nothing.
double caseLineWidth(double base, int i, int n)
{
    if (n < 2 || i <= 0) return base;
    const double t = double(std::min(i, n - 1)) / double(n - 1);   // 0 .. 1
    return std::max(base * (1.0 - 0.5 * t), 0.9);
}

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

    // A stem does not always mean the same thing in every scope, and the
    // table below is keyed by stem alone: PR is the average reservoir
    // pressure of a field or a region, but a GROUP's PR is its nodal
    // pressure and a BLOCK's is that block's, so answering from the stem
    // states something false with the same confidence as something true.
    // Anything genuinely scope-dependent is answered here or not at all -
    // a keyword shown bare is honest, a wrong definition is not.
    {
        static const QHash<QString, QString> byScope = {
            {QStringLiteral("G|PR"),  QStringLiteral("Group Nodal Pressure")},
            {QStringLiteral("N|PR"),  QStringLiteral("Node Pressure")},
            {QStringLiteral("B|PR"),  QStringLiteral("Block Pressure")},
            {QStringLiteral("C|PR"),  QStringLiteral("Connection Pressure")},
        };
        static const QSet<QString> scopeDependent = { QStringLiteral("PR") };

        QChar scope;
        if (hasScopeLetter(cat) && keyword.size() > 1) scope = keyword.at(0);
        if (!scope.isNull()) {
            const auto sit = byScope.constFind(QString(scope) + QLatin1Char('|') + body);
            if (sit != byScope.constEnd()) return sit.value();
            // Scope-dependent and not listed for this scope: say nothing
            // rather than fall through to another scope's meaning.
            if (scopeDependent.contains(body) && scope != QLatin1Char('F')
                && scope != QLatin1Char('R'))
                return QString();
        }
    }

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
    // Wrapping, not one fixed line: this row holds enough controls that as a
    // QHBoxLayout it set a minimum width no laptop panel could satisfy, which
    // left the whole window unable to fit - or be maximised on - such a
    // screen. Given the width it still comes out as a single row.
    {
        FlowLayout* row = nullptr;
        top->addWidget(FlowLayout::host(&row));
        auto* bbrowse  = new QPushButton(QStringLiteral("Open SMSPEC..."));
        bbrowse->setToolTip(QStringLiteral(
            "open one or more runs; each joins the case list shared with the "
            "3D View and Compare tabs, and the first becomes the active one"));
        auto* brefresh = new QPushButton(QStringLiteral("Refresh"));
        autoRef_ = new QCheckBox(QStringLiteral("auto-refresh"));
        autoRef_->setToolTip(QStringLiteral(
            "re-read the plotted cases as they are written: every 10 s while\n"
            "something is changing, dropping to a minute once nothing is, and\n"
            "not at all while another tab is shown"));
        dateAxis_ = new QCheckBox(QStringLiteral("date axis"));
        dateAxis_->setChecked(true);   // calendar dates by default
        markers_  = new QCheckBox(QStringLiteral("markers"));
        markers_->setToolTip(QStringLiteral("mark the data points on each curve"));
        // Cases marked at the same points stack their markers exactly, so a
        // stretch where the runs agree shows only the case drawn last. Starting
        // each case at a different point spreads them out instead.
        //
        // Off by default, and deliberately so: it buys that legibility by
        // marking a DIFFERENT subset of the samples per case, so a marker no
        // longer means "every case has a sample here" and reading a value off
        // one is reading that case only. When the question is what a curve did
        // at a given date, the honest answer is every case marked alike.
        stagger_ = new QCheckBox(QStringLiteral("stagger"));
        stagger_->setToolTip(QStringLiteral(
            "start each case's markers at a different data point, so curves "
            "that agree do not hide each other's markers.\n\n"
            "Needs 'every' above 1 - that is the room the offsets come out of. "
            "Off, every case is marked at the same points, which is what you "
            "want when reading a value at a date off the plot."));
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
        // Axis text has its own knob because it is the part a figure is read
        // THROUGH: on a projector or in a printed column, a tick nobody can
        // read costs the reader the value, not just the polish. It is left out
        // of "scale with plot" for the same reason - see the tick-count
        // comment in plotChart - so this is the only thing that moves it, and
        // it goes well past what a screen needs.
        axisScaleSpin_ = new QDoubleSpinBox;
        axisScaleSpin_->setRange(0.5, 3.0);
        axisScaleSpin_->setSingleStep(0.1);
        axisScaleSpin_->setValue(1.0);
        axisScaleSpin_->setDecimals(1);
        axisScaleSpin_->setPrefix(QStringLiteral("x"));
        axisScaleSpin_->setToolTip(QStringLiteral(
            "size of the tick numbers and axis titles, as a factor of the "
            "normal one.\n\n"
            "Unlike the curves, axis text does NOT follow the size of its "
            "subplot - a label is the last thing that should shrink - so turn "
            "this up for a slide or a figure that will be printed small."));
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
        row->addWidget(stagger_);
        // Which dimension colour separates. Comparing runs of one deck, two
        // curves of the same quantity are the pair being read against each
        // other, and giving them one colour is what makes them hard to tell
        // apart where they run close - so which dimension deserves the colour
        // is the plot's question, not something to guess once.
        colourByBox_ = new QComboBox;
        colourByBox_->addItem(QStringLiteral("Colour: auto"),      0);
        colourByBox_->addItem(QStringLiteral("Colour: by vector"), 1);
        colourByBox_->addItem(QStringLiteral("Colour: by case"),   2);
        colourByBox_->setToolTip(QStringLiteral(
            "what the curve colour separates. Auto keys the vector when several "
            "are plotted and the case when only one is; the other dimension "
            "takes the dash pattern and the marker shape either way"));
        row->addWidget(colourByBox_);
        row->addWidget(new QLabel(QStringLiteral("Axis:")));
        row->addWidget(axisScaleSpin_);
        row->addWidget(new QLabel(QStringLiteral("Legend:")));
        row->addWidget(legendScaleSpin_);
        row->addWidget(autoScale_);
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
        // no separate "Layout:" label - the button already reads
        // "Layout: 2 x 2", and a lone label can wrap away from its control
        row->addWidget(layoutBtn_);
        row->addWidget(bzoom);
        row->addWidget(bpng);
        row->addWidget(bcsv);


        connect(bbrowse,  &QPushButton::clicked, this, [this] { browseCase(); });
        connect(brefresh, &QPushButton::clicked, this, [this] { reload(true); });
        connect(dateAxis_, &QCheckBox::toggled, this, [this](bool) { replot(); });
        // Staggering needs markers to be on, and needs "every" to be above 1 -
        // with every sample marked there is no gap for an offset to move into.
        // Greyed out rather than silently doing nothing, so the box says which
        // of the two is missing through its tooltip.
        auto syncStagger = [this] {
            if (!stagger_) return;
            stagger_->setEnabled(markers_ && markers_->isChecked() &&
                                 markerEverySpin_ && markerEverySpin_->value() > 1);
        };
        connect(markers_,  &QCheckBox::toggled, this,
                [this, syncStagger](bool) { syncStagger(); replot(); });
        connect(stagger_,  &QCheckBox::toggled, this, [this](bool) { replot(); });
        connect(lineWidthSpin_, &QDoubleSpinBox::valueChanged, this, [this](double) { replot(); });
        connect(markerSizeSpin_, &QDoubleSpinBox::valueChanged, this, [this](double) { replot(); });
        connect(markerEverySpin_, &QSpinBox::valueChanged, this,
                [this, syncStagger](int) { syncStagger(); replot(); });
        syncStagger();
        connect(colourByBox_, &QComboBox::currentIndexChanged, this, [this](int) { replot(); });
        connect(axisScaleSpin_,   &QDoubleSpinBox::valueChanged, this, [this](double) { replot(); });
        connect(legendScaleSpin_, &QDoubleSpinBox::valueChanged, this, [this](double) { replot(); });
        connect(autoScale_, &QCheckBox::toggled, this, [this](bool) { replot(); });
        connect(bzoom, &QToolButton::clicked, this, [this] { resetZoom(false); });
        connect(bpng,  &QPushButton::clicked, this, [this] { savePng(); });
        connect(bcsv,  &QPushButton::clicked, this, [this] { saveCsv(); });
        timer_ = new QTimer(this);
        timer_->setInterval(kFastRefreshMs);
        connect(timer_, &QTimer::timeout, this, [this] { reload(true); });
        connect(autoRef_, &QCheckBox::toggled, this, [this](bool) {
            idleTicks_ = 0;          // a fresh tick-off deserves a fast look
            syncRefreshTimer();
        });
        // On by default: a plot of a run in progress that silently stops
        // updating is the more surprising behaviour, and re-reading a summary
        // file is cheap next to what the simulation is doing. Set after the
        // connection above so it starts the timer rather than only ticking the
        // box. A restored session applies its own saved choice afterwards.
        autoRef_->setChecked(true);
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

        // Filtering narrows the list; it deliberately does not plot, because a
        // filter is also how you go looking for one vector among thousands.
        // But a pattern like W*PR is usually meant as "these are the curves I
        // want", and going on to click each leaf is the boring half of that -
        // so offer the whole listed set in one press.
        // Arithmetic over the vectors, for the quantities a run does not write
        // but everyone wants: a drawdown is WBP minus WBHP and nothing in the
        // SMSPEC is going to say so.
        row->addWidget(new QLabel(QStringLiteral("f(x):")));
        exprBox_ = new QComboBox;
        exprBox_->setEditable(true);
        exprBox_->setInsertPolicy(QComboBox::NoInsert);
        exprBox_->setMinimumWidth(230);
        exprBox_->lineEdit()->setPlaceholderText(
            QStringLiteral("e.g.  WBP:B-1H - WBHP:B-1H"));
        exprBox_->setToolTip(QStringLiteral(
            "plot arithmetic over summary vectors: + - * / and brackets, with "
            "keys and numbers as the terms.\n"
            "  WBP:B-1H - WBHP:B-1H      drawdown\n"
            "  FOPT/FGPT                 a ratio\n"
            "  WWCT:C-2H*100             as a percentage\n\n"
            "A '-' inside a name belongs to the name, so put spaces around the "
            "operator when both sides are hyphenated."));
        row->addWidget(exprBox_);
        auto* exprAdd = new QPushButton(QStringLiteral("Add"));
        exprAdd->setToolTip(QStringLiteral("plot this expression on the focused subplot"));
        row->addWidget(exprAdd);
        auto* exprDel = new QPushButton(QStringLiteral("Drop"));
        exprDel->setToolTip(QStringLiteral("forget the expression shown in the box"));
        row->addWidget(exprDel);
        connect(exprAdd, &QPushButton::clicked, this, [this] { addExpression(); });
        connect(exprDel, &QPushButton::clicked, this, [this] { removeExpression(); });
        connect(exprBox_->lineEdit(), &QLineEdit::returnPressed,
                this, [this] { addExpression(); });

        auto* plotAll = new QPushButton(QStringLiteral("Plot all listed"));
        plotAll->setToolTip(QStringLiteral(
            "select every vector the list currently shows, so the filter's "
            "matches are plotted in one go"));
        row->addWidget(plotAll);
        connect(plotAll, &QPushButton::clicked, this, [this] {
            QList<QTreeWidgetItem*> leaves;
            for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
                auto* g = tree_->topLevelItem(i);
                if (g->childCount() == 0) { leaves << g; continue; }
                for (int j = 0; j < g->childCount(); ++j) leaves << g->child(j);
            }
            if (leaves.isEmpty()) {
                setStatus(QStringLiteral("nothing to plot: the filter matches no vector"));
                return;
            }
            // A wide pattern over a big case can list thousands of vectors, and
            // each is a curve per checked case. Say what that would mean before
            // spending it, rather than appearing to hang.
            int cases = 0;
            for (int i = 0; i < caseList_->count(); ++i)
                if (caseList_->item(i)->checkState() == Qt::Checked) ++cases;
            const int curves = leaves.size() * std::max(1, cases);
            if (curves > 60 &&
                QMessageBox::question(this, QStringLiteral("Summary plots"),
                    QStringLiteral("Plot %1 vectors across %2 case(s) - %3 curves?\n"
                                   "Narrow the filter for fewer.")
                        .arg(leaves.size()).arg(std::max(1, cases)).arg(curves))
                    != QMessageBox::Yes)
                return;
            // Select in bulk with the tree quiet, then replot once.
            syncingTree_ = true;
            tree_->clearSelection();
            for (auto* it : std::as_const(leaves)) {
                if (it->parent()) it->parent()->setExpanded(true);
                it->setSelected(true);
            }
            syncingTree_ = false;
            applyTreeSelection();
        });
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
        bremove->setToolTip(QStringLiteral(
            "remove the selected cases from the list (Ctrl or Shift to pick "
            "several); the files themselves are untouched"));
        crow->addWidget(bremove);
        ll->addLayout(crow);

        caseList_ = new QListWidget;
        caseList_->setMinimumHeight(64);
        // Several at once, because removing cases is the thing people do in
        // bulk after a batch of runs. The ACTIVE case - the one whose vectors
        // fill the tree - stays the current item, not the selection, so
        // picking a range to delete does not silently swap what is listed.
        caseList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
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
        chartGrid_->setSpacing(1);
        ensureCharts(1);
        chartGrid_->addWidget(chartViews_[0], 0, 0);
        split->addWidget(chartArea_);
        split->setStretchFactor(0, 0);
        split->setStretchFactor(1, 1);
        split->setSizes({ 320, 680 });
        top->addWidget(split, 1);

        connect(tree_, &QTreeWidget::itemSelectionChanged, this,
                &SummaryPlotWidget::applyTreeSelection);
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
    if (!exprs_.isEmpty()) {
        QJsonArray ex;
        for (const QString& e : exprs_) ex.append(e);
        o[QStringLiteral("expressions")] = ex;
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
    o[QStringLiteral("markerStagger")] = stagger_ && stagger_->isChecked();
    o[QStringLiteral("autoRefresh")] = autoRef_  && autoRef_->isChecked();
    if (autoScale_)       o[QStringLiteral("autoScale")]   = autoScale_->isChecked();
    if (axisScaleSpin_)   o[QStringLiteral("axisScale")]   = axisScaleSpin_->value();
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
    if (stagger_  && has("markerStagger"))
        stagger_->setChecked(val("markerStagger").toBool(false));
    if (autoScale_       && has("autoScale"))   autoScale_->setChecked(val("autoScale").toBool(true));
    if (axisScaleSpin_   && has("axisScale"))   axisScaleSpin_->setValue(val("axisScale").toDouble(1.0));
    if (legendScaleSpin_ && has("legendScale")) legendScaleSpin_->setValue(val("legendScale").toDouble(1.0));
    if (lineWidthSpin_   && has("lineWidth"))   lineWidthSpin_->setValue(val("lineWidth").toDouble(2.0));
    if (markerSizeSpin_  && has("markerSize"))  markerSizeSpin_->setValue(val("markerSize").toDouble(7.5));
    if (markerEverySpin_ && has("markerEvery")) markerEverySpin_->setValue(val("markerEvery").toInt(1));
    if (legendBox_ && has("legend")) {
        const int i = val("legend").toInt(0);
        if (i >= 0 && i < legendBox_->count()) legendBox_->setCurrentIndex(i);
    }

    // Expressions before the cases, so the load that follows already builds
    // their Vecs and the restored subplot selections find them.
    if (has("expressions")) {
        exprs_.clear();
        const QJsonArray ex = val("expressions").toArray();
        for (const auto& e : ex) {
            const QString t = e.toString().trimmed();
            if (!t.isEmpty() && !exprs_.contains(t)) exprs_ << t;
        }
        syncExprBox();
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
    // Coming back to the tab: catch up on whatever was written while it was
    // away (cheap - an unchanged case costs a few stat calls), then resume.
    else if (autoRef_ && autoRef_->isChecked()) { idleTicks_ = 0; reload(true); }
    syncRefreshTimer();
}

void SummaryPlotWidget::hideEvent(QHideEvent* ev)
{
    QWidget::hideEvent(ev);
    syncRefreshTimer();        // another tab is up: stop polling entirely
}

// When the refresh timer should run, and how often. Three things make an
// auto-refresh tick worth nothing: no case to follow, a case nobody is
// writing, and a tab nobody is looking at - and the last two are the normal
// state of a plot, which is studied far longer than it takes to produce.
// So: 10 s while the files are changing, backing off to a minute once they
// are not, and stopped while this tab is not the one on screen. Any changed
// byte snaps it back, so a run started here or in a terminal is picked up
// within a tick either way.
void SummaryPlotWidget::syncRefreshTimer()
{
    if (!timer_ || !autoRef_) return;
    if (!autoRef_->isChecked() || !isVisible()) { timer_->stop(); return; }
    const int want = idleTicks_ < kIdleTicksSlow  ? kFastRefreshMs
                   : idleTicks_ < kIdleTicksQuiet ? kSlowRefreshMs
                                                  : kQuietRefreshMs;
    if (timer_->interval() != want) timer_->setInterval(want);   // restarts it
    if (!timer_->isActive()) timer_->start();
}

void SummaryPlotWidget::removeCurrentCase()
{
    // Bottom up, so the rows still to be taken keep the indices they had.
    QList<int> rows;
    for (auto* it : caseList_->selectedItems()) rows << caseList_->row(it);
    if (rows.isEmpty() && caseList_->currentRow() >= 0) rows << caseList_->currentRow();
    if (rows.isEmpty()) return;
    std::sort(rows.begin(), rows.end(), std::greater<int>());

    QStringList gone;
    for (int row : std::as_const(rows)) {
        QListWidgetItem* it = caseList_->takeItem(row);   // fires currentItemChanged
        if (!it) continue;
        const QString path = it->data(Qt::UserRole).toString();
        others_.erase(path);
        delete it;
        gone << path;
    }
    // Told afterwards rather than one at a time during: the mirrors in the
    // other tabs rebuild on each of these, and there is no reason to make them
    // do it half way through a list that is still shrinking.
    for (const QString& path : std::as_const(gone)) emit caseRemoved(path);

    if (caseList_->count() == 0) { clearActiveCase(); return; }
    relabelCases();    // a case left alone with its name drops the tag again
    replot();          // plotted set may have changed even if active did not
    if (gone.size() > 1)
        setStatus(QStringLiteral("removed %1 cases").arg(gone.size()));
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
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Open summary specifications"),
        flowgui::startDir(QStringLiteral("smspec"), activePath()),
        QStringLiteral("Summary spec (*.SMSPEC);;All files (*)"));
    if (files.isEmpty()) return;
    flowgui::rememberDir(QStringLiteral("smspec"), files.first());
    for (const QString& f : files)
        addCase(QFileInfo(f).completeBaseName(), f);
    // The first of them, not the last: opening a folder full of runs should
    // land somewhere predictable, and the top of the list is what was asked
    // for first.
    activateCase(files.first());
}

// ---------------------------------------------------------------------------
QString SummaryPlotWidget::plottedStamp() const
{
    // Everything a reload would read: the active case, plus the checked ones
    // it clears so replot() reopens them. mtime and size are enough to notice
    // a run appending to a summary, and cost a stat() each.
    QStringList paths(activePath());
    for (int i = 0; i < caseList_->count(); ++i) {
        const auto* it = caseList_->item(i);
        if (it->checkState() == Qt::Checked)
            paths << it->data(Qt::UserRole).toString();
    }
    QString stamp;
    for (const QString& p : std::as_const(paths)) {
        if (p.isEmpty()) continue;
        QString base = p;
        if (base.endsWith(QStringLiteral(".SMSPEC"), Qt::CaseInsensitive)) base.chop(7);
        // the spec and whichever file carries the samples
        for (const auto& suffix : { QStringLiteral(".SMSPEC"),
                                    QStringLiteral(".UNSMRY"),
                                    QStringLiteral(".ESMRY") }) {
            const QFileInfo fi(base + suffix);
            if (!fi.exists()) continue;
            stamp += QStringLiteral("%1|%2|%3;").arg(fi.fileName())
                         .arg(fi.lastModified().toMSecsSinceEpoch())
                         .arg(fi.size());
        }
    }
    return stamp;
}

void SummaryPlotWidget::reload(bool keepSelection)
{
    const QString path = activePath();
    if (path.isEmpty()) return;
    if (!QFileInfo::exists(path)) {
        setStatus(QStringLiteral("waiting for %1 ...").arg(path));
        return;
    }

    // A refresh of cases nothing has written to since they were read has
    // nothing to give: re-opening and re-parsing them, rebuilding the vector
    // tree and replotting is pure cost, and auto-refresh would pay it every
    // 10 s for as long as the window is open. Finished runs are the common
    // case - a plot is looked at far longer than it takes to produce.
    const QString stamp = plottedStamp();
    if (keepSelection && smry_ && path == smryPath_
        && !stamp.isEmpty() && stamp == loadedStamp_) {
        ++idleTicks_;              // nothing is being written; poll less often
        syncRefreshTimer();
        return;
    }
    // Past the guard: either the files changed or this is a deliberate reload,
    // and both want the fast pace back. Set here rather than after the read
    // below, because a read that fails is not evidence of an idle case - a
    // half-written file is unreadable for a moment precisely while a run is
    // writing it, which is when following it matters most.
    idleTicks_ = 0;
    syncRefreshTimer();

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
    //
    // One node per KEY, not per record. A deck that names a keyword twice in
    // SUMMARY - WBHP once for every well, and again well by well - makes the
    // simulator write a vector for each declaration, and the SMSPEC tells them
    // apart only by a NUMS that the well category does not carry into its
    // name. Both are the same quantity for the same well, so the tree showed
    // PROD01 twice with nothing to choose between the two: selection stores
    // the key, the plot looks the data up by the key, and the index QHash is
    // keyed on it and keeps one of them anyway. The second row was unreachable
    // - clicking either lit up both - so drop it here rather than leave it to
    // puzzle over, and say how many went.
    vecs_.clear();
    QSet<QString> seenKeys;
    int duplicateNodes = 0;
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
        if (seenKeys.contains(v.key)) { ++duplicateNodes; continue; }
        seenKeys.insert(v.key);
        vecs_.push_back(v);
    }
    appendDerivedVecs();

    rebuildFilters();
    rebuildTree(reselect);
    // The stamp taken BEFORE the read, deliberately: a run appending while we
    // were reading leaves the file newer than this, so the next refresh sees a
    // difference and re-reads. Stamping afterwards would record data we never
    // parsed and skip it for good.
    loadedStamp_ = stamp;

    QString loaded = QStringLiteral("%1: %2 vectors, %3 timesteps")
                         .arg(QFileInfo(path).completeBaseName())
                         .arg(vecs_.size())
                         .arg(int(smry_->numberOfTimeSteps()));
    if (duplicateNodes > 0)
        loaded += QStringLiteral("  (%1 declared twice in SUMMARY, shown once)")
                      .arg(duplicateNodes);
    setStatus(loaded);
}

// Take what is in the box, check it against the active case, and plot it.
void SummaryPlotWidget::addExpression()
{
    const QString text = exprBox_->currentText().trimmed();
    if (text.isEmpty()) return;
    if (!smry_) { setStatus(QStringLiteral("open a case first")); return; }

    QString err;
    const expr::Ptr e = expr::parse(text, &err);
    if (!e) { setStatus(QStringLiteral("%1: %2").arg(text, err)); return; }

    // Checked against the case now rather than left to fail quietly at plot
    // time as a curve that never appears. A '-' that was meant as an operator
    // and got swallowed by a well name shows up here as an unknown key, so say
    // what to do about it.
    QStringList keys;
    expr::keysIn(*e, keys);
    if (keys.isEmpty()) {
        setStatus(QStringLiteral("%1: names no summary vector").arg(text));
        return;
    }
    QStringList missing;
    for (const QString& k : std::as_const(keys)) {
        bool have = false;
        try { have = smry_->hasKey(k.toStdString()); } catch (...) {}
        if (!have) missing << k;
    }
    if (!missing.isEmpty()) {
        QString why = QStringLiteral("%1: %2 not in this case")
                          .arg(text, missing.join(QStringLiteral(", ")));
        for (const QString& m : std::as_const(missing))
            if (m.contains(QLatin1Char('-'))) {
                why += QStringLiteral("  -  put spaces around the '-' if it was "
                                      "meant as a subtraction");
                break;
            }
        setStatus(why);
        return;
    }

    if (!exprs_.contains(text)) exprs_ << text;
    syncExprBox();
    refreshDerived();
    if (focusChart_ >= 0 && focusChart_ < chartSel_.size()
        && !chartSel_[focusChart_].contains(text)) {
        chartSel_[focusChart_] << text;
    }
    rebuildTree(chartSel_.value(focusChart_));
    replot();
    setStatus(QStringLiteral("plotting %1%2").arg(text,
        vecs_.isEmpty() || vecs_.last().unit.isEmpty()
            ? QString() : QStringLiteral("  [%1]").arg(vecs_.last().unit)));
}

void SummaryPlotWidget::removeExpression()
{
    const QString text = exprBox_->currentText().trimmed();
    if (text.isEmpty() || !exprs_.removeAll(text)) return;
    for (auto& sel : chartSel_) sel.removeAll(text);
    syncExprBox();
    refreshDerived();
    rebuildTree(chartSel_.value(focusChart_));
    replot();
}

void SummaryPlotWidget::syncExprBox()
{
    const QString text = exprBox_->currentText();
    QSignalBlocker block(exprBox_);
    exprBox_->clear();
    exprBox_->addItems(exprs_);
    exprBox_->setCurrentText(text);
}

bool SummaryPlotWidget::seriesData(const Vec& v, Opm::EclIO::ESmry* smry,
                                   bool isActive, std::vector<float>& out) const
{
    out.clear();
    if (!smry) return false;

    if (v.expr.isEmpty()) {
        try {
            const std::string key = v.key.toStdString();
            if (smry->hasKey(key)) out = smry->get(key);
            else if (isActive)     out = smry->get(v.node);
            else                   return false;   // vector absent in this case
        } catch (...) { return false; }
        return !out.empty();
    }

    const expr::Ptr e = expr::parse(v.expr, nullptr);
    if (!e) return false;
    QStringList keys;
    expr::keysIn(*e, keys);
    if (keys.isEmpty()) return false;              // arithmetic on no vector

    // get() hands back a reference into the reader's own cache, which outlives
    // this call, so the pointers stay good while the samples are read off.
    QHash<QString, const std::vector<float>*> data;
    std::size_t n = std::numeric_limits<std::size_t>::max();
    for (const QString& k : std::as_const(keys)) {
        try {
            const std::string key = k.toStdString();
            if (!smry->hasKey(key)) return false;
            const std::vector<float>& d = smry->get(key);
            data.insert(k, &d);
            n = std::min(n, d.size());
        } catch (...) { return false; }
    }
    if (n == 0 || n == std::numeric_limits<std::size_t>::max()) return false;

    out.resize(n);
    for (std::size_t k = 0; k < n; ++k) out[k] = float(expr::at(*e, data, k));
    return true;
}

// Drop the derived Vecs and build them again. Adding an expression must not go
// through reload(): that re-reads the case, and skips the work entirely when
// the file has not changed since the last read - which it has not, since the
// expression is the only thing that moved.
void SummaryPlotWidget::refreshDerived()
{
    for (int i = vecs_.size() - 1; i >= 0; --i)
        if (!vecs_[i].expr.isEmpty()) vecs_.remove(i);
    appendDerivedVecs();
}

void SummaryPlotWidget::appendDerivedVecs()
{
    if (!smry_) return;
    for (const QString& text : std::as_const(exprs_)) {
        const expr::Ptr e = expr::parse(text, nullptr);
        if (!e) continue;
        Vec v;
        v.expr    = text;
        v.key     = text;
        v.keyword = QStringLiteral("expression");
        v.item    = text;
        v.itemMain = text;
        v.unit = expr::unitOf(*e, [this](const QString& k) -> QString {
            try { return QString::fromStdString(smry_->get_unit(k.toStdString())).trimmed(); }
            catch (...) { return {}; }
        });
        vecs_.push_back(v);
    }
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
    // A pattern naming no item is about the KEYWORD: W*PR means WOPR, WWPR,
    // WVPR - not "a key that ends in PR", which nothing does once the item is
    // on it (WOPR:D-1H). So a term without a ':' is matched against the
    // keyword as well as the whole key; one with a ':' stays a key pattern,
    // which is what WBHP:B* needs.
    QVector<QRegularExpression> wilds;      // matched against the full key
    QVector<QRegularExpression> wildsKw;    // ... and these against the keyword
    QStringList substrings;
    for (const QString& t : search.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString term = t.trimmed();
        if (term.isEmpty()) continue;
        if (term.contains(QLatin1Char('*')) || term.contains(QLatin1Char('?'))) {
            const auto re = QRegularExpression::fromWildcard(term, Qt::CaseInsensitive);
            wilds.push_back(re);
            if (!term.contains(QLatin1Char(':'))) wildsKw.push_back(re);
        } else {
            substrings << term;
        }
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
    QTreeWidgetItem* firstSelected = nullptr;   // to scroll to on a fresh load
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
        // An expression belongs to no category, type or item, so those boxes
        // would hide it whatever they were set to. Only the search narrows it.
        if (v.expr.isEmpty()) {
            if (selCat  >= 0 && int(v.cat)  != selCat)  continue;
            if (selType >= 0 && int(v.type) != selType) continue;
            if (!selItem.isEmpty() && v.itemMain != selItem) continue;
            if (!selSub.isEmpty()  && v.itemSub  != selSub)  continue;
        }
        if (!wilds.isEmpty() || !substrings.isEmpty()) {
            bool hit = false;
            for (const auto& re : wilds)
                if (re.match(v.key).hasMatch()) { hit = true; break; }
            if (!hit)
                for (const auto& re : wildsKw)
                    if (re.match(v.keyword).hasMatch()) { hit = true; break; }
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
            bool holdsSelected = false;
            for (int idx : idxs) {
                const Vec& v = vecs_[idx];
                auto* leaf = new QTreeWidgetItem(grp);
                leaf->setText(0, v.item.isEmpty() ? kw : v.item);
                leaf->setData(0, RoleVecIndex, v.key);
                leaf->setData(0, RoleVecIndex + 1, idx);
                if (keep.contains(v.key)) { leaf->setSelected(true); holdsSelected = true; }
            }
            // Open a group that holds something plotted, as well as one the
            // user had open. Restoring a session there was nothing to restore
            // from, so every group came up collapsed and the tree said nothing
            // about what the chart was already showing.
            // Only on a FRESH tree: on a rebuild the snapshot above is the
            // user's own doing, and a group they collapsed must not spring
            // open again every time a running case refreshes.
            if ((!hadItems && holdsSelected) || expanded.contains(kwLabel))
                grp->setExpanded(true);
            if (holdsSelected && !firstSelected) firstSelected = grp->child(0);
        }
    }
    tree_->blockSignals(false);
    if (hadItems) {
        tree_->verticalScrollBar()->setValue(scrollPos);
    } else if (firstSelected) {
        // Nothing was on screen to preserve, so put what is plotted there:
        // a restored session otherwise opened on the top of a long list with
        // its selection somewhere below the fold.
        tree_->scrollToItem(firstSelected, QAbstractItemView::PositionAtTop);
    }

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
    // No early return for "the layout is already this". The state below is not
    // only a rearrangement - it is what SHOWS the views at all, ensureCharts()
    // building them hidden - and asking for the layout that is already set is
    // exactly what happens on the way up with a single chart saved: the
    // constructor asks for 1x1, which is what the members already say, and so
    // did restoring the session. Skipping the work left chart 0 hidden, the
    // grid holding nothing visible, and the whole plot area collapsed to
    // nothing. Every step here is idempotent, and none of it is hot - this
    // runs when the layout button is used, not on a replot.

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
    // Give the grid its new geometry NOW. Everything drawn on a chart is sized
    // from the view it sits in, and the caller replots as soon as this returns
    // - but Qt would not lay the views out until control went back to the event
    // loop, so that replot would read the sizes the OLD grid had. Going 2x2 to
    // 3x3 then left the curves, markers and legend scaled for the larger plot
    // until something else forced a second replot, which is why nudging the
    // legend scale appeared to fix it. The resize filter does not cover this:
    // it watches chartArea_, whose own size does not change when the grid does.
    chartGrid_->activate();
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

void SummaryPlotWidget::expandToSelection()
{
    QTreeWidgetItem* first = nullptr;
    QTreeWidgetItemIterator it(tree_);
    while (*it) {
        if ((*it)->isSelected()) {
            for (auto* p = (*it)->parent(); p; p = p->parent()) p->setExpanded(true);
            if (!first) first = *it;
        }
        ++it;
    }
    if (first) tree_->scrollToItem(first, QAbstractItemView::EnsureVisible);
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
    // A selection nobody can see says nothing: this path mirrors chartSel_
    // without rebuilding the tree, so the groups holding it stay shut - which
    // is what a restored session came up looking like.
    expandToSelection();
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

// What the tree has selected becomes the focused subplot's curve list. Its
// own signal calls this, and so does "Plot all listed" after selecting in
// bulk - one replot for the lot rather than one per item.
void SummaryPlotWidget::applyTreeSelection()
{
    if (syncingTree_) return;          // programmatic reselect, not the user
    QStringList keys;
    for (auto* it : tree_->selectedItems()) {
        const QVariant k = it->data(0, RoleVecIndex);
        if (k.isValid()) keys << k.toString();
    }
    if (focusChart_ >= 0 && focusChart_ < chartSel_.size())
        chartSel_[focusChart_] = keys;
    replot();
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
    // Qt's default 20px all round is padding OUTSIDE the axis labels and
    // titles, so in a grid every gap between two subplots costs two of them -
    // 40px of white before the 1px the layout adds. Half that: it reads as a
    // gap rather than a gutter, and the space goes back to the plots.
    // NB: this is a floor, not a target. The labels and titles are laid out
    // inside these margins, so taking them much below this drops the axes
    // entirely rather than merely crowding them.
    chart->setMargins(QMargins(10, 10, 10, 10));
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

void SummaryPlotWidget::styleAxis(QAbstractAxis* axis) const
{
    if (!axis) return;
    // Tick numbers carry the quantitative content of the figure, so they are
    // sized to be read rather than to stay out of the way: a step up from the
    // old 9pt, and demi-bold, which is what makes a digit hold up against the
    // grid lines behind it without going as heavy as the axis title.
    //
    // Times whatever the Axis box asks for, which is how a figure headed for a
    // slide gets text a room can read. Clamped at both ends: the spin cannot
    // reach either bound on its own, but a session file can carry any number.
    const double as = std::clamp(axisScaleSpin_ ? axisScaleSpin_->value() : 1.0,
                                 0.4, 4.0);
    QFont f = axis->labelsFont();
    f.setPointSizeF(std::clamp(11.0 * as, 4.0, 48.0));
    f.setWeight(QFont::DemiBold);
    axis->setLabelsFont(f);
    QFont t = axis->titleFont();
    t.setPointSizeF(std::clamp(11.0 * as, 4.0, 48.0));
    t.setBold(true);
    axis->setTitleFont(t);
    axis->setLabelsColor(QColor(0x22, 0x26, 0x2b));
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
    // Measure the labels the way Qt will: it lays text out with the integer
    // metrics, which round up per glyph, and elides on the slightest overrun.
    // Measured with the float ones the plate comes out a few pixels short and
    // the longest entry loses its tail to an ellipsis.
    const QFontMetrics fmi(lg->font());
    const auto markers = lg->markers();
    for (auto* mk : markers) {
        // Count the stand-in series carrying the entries, by their NAME - not
        // by marker visibility: once the legend itself is hidden its markers
        // report invisible too, so counting those would latch the legend off
        // for good.
        if (mk->series()->objectName() != kLegendSample) continue;
        ++rows;
        textW = std::max(textW, qreal(fmi.horizontalAdvance(mk->label())));
    }
    if (rows == 0) { lg->setVisible(false); return; }
    // What a row of this legend costs, measured off Qt rather than guessed at.
    // Rendering entries across 4-12pt, with and without a sample marker, and
    // reading back the smallest plate that neither reflows them into a second
    // column nor elides a label:
    //
    //     height  max(marker, font) + 13.5
    //     width   label + max(26 + font, marker + 31)
    //
    // Qt's own size hint is no substitute - it reports a row shorter than the
    // one it then lays out, which is what the plate has to clear.
    //
    // The marker is the term that matters and the one the old +20 a row was
    // standing in for. It stopped covering it once the scale spin took the
    // legend below about x0.8 - hence entries clipped in a small subplot - and
    // being constant it was also why the box stopped shrinking: the sample was
    // pinned at 20px whatever the scale, so the rows were too. Scaled with the
    // legend and sized from the numbers above, the plate tracks the scale down
    // and carries no slack at the top of the range either.
    const qreal fh  = fm.height();
    const qreal box = (markers_ && markers_->isChecked())
        ? legendMarkerBox((markerSizeSpin_ ? markerSizeSpin_->value() : 7.5)
                              * plotScale(chart),
                          legendScaleSpin_ ? legendScaleSpin_->value() : 1.0)
        : 0.0;
    const qreal rowH = std::max(fh, box) + 14.0;
    const qreal entW = textW + std::max(26.0 + fh, box + 31.0);
    const qreal pad  = 4.0;
    // Trim Qt's padding inside the plate too, so the space the box does take
    // is spent on the entries rather than on a border around them.
    lg->setContentsMargins(0, 0, 0, 0);
    QSizeF sz(std::min(entW + pad,        maxW),
              std::min(rows * rowH + pad, maxH));
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
    const double legendSpin = legendScaleSpin_ ? legendScaleSpin_->value() : 1.0;
    {
        const double legendScale = legendSpin * plotScale(chart);
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
    // A vector with no unit - WMCTL's control-mode code, a count, a region
    // number - is not "compatible with everything": its integers share an axis
    // with a rate no more sensibly than bar does. It was being skipped in the
    // scan below instead, so it could never be given an axis at all and was
    // dropped whenever anything with a unit was selected, however free the
    // right axis was. Give it an identity so it competes for an axis like any
    // other unit; only the axis TITLE has to say something friendlier.
    const QString kNoUnit = QStringLiteral("\x01none");   // no real unit collides
    auto unitKey = [&kNoUnit](const QString& u) {
        return u.isEmpty() ? kNoUnit : u;
    };
    QString unitL, unitR;
    bool haveL = false, haveR = false;
    for (int i : sel) {
        const QString u = unitKey(vecs_[i].unit);
        if      (!haveL)               { unitL = u; haveL = true; }
        else if (u != unitL && !haveR) { unitR = u; haveR = true; }
    }
    auto axisFor = [&](const QString& raw) -> int {   // 0 left, 1 right, -1 skip
        const QString u = unitKey(raw);
        if (u == unitL) return 0;
        if (haveR && u == unitR) return 1;
        return -1;
    };

    // An axis labelled "SM3/DAY" says how the numbers were measured but not
    // what they are, leaving the reader to work that out from the legend - and
    // when the legend is off, or the curve is one of a dozen, not at all. Name
    // the quantities that ended up on the axis: they are what the axis is FOR,
    // and there are usually one or two of them however many curves there are,
    // since every well's WBHP is still WBHP. Past three the name would be
    // longer than the axis, so the unit alone has to carry it.
    QStringList kwL, kwR;
    for (int i : sel) {
        const int a = axisFor(vecs_[i].unit);
        if (a < 0) continue;
        QStringList& k = (a == 0) ? kwL : kwR;
        // An expression's keyword is the word "expression", which names
        // nothing; what it is called is what it says.
        const QString name = vecs_[i].expr.isEmpty() ? vecs_[i].keyword
                                                     : vecs_[i].expr;
        if (!k.contains(name)) k << name;
    }
    auto axisTitle = [&kNoUnit](const QString& u, const QStringList& kw) {
        const QString unit = (u == kNoUnit) ? QString() : u;
        const QString name = (!kw.isEmpty() && kw.size() <= 3)
                                 ? kw.join(QStringLiteral(", ")) : QString();
        if (name.isEmpty())
            return unit.isEmpty() ? QStringLiteral("value") : unit;
        return unit.isEmpty() ? name
                              : QStringLiteral("%1 [%2]").arg(name, unit);
    };

    const bool useDates = dateAxis_ && dateAxis_->isChecked();
    const bool showPts  = markers_ && markers_->isChecked();
    const bool staggerPts = showPts && stagger_ && stagger_->isChecked();
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

    // What colour separates. Auto: the CASES when one vector is compared across
    // runs (keying colour to the vector would paint every curve the same), the
    // VECTOR when several are plotted. Either way the dimension colour does not
    // carry takes the dash and the marker shape, so both stay readable - and
    // the box lets that be chosen outright, since which pair a reader is
    // comparing is a property of the plot, not something to guess.
    const int colourMode = colourByBox_ ? colourByBox_->currentData().toInt() : 0;
    const bool colourByCase = (colourMode == 2)
        || (colourMode == 0 && multi && sel.size() == 1);
    // The dash carries the other dimension - except where that one does not
    // vary (a single vector across cases), where keying it to the case keeps
    // the runs apart in a greyscale print too.
    const bool dashByVector  = colourByCase && sel.size() > 1;
    const bool shapeByVector = colourByCase && sel.size() > 1;

    // How many ticks a subplot this size can label. Qt ELIDES a label that
    // does not fit rather than dropping the tick, so in a dense grid a date
    // turns into "2018-1..." unless there are fewer of them. The labels
    // themselves stay at full size - they are the last thing that should
    // shrink.
    // Bigger text needs the same room per label, so the count has to come down
    // as the Axis box goes up - otherwise turning it up to be legible is what
    // makes the dates elide, which is the opposite of the point.
    const int vidx = charts_.indexOf(chart);
    const QSize vsz = (vidx >= 0 && vidx < chartViews_.size())
                          ? chartViews_[vidx]->size() : QSize(900, 600);
    const double axisS = std::clamp(axisScaleSpin_ ? axisScaleSpin_->value() : 1.0,
                                    0.4, 4.0);
    const int xTicks = std::clamp(int(vsz.width()  / ((useDates ? 170 : 130) * axisS)), 2, 6);
    const int yTicks = std::clamp(int(vsz.height() / (90 * axisS)), 2, 6);

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
    ayL->setTitleText(axisTitle(unitL, kwL));
    chart->addAxis(ayL, Qt::AlignLeft);
    QValueAxis* ayR = nullptr;
    if (haveR) {
        ayR = new QValueAxis; ayR->setTitleText(axisTitle(unitR, kwR));
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
            if (!seriesData(v, pc.second, isActive, data)) continue;

            auto* s = new QLineSeries;
            s->setName(multi ? pc.first + QStringLiteral(" | ") + v.key : v.key);
            // Colour keys one dimension, dash and marker shape the other, so
            // no two curves in the plot share every channel.
            QPen pen(kCurveColors[(colourByCase ? ci : si) % kCurveColorCount]);
            // Thinner for each case in turn, so the ones drawn earlier are not
            // simply buried by the ones drawn over them where they agree.
            pen.setWidthF(caseLineWidth(lineW, ci, int(plotCases.size())));
            pen.setStyle(kCaseDashes[(dashByVector ? si : ci) % kCaseDashCount]);
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
                // Markers are on, so the entry shows what the curve shows:
                // the same shape riding on the sample line - the SAME index
                // the curve used, or the legend would describe a marker the
                // plot does not draw. Qt sizes a sample carrying a marker from
                // the series' marker size, which also buys the line a little
                // more length to show its dash in.
                const qreal box  = legendMarkerBox(markerS, legendSpin);
                const qreal size = std::min(markerS, box - 3.0);
                sample->setMarkerSize(box);
                sample->setLightMarker(
                    sampleMarker(kShapes[(shapeByVector ? si : ci) % kShapeCount], box, size,
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
                //
                // Staggered, each case starts a little further along, dividing
                // the gap between marked points among the cases. Curves that
                // agree then alternate their markers rather than stacking them,
                // where before only the case drawn last was visible. Every
                // marker is still a real sample of the case it belongs to; what
                // it stops meaning is that the OTHER cases have one there too,
                // which is why this is asked for rather than assumed.
                const int offset = staggerPts
                    ? (ci * markerEvery / std::max(1, int(plotCases.size()))) % markerEvery
                    : 0;
                const QList<QPointF> pts = s->points();
                QList<QPointF> marks;
                marks.reserve(int(pts.size()) / markerEvery + 1);
                for (int k = offset; k < pts.size(); k += markerEvery)
                    marks.append(pts[k]);

                auto* sc = new QScatterSeries;
                sc->replace(marks);
                sc->setMarkerShape(kShapes[(shapeByVector ? si : ci) % kShapeCount]);
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
            auto* xa = static_cast<QValueAxis*>(ax);
            xa->setRange(xmin, xmax);
            applyTickFormat(xa);      // a days axis runs small on a short case
        }
    }

    // Round tick values ("6 000 000", not "5581102.4"): applyNiceNumbers()
    // widens the range to the next round step and picks a matching tick count.
    auto pad = [yTicks](QValueAxis* a, double lo, double hi) {
        a->setTickCount(yTicks);
        if (hi > lo) a->setRange(lo - 0.05 * (hi - lo), hi + 0.05 * (hi - lo));
        else         a->setRange(lo - 1.0, hi + 1.0);
        a->applyNiceNumbers();
        applyTickFormat(a);
    };
    if (lset) pad(ayL, lmin, lmax);
    if (ayR && rset) pad(ayR, rmin, rmax);

    // Two axes both crossing zero put two different zero lines on one plot,
    // at whatever heights their own ranges happened to land - so a curve
    // touching "0" on the left sits above or below the right axis' zero, and
    // the two gridlines read as data. Give them a common grid instead: keep
    // each axis' own tick step and extend whichever has fewer ticks on one
    // side of zero, so both end up with the same number above and below. Zero
    // lines up, every other gridline lines up with it, and since this only
    // ever extends a range no data is cut off.
    if (lset && ayR && rset && ayL->min() < 0 && ayL->max() > 0
        && ayR->min() < 0 && ayR->max() > 0) {
        auto shape = [](QValueAxis* ax, int& below, int& above, double& step) {
            const int n = std::max(2, ax->tickCount());
            step  = (ax->max() - ax->min()) / (n - 1);
            below = int(std::lround(-ax->min() / step));
            above = int(std::lround( ax->max() / step));
        };
        int lBelow, lAbove, rBelow, rAbove;
        double lStep, rStep;
        shape(ayL, lBelow, lAbove, lStep);
        shape(ayR, rBelow, rAbove, rStep);
        const int below = std::max(lBelow, rBelow);
        const int above = std::max(lAbove, rAbove);
        // Only worth it while the shared grid stays a sensible number of
        // lines; ranges wildly apart in shape would otherwise be stretched
        // into a plot that is mostly empty.
        if (below > 0 && above > 0 && below + above + 1 <= 2 * yTicks + 2) {
            ayL->setRange(-below * lStep, above * lStep);
            ayL->setTickCount(below + above + 1);
            ayR->setRange(-below * rStep, above * rStep);
            ayR->setTickCount(below + above + 1);
            applyTickFormat(ayL);
            applyTickFormat(ayR);
        }
    }
    // The title carries whatever the legend does not.
    //
    // With several cases drawn the legend names every one of them, so a title
    // of "3 cases" spends the most prominent text on the chart repeating, less
    // precisely, what is already spelled out below it - and in a grid it puts
    // the SAME words on every subplot, so the titles stop telling them apart
    // at exactly the moment there is something to tell apart. What the legend
    // does not lead with is the quantity, so that is the title: the friendly
    // name and the well or region, "Bottom Hole Pressure - PROD02".
    //
    // With one case it is the other way round - the legend drops the case name
    // from its entries, having nothing to distinguish - so the title keeps
    // naming the case, which is then the only place it appears.
    const QChar dot(0x00B7);   // middle dot, spelled out: not every source
                               // encoding survives a literal one
    QString shown;
    if (plotCases.size() > 1) {
        // Only what was actually drawn. A vector dropped for want of a third
        // axis must not be counted in "+N more": the title would then claim
        // the plot shows something it does not, which is how "+1 more" came
        // to name a curve that is not there.
        QList<int> drawn;
        for (int i : sel)
            if (i >= 0 && i < vecs_.size() && axisFor(vecs_[i].unit) >= 0)
                drawn << i;
        QStringList parts;
        int named = 0;
        for (int i : drawn) {
            if (named == 2) break;
            const Vec& v = vecs_[i];
            const QString fn = friendlyName(v.keyword, v.cat);
            QString one = fn.isEmpty() ? v.keyword : fn;
            if (!v.item.isEmpty())
                one += QStringLiteral(" ") + dot + QStringLiteral(" ") + v.item;
            parts << one;
            ++named;
        }
        shown = parts.join(QStringLiteral(",   "));
        // Two named and the rest counted: a title is a label, not a list, and
        // past two the legend is the place to read them off.
        const int rest = int(drawn.size()) - named;
        if (rest > 0) shown += QStringLiteral("   +%1 more").arg(rest);
    } else {
        shown = plotCases.front().first;
    }
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
            if (!seriesData(v, pc.second, isActive, data)) continue;
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
