/*
  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  DeckModel implementation. Part of the opm_flow_windows harness;
  GPL v3+ (see repository LICENSE).
*/
#include "DeckModel.h"

#include "GuiPaths.h"

#include <QApplication>
#include <QPageSize>
#include <QPdfWriter>

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QLineEdit>
#include <QPainter>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QThread>
#include <QTimeZone>
#include <QTimer>
#include <QTreeWidget>
#include <QDialog>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <opm/input/eclipse/Deck/Deck.hpp>
#include <opm/input/eclipse/EclipseState/EclipseState.hpp>
#include <opm/input/eclipse/Parser/ErrorGuard.hpp>
#include <opm/input/eclipse/Parser/InputErrorAction.hpp>
#include <opm/input/eclipse/Parser/ParseContext.hpp>
#include <opm/input/eclipse/Parser/Parser.hpp>
#include <opm/input/eclipse/Python/Python.hpp>
#include <opm/input/eclipse/Schedule/Group/Group.hpp>
#include <opm/input/eclipse/Schedule/Network/Branch.hpp>
#include <opm/input/eclipse/Schedule/Network/ExtNetwork.hpp>
#include <opm/input/eclipse/Schedule/Network/Node.hpp>
#include <opm/input/eclipse/Schedule/MSW/Segment.hpp>
#include <opm/input/eclipse/Schedule/MSW/WellSegments.hpp>
#include <opm/input/eclipse/Schedule/Schedule.hpp>
#include <opm/input/eclipse/Schedule/Well/Connection.hpp>
#include <opm/input/eclipse/Schedule/Well/Well.hpp>
#include <opm/input/eclipse/Schedule/Well/WellConnections.hpp>

#include <algorithm>
#include <exception>
#include <functional>
#include <memory>

namespace flowgui {

QString injectName(Inject d)
{
    switch (d) {
        case Inject::Water: return QStringLiteral("water injector");
        case Inject::Gas:   return QStringLiteral("gas injector");
        case Inject::Oil:   return QStringLiteral("oil injector");
        case Inject::Multi: return QStringLiteral("multi-phase injector");
    }
    return QStringLiteral("injector");
}

namespace {
// What to draw a well as, at this moment: producers are one thing, injectors
// three, since water and gas going back in are not the same operation and the
// picture is read by people for whom that distinction is the point.
int wellKind(const Structure& s, const QString& well)
{
    if (!s.injectors.contains(well)) return GraphView::KindProducer;
    switch (s.injectors.value(well)) {
        case Inject::Water: return GraphView::KindInjWater;
        case Inject::Gas:   return GraphView::KindInjGas;
        default:            return GraphView::KindInjOther;
    }
}
} // namespace

const GroupNode* Structure::find(const QString& name) const
{
    for (const auto& g : groups) if (g.name == name) return &g;
    return nullptr;
}

int Structure::wellCount() const
{
    int n = 0;
    for (const auto& g : groups) n += g.wells.size();
    return n;
}

QString Structure::fingerprint() const
{
    QStringList bits;
    for (const auto& g : groups)
        bits << g.name + QLatin1Char('<') + g.parent + QLatin1Char('{')
                + g.wells.join(QLatin1Char(',')) + QLatin1Char('}');
    bits.sort();
    QStringList nb;
    for (const auto& b : branches) nb << b.down + QStringLiteral("->") + b.up;
    nb.sort();
    // Injectors count towards identity: converting a producer changes nothing
    // about the hierarchy but everything about what the picture says, and a
    // step that only did that would otherwise collapse into the one before it.
    QStringList inj;
    for (auto it = injectors.cbegin(); it != injectors.cend(); ++it)
        inj << it.key() + QLatin1Char('=') + QString::number(int(it.value()));
    return bits.join(QLatin1Char(';')) + QStringLiteral("|") + nb.join(QLatin1Char(';'))
           + QStringLiteral("|") + inj.join(QLatin1Char(','))
           + QStringLiteral("|%1%2").arg(int(netActive)).arg(int(netStandard));
}

namespace {

// The relaxations opm-common's own wellgraph example uses, named one by one
// rather than waving PARSE_* through wholesale. Norne trips the strict reader
// on a stray '/' - PARSE_RANDOM_SLASH, the first of these - and runs perfectly
// well; but a blanket downgrade would also swallow the errors that do mean a
// deck is broken, and this view is often what is being used to find that out.
Opm::ParseContext relaxedContext()
{
    return Opm::ParseContext({
        { Opm::ParseContext::PARSE_RANDOM_SLASH,         Opm::InputErrorAction::IGNORE },
        { Opm::ParseContext::PARSE_MISSING_DIMS_KEYWORD, Opm::InputErrorAction::WARN },
        { Opm::ParseContext::SUMMARY_UNKNOWN_WELL,       Opm::InputErrorAction::WARN },
        { Opm::ParseContext::SUMMARY_UNKNOWN_GROUP,      Opm::InputErrorAction::WARN },
    });
}

Structure snapshotAt(const Opm::Schedule& sched, std::size_t step)
{
    Structure s;
    s.step = int(step);
    s.when = QDateTime::fromSecsSinceEpoch(qint64(sched.simTime(step)), QTimeZone::utc());

    const auto& st = sched[step];
    for (const auto& name : sched.groupNames(step)) {
        if (!st.groups.has(name)) continue;
        const auto& g = st.groups.get(name);
        GroupNode n;
        n.name   = QString::fromStdString(g.name());
        n.parent = QString::fromStdString(g.parent());
        for (const auto& c : g.groups()) n.childGroups << QString::fromStdString(c);
        for (const auto& w : g.wells()) {
            const QString wn = QString::fromStdString(w);
            n.wells << wn;
            if (st.wells.has(w) && st.wells.get(w).isInjector()) {
                switch (st.wells.get(w).injectorType()) {
                    case Opm::InjectorType::WATER: s.injectors[wn] = Inject::Water; break;
                    case Opm::InjectorType::GAS:   s.injectors[wn] = Inject::Gas;   break;
                    case Opm::InjectorType::OIL:   s.injectors[wn] = Inject::Oil;   break;
                    default:                       s.injectors[wn] = Inject::Multi; break;
                }
            }
        }
        s.groups.push_back(n);
    }

    const auto& net = st.network();
    s.netActive   = net.active();
    s.netStandard = net.is_standard_network();
    if (s.netActive) {
        for (const auto* b : net.branches()) {
            if (!b) continue;
            NetBranch nb{ QString::fromStdString(b->downtree_node()),
                          QString::fromStdString(b->uptree_node()),
                          b->vfp_table().value_or(-1) };
            s.branches.push_back(nb);
            if (!s.netNodes.contains(nb.down)) s.netNodes << nb.down;
            if (!s.netNodes.contains(nb.up))   s.netNodes << nb.up;
        }
    }
    return s;
}

} // namespace

// The parsed deck, alive for as long as the structure read out of it. Clicking
// a well then costs a lookup rather than another half-second parse, and no
// segment or connection has to be copied out of the Schedule in advance.
struct DeckHold {
    std::shared_ptr<Opm::Python>       python;
    std::unique_ptr<Opm::EclipseState> es;
    std::unique_ptr<Opm::Schedule>     sched;
};

WellShape wellShapeAt(const DeckStructure& ds, int step, const QString& well)
{
    WellShape ws;
    ws.name = well;
    if (!ds.hold || !ds.hold->sched) {
        ws.problem = QStringLiteral("the deck is no longer loaded");
        return ws;
    }
    const auto& sched = *ds.hold->sched;
    if (sched.size() == 0) { ws.problem = QStringLiteral("empty schedule"); return ws; }
    const std::size_t st = std::size_t(std::clamp(step, 0, int(sched.size()) - 1));
    const std::string nm = well.toStdString();
    if (!sched[st].wells.has(nm)) {
        ws.problem = QStringLiteral("%1 is not in the schedule at this date").arg(well);
        return ws;
    }
    const auto& w = sched[st].wells.get(nm);
    ws.injector = w.isInjector();
    ws.msw      = w.isMultiSegment();
    if (ws.msw) {
        const auto& segs = w.getSegments();
        for (std::size_t i = 0; i < segs.size(); ++i) {
            const auto& sg = segs[i];
            WellSeg out;
            out.nr     = sg.segmentNumber();
            out.outlet = sg.outletSegment();
            out.branch = sg.branchNumber();
            out.length = sg.totalLength();
            out.depth  = sg.depth();
            out.diam   = sg.internalDiameter();
            // What opm-common's own drawing tells apart by colour and shape.
            if      (sg.isValve())     out.device = QStringLiteral("valve");
            else if (sg.isSpiralICD()) out.device = QStringLiteral("SICD");
            else if (sg.isAICD())      out.device = QStringLiteral("AICD");
            ws.segs.push_back(out);
        }
    }
    const auto& cs = w.getConnections();
    for (std::size_t i = 0; i < cs.size(); ++i) {
        const auto& c = cs[i];
        WellConn out;
        out.i = c.getI() + 1;          // the deck's own 1-based numbering
        out.j = c.getJ() + 1;
        out.k = c.getK() + 1;
        out.segment = c.attachedToSegment() ? c.segment() : 0;
        out.open = c.state() == Opm::Connection::State::OPEN;
        ws.conns.push_back(out);
    }
    ws.ok = true;
    return ws;
}

DeckStructure readDeckStructure(const QString& dataFile,
                                std::atomic<bool>* cancel,
                                std::atomic<int>* progress)
{
    DeckStructure out;
    out.deckPath = dataFile;
    const auto tick = [progress](int p) { if (progress) progress->store(p); };
    const auto stop = [cancel] { return cancel && cancel->load(); };
    tick(1);

    try {
        Opm::ParseContext ctx = relaxedContext();
        Opm::ErrorGuard guard;

        Opm::Parser parser;
        const auto deck = parser.parseFile(dataFile.toStdString(), ctx, guard);
        if (stop()) { out.problem = QStringLiteral("cancelled"); return out; }
        tick(35);

        auto hold = std::make_shared<DeckHold>();
        hold->es = std::make_unique<Opm::EclipseState>(deck);
        if (stop()) { out.problem = QStringLiteral("cancelled"); return out; }
        tick(55);

        hold->python = std::make_shared<Opm::Python>();
        hold->sched  = std::make_unique<Opm::Schedule>(deck, *hold->es, ctx, guard,
                                                      hold->python);
        const Opm::Schedule& sched = *hold->sched;
        out.hold = hold;         // kept, so a well can be looked at on demand
        tick(75);

        out.scheduleSteps = int(sched.size());
        QString last;
        for (std::size_t i = 0; i < sched.size(); ++i) {
            if (stop()) { out.problem = QStringLiteral("cancelled"); return out; }
            Structure s = snapshotAt(sched, i);
            // Only when something actually changed. A schedule is mostly
            // repetition, and a picker with 248 identical entries hides the
            // three moments that matter.
            const QString fp = s.fingerprint();
            if (fp != last) { out.shapes.push_back(s); last = fp; }
            tick(75 + int(24.0 * double(i + 1) / double(sched.size())));
        }
        out.ok = !out.shapes.isEmpty();
        if (!out.ok) out.problem = QStringLiteral("the deck defines no groups");
    } catch (const std::exception& e) {
        out.ok = false;
        out.problem = QString::fromLocal8Bit(e.what());
    } catch (...) {
        out.ok = false;
        out.problem = QStringLiteral("could not read the deck");
    }
    tick(100);
    return out;
}

// ===========================================================================
// GraphView
// ===========================================================================

GraphView::GraphView(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(160);
    setToolTip(QStringLiteral(
        "drag the key to move it; double-click it to put it back"));
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::white);
    setPalette(pal);
}

void GraphView::setGraph(const QStringList& nodes, const QVector<Edge>& edges,
                         const QString& emptyText,
                         const QHash<QString, int>& kinds)
{
    nodes_ = nodes; edges_ = edges; empty_ = emptyText; kinds_ = kinds;
    relayout();
    update();
}

void GraphView::setHighlight(const QString& node)
{
    highlight_ = node;
    update();
}

QSize GraphView::minimumSizeHint() const { return { 280, 160 }; }

// Layered, with the roots on top. Depth is the longest path to a root, so a
// node always sits below everything that feeds it - shortest-path would let an
// edge run sideways or backwards and the picture stops reading as a flow.
//
// Order within a layer is then swept by barycentre: repeatedly put each node
// at the average x of its neighbours in the layer above and re-sort. It is the
// cheap half of what a real graph layout does, and on these graphs - a dozen
// nodes, mostly tree-shaped - it removes essentially all the crossings that
// the arbitrary input order produced.
QFont GraphView::nodeFont() const
{
    QFont f = font();
    f.setPointSizeF(9.0);
    f.setBold(true);
    return f;
}

// The layout, in units of its own rather than fractions of the pane. Spreading
// each layer evenly across the whole width is what put two children of one node
// at opposite edges of the window: the drawing has a natural size, and render()
// fits that into whatever space there is instead of stretching it.
//
// Layers come from longest-path depth, their ORDER from barycentre sweeps, and
// then x is settled by alternating pulls - a node under the middle of its
// parents, a parent over the middle of what it holds - with each layer packed
// afterwards so nothing overlaps. That is the cheap half of what dot does, and
// it produces the same shape: children clustered under their parent, growing
// outwards from the centre.
void GraphView::relayout()
{
    placed_.clear();
    maxDepth_ = 0;
    natW_ = natH_ = 0;
    if (nodes_.isEmpty()) return;

    QHash<QString, QStringList> parents;    // node -> the nodes it points at
    QHash<QString, QStringList> children;   // ... and the ones pointing at it
    for (const auto& e : edges_) {
        parents[e.from] << e.to;
        children[e.to]  << e.from;
    }

    QHash<QString, int> depth;
    std::function<int(const QString&, int)> deep = [&](const QString& n, int guard) -> int {
        if (guard > 64) return 0;                    // a cycle is not worth hanging over
        if (depth.contains(n)) return depth[n];
        int d = 0;
        for (const auto& up : parents.value(n)) d = std::max(d, deep(up, guard + 1) + 1);
        depth[n] = d;
        return d;
    };
    for (const auto& n : nodes_) maxDepth_ = std::max(maxDepth_, deep(n, 0));

    QVector<QStringList> layers(maxDepth_ + 1);
    for (const auto& n : nodes_) layers[depth.value(n)] << n;

    // Order within each layer, by barycentre, on evenly spaced positions - this
    // pass decides who is left of whom and nothing else.
    QHash<QString, double> ord;
    for (auto& layer : layers)
        for (int i = 0; i < layer.size(); ++i)
            ord[layer[i]] = layer.size() > 1 ? double(i) / (layer.size() - 1) : 0.5;
    for (int sweep = 0; sweep < 4; ++sweep) {
        for (int d = 1; d <= maxDepth_; ++d) {
            QStringList& layer = layers[d];
            QHash<QString, double> bary;
            for (const auto& n : layer) {
                const QStringList ups = parents.value(n);
                if (ups.isEmpty()) { bary[n] = ord.value(n); continue; }
                double sum = 0; int cnt = 0;
                for (const auto& u : ups) { sum += ord.value(u, 0.5); ++cnt; }
                bary[n] = cnt ? sum / cnt : ord.value(n);
            }
            std::sort(layer.begin(), layer.end(),
                      [&bary](const QString& a, const QString& b) { return bary[a] < bary[b]; });
            for (int i = 0; i < layer.size(); ++i)
                ord[layer[i]] = layer.size() > 1 ? double(i) / (layer.size() - 1) : 0.5;
        }
    }

    // How wide each node actually is, which is what the packing has to respect.
    const QFontMetricsF fm(nodeFont());
    natBoxH_   = fm.height() + 10;
    natRowGap_ = natBoxH_ * 2.7;
    QHash<QString, double> W;
    for (const auto& n : nodes_) {
        const bool well = isWellKind(kinds_.value(n, KindGroup));
        W[n] = std::max(54.0, fm.horizontalAdvance(n) + (well ? 20.0 : 34.0));
    }

    const double gap = 24.0;
    QHash<QString, double> X;
    for (const auto& layer : layers) {          // start packed left to right
        double x = 0;
        for (const auto& n : layer) { X[n] = x + W[n] / 2; x += W[n] + gap; }
    }
    auto pack = [&](const QStringList& layer) {
        for (int i = 1; i < layer.size(); ++i)
            X[layer[i]] = std::max(X[layer[i]],
                                   X[layer[i - 1]]
                                       + (W[layer[i - 1]] + W[layer[i]]) / 2 + gap);
    };
    auto meanOf = [&](const QStringList& ns) {
        double sum = 0; int cnt = 0;
        for (const auto& n : ns) if (X.contains(n)) { sum += X[n]; ++cnt; }
        return cnt ? sum / cnt : 0.0;
    };
    for (int it = 0; it < 8; ++it) {
        for (int d = 1; d <= maxDepth_; ++d) {          // under their parents
            for (const auto& n : layers[d])
                if (!parents.value(n).isEmpty()) X[n] = meanOf(parents.value(n));
            pack(layers[d]);
        }
        for (int d = maxDepth_ - 1; d >= 0; --d) {      // over what they hold
            for (const auto& n : layers[d])
                if (!children.value(n).isEmpty()) X[n] = meanOf(children.value(n));
            pack(layers[d]);
        }
    }

    double lo = 1e18, hi = -1e18;
    for (const auto& n : nodes_) {
        lo = std::min(lo, X[n] - W[n] / 2);
        hi = std::max(hi, X[n] + W[n] / 2);
    }
    natW_ = std::max(1.0, hi - lo);
    natH_ = maxDepth_ * natRowGap_ + natBoxH_;

    for (const auto& n : nodes_) {
        Placed q;
        q.name  = n;
        q.depth = depth.value(n);
        q.x     = X[n] - lo;
        q.w     = W[n];
        placed_.push_back(q);
    }
}

// One node, drawn according to what it is. Groups are rectangles and wells are
// ellipses, so the two are told apart by shape before colour comes into it -
// which matters on a projector, in a black-and-white print, and to a reader
// who does not see colour the way the person who chose it does.
void GraphView::paintNode(QPainter& p, const QRectF& r, const QString& text,
                          int kind, double thin, bool hot) const
{
    QColor fill, line, ink;
    kindColours(kind, fill, line, ink);

    // Selection is a halo rather than a recolour: every colour in the palette
    // already means something, so borrowing one to mean "selected" would make
    // a selected producer look like an injector.
    if (hot) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xff, 0xd0, 0x4a));
        const QRectF halo = r.adjusted(-5, -5, 5, 5);
        if (isWellKind(kind)) p.drawRect(halo); else p.drawEllipse(halo);
    }

    p.setPen(QPen(line, (isWellKind(kind) ? 1.6 : 1.1) * thin));
    p.setBrush(fill);
    if (isWellKind(kind)) p.drawRect(r); else p.drawEllipse(r);

    p.setPen(ink);
    p.drawText(r, Qt::AlignCenter, text);
}

// Where a line from `towards` meets this node's outline. An ellipse is cut
// back along the ray; a rectangle at whichever edge the ray leaves through.
QPointF GraphView::edgePoint(const QRectF& r, bool rect, const QPointF& towards)
{
    const double dx = towards.x(), dy = towards.y();
    if (qFuzzyIsNull(dx) && qFuzzyIsNull(dy)) return r.center();
    const double a = r.width() / 2, b = r.height() / 2;
    double s;
    if (rect) {
        s = std::min(qFuzzyIsNull(dx) ? 1e18 : a / std::abs(dx),
                     qFuzzyIsNull(dy) ? 1e18 : b / std::abs(dy));
    } else {
        s = 1.0 / std::sqrt((dx / a) * (dx / a) + (dy / b) * (dy / b));
    }
    return r.center() + QPointF(dx * s, dy * s);
}

void GraphView::drawArrow(QPainter& p, const QPointF& tip, const QPointF& from,
                          double size)
{
    QPointF d = tip - from;
    const double len = std::hypot(d.x(), d.y());
    if (len < 1e-6) return;
    d /= len;
    const QPointF n(-d.y(), d.x());
    QPolygonF head;
    head << tip
         << tip - d * size + n * (size * 0.40)
         << tip - d * size - n * (size * 0.40);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x33, 0x33, 0x33));
    p.drawPolygon(head);
}

// The one place the palette lives. The tree pane reads its text colours from
// here too, so the two panes cannot drift apart into two colour schemes.
//
// These are the colours opm-common's own well/group graphs use - orange for
// the groups that hold wells, red for producers, blue for injectors - so a
// picture from this tab sits beside one from the wellgraph tool and reads the
// same way. Splitting the injectors by phase needs colours dot's convention
// does not have: gas takes green and the rare oil and multi-phase cases purple,
// since red is already spoken for by the producers.
void GraphView::kindColours(int kind, QColor& fill, QColor& line, QColor& ink)
{
    fill = Qt::white;
    line = QColor(0x22, 0x22, 0x22);
    ink  = QColor(0x11, 0x11, 0x11);
    switch (kind) {
        case KindWellGroup: fill = QColor(0xff, 0xa5, 0x00); break;
        case KindProducer:  line = QColor(0xd4, 0x00, 0x00); break;
        case KindInjWater:  line = QColor(0x00, 0x00, 0xd8); break;
        case KindInjGas:    line = QColor(0x00, 0x77, 0x2b); break;
        case KindInjOther:  line = QColor(0x7a, 0x1f, 0xa2); break;
        // Inside a well. The connection colour is opm-common's lightgreen, so
        // a picture drawn here and one drawn by plot_ms_wells read alike.
        case KindSegment:    fill = QColor(0xff, 0xff, 0xf0); break;
        case KindSegDevice:  fill = QColor(0xff, 0xd7, 0x00);
                             line = QColor(0x8a, 0x6d, 0x00); break;
        case KindConnection: fill = QColor(0x90, 0xee, 0x90);
                             line = QColor(0x2e, 0x6b, 0x2e); break;
        default: break;
    }
}

// The same meaning as the outline colour in the drawing, but readable as text
// on white - which orange is not, so a well group takes a darker amber of the
// same hue. Used by the tree pane, so both panes say the same thing.
QColor GraphView::kindText(int kind)
{
    switch (kind) {
        case KindWellGroup: return QColor(0xa8, 0x6a, 0x00);
        case KindProducer:  return QColor(0xc4, 0x00, 0x00);
        case KindInjWater:  return QColor(0x00, 0x00, 0xc8);
        case KindInjGas:    return QColor(0x00, 0x6b, 0x27);
        case KindInjOther:  return QColor(0x7a, 0x1f, 0xa2);
        default:            return QColor(0x22, 0x26, 0x2b);
    }
}

bool GraphView::isWellKind(int kind)
{
    // Really "drawn as a rectangle rather than an ellipse": wells are, groups
    // are not. Segments join them, and connections do not, which is the way
    // round opm-common's own well drawing has it.
    return kind == KindProducer || kind == KindInjWater
        || kind == KindInjGas   || kind == KindInjOther
        || kind == KindSegment  || kind == KindSegDevice;
}

QString GraphView::kindName(int kind)
{
    switch (kind) {
        case KindProducer:  return QStringLiteral("producer");
        case KindInjWater:  return QStringLiteral("water injector");
        case KindInjGas:    return QStringLiteral("gas injector");
        case KindInjOther:  return QStringLiteral("other injector");
        case KindWellGroup: return QStringLiteral("well group");
        case KindSegment:    return QStringLiteral("segment");
        case KindSegDevice:  return QStringLiteral("segment with a device");
        case KindConnection: return QStringLiteral("connection");
        default:            return QStringLiteral("group");
    }
}

void GraphView::render(QPainter& p, const QRectF& area) const
{
    p.fillRect(area, Qt::white);
    if (placed_.isEmpty()) {
        p.setPen(QColor(0x88, 0x8e, 0x94));
        p.drawText(area, Qt::AlignCenter, empty_);
        return;
    }
    p.setRenderHint(QPainter::Antialiasing);

    const double sc = fitScale(area);
    const QRectF drawn = drawnRect(area);
    // Lines and arrowheads scale with everything else, but not all the way
    // down: at the size a whole field needs they would vanish.
    const double thin = 1.0 / std::clamp(sc, 0.35, 1.0);

    p.save();
    p.translate(drawn.topLeft());
    p.scale(sc, sc);

    const QFont nf = nodeFont();
    p.setFont(nf);

    auto isWell = [&](const QString& n) {
        return isWellKind(kinds_.value(n, KindGroup));
    };
    auto boxOf = [&](const Placed& q) {
        const bool well = isWell(q.name);
        const double h = well ? natBoxH_ - 3 : natBoxH_;
        return QRectF(q.x - q.w / 2,
                      q.depth * natRowGap_ + (natBoxH_ - h) / 2,
                      q.w, h);
    };
    auto find = [&](const QString& n) -> const Placed* {
        for (const auto& q : placed_) if (q.name == n) return &q;
        return nullptr;
    };

    // Edges first, so a node always sits on top of the lines that reach it.
    QFont ef = nf; ef.setPointSizeF(7.5); ef.setBold(false);
    for (const auto& e : edges_) {
        const Placed* a = find(e.from);
        const Placed* b = find(e.to);
        if (!a || !b) continue;
        const QRectF ra = boxOf(*a), rb = boxOf(*b);
        // Centre to centre, then cut back to each shape's own boundary, so a
        // line meets an ellipse where the ellipse actually is rather than at
        // the corner of the rectangle around it.
        const QPointF from = edgePoint(rb, isWell(e.to), ra.center() - rb.center());
        const QPointF to   = edgePoint(ra, isWell(e.from), rb.center() - ra.center());
        p.setPen(QPen(QColor(0x33, 0x33, 0x33), 1.1 * thin));
        p.setBrush(Qt::NoBrush);
        p.drawLine(from, to);
        // The arrow points the way the eye reads the tree: down from a node to
        // the things it holds.
        drawArrow(p, to, from, 9.0 * thin);
        if (!e.label.isEmpty()) {
            p.setFont(ef);
            const QPointF mid = (from + to) / 2;
            const QFontMetricsF efm(ef);
            QRectF lr(0, 0, efm.horizontalAdvance(e.label) + 6, efm.height());
            lr.moveCenter(mid);
            // On a patch of background, or the line it labels runs through it.
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::white);
            p.drawRect(lr);
            p.setPen(QColor(0x8a, 0x6d, 0x3b));
            p.drawText(lr, Qt::AlignCenter, e.label);
            p.setFont(nf);
        }
    }

    for (const auto& q : placed_)
        paintNode(p, boxOf(q), q.name, kinds_.value(q.name, KindGroup), thin,
                  !highlight_.isEmpty() && q.name == highlight_);
    p.restore();

    // The key stays the size it is: it is there to be read, not to be part of
    // the picture's scale. It floats over the drawing rather than taking a
    // strip off it, and where it should sit depends on the graph - so it is
    // draggable, and drag beats guessing.
    const QVector<int> kk = keyKinds();
    if (kk.isEmpty()) return;
    const QFont kf = keyFont(sc);
    p.setFont(kf);
    const QFontMetricsF kfm(kf);
    const QRectF kr = keyRect(area, kfm);
    const double rowH = kfm.height() + 6;
    const double sw   = kfm.height() * 1.9;
    const double sh   = kfm.height() * 0.95;

    p.setPen(QPen(QColor(0xb6, 0xbe, 0xc6), 1.0));
    p.setBrush(QColor(255, 255, 255, 235));
    p.drawRoundedRect(kr, 5, 5);

    double ky = kr.top() + 7;
    for (int k : kk) {
        paintNode(p, QRectF(kr.left() + 10, ky + (rowH - sh) / 2, sw, sh),
                  QString(), k, std::clamp(sc, 1.0, 1.6), false);
        p.setPen(QColor(0x33, 0x38, 0x3d));
        p.drawText(QRectF(kr.left() + 10 + sw + 9, ky, kr.width() - sw - 29, rowH),
                   Qt::AlignVCenter | Qt::AlignLeft, kindName(k));
        ky += rowH;
    }
}

// How much of its natural size the drawing is shown at. It shrinks to fit
// without argument; growing is deliberately damped - a big window should make
// the picture comfortably bigger, not stretch eight nodes across a metre of
// screen - and stops at twice size, past which the type stops looking like
// type and starts looking like a poster.
double GraphView::fitScale(const QRectF& area) const
{
    if (natW_ <= 0 || natH_ <= 0) return 1.0;
    const double margin = 18.0;
    double fit = std::min((area.width()  - 2 * margin) / natW_,
                          (area.height() - 2 * margin) / natH_);
    if (fit > 1.0) fit = 1.0 + (fit - 1.0) * 0.5;
    return std::clamp(fit, 0.02, 2.0);
}

// Where the drawing actually lands: centred, at its fitted size.
QRectF GraphView::drawnRect(const QRectF& area) const
{
    const double sc = fitScale(area);
    const QSizeF sz(natW_ * sc, natH_ * sc);
    return QRectF(area.center().x() - sz.width() / 2,
                  area.center().y() - sz.height() / 2,
                  sz.width(), sz.height());
}

// The key grows with the drawing, since it is read at the same distance and on
// the same page. Not all the way down, though: on a field squeezed into a small
// pane the node labels become unreadable long before the key needs to, and the
// key is the one thing still worth reading there.
QFont GraphView::keyFont(double sc) const
{
    QFont f = font();
    f.setPointSizeF(std::clamp(8.5 * sc, 8.0, 14.0));
    f.setBold(false);
    return f;
}

QVector<int> GraphView::keyKinds() const
{
    QVector<int> kk;
    for (const auto& q : placed_) {
        const int k = kinds_.value(q.name, KindGroup);
        if (k != KindNetwork && !kk.contains(k)) kk << k;
    }
    if (kk.size() < 2) return {};      // one kind tells nothing apart
    std::sort(kk.begin(), kk.end());
    return kk;
}

QRectF GraphView::keyRect(const QRectF& area, const QFontMetricsF& fm) const
{
    const QVector<int> kk = keyKinds();
    if (kk.isEmpty()) return {};
    const double rowH = fm.height() + 6;
    const double sw   = fm.height() * 1.9;      // the swatch, sized to the text
    double tw = 0;
    for (int k : kk) tw = std::max(tw, fm.horizontalAdvance(kindName(k)));
    const double w = 10 + sw + 9 + tw + 10;
    const double h = 7 + kk.size() * rowH + 7;

    double x, y;
    if (keyPos_.x() < 0) {
        // The drawing's top-right corner, not the pane's: in a window much
        // bigger than the graph the pane corner is half a screen away from
        // what it explains. The top row is usually one root in the middle, so
        // the corner it lands in is free.
        const QRectF drawn = drawnRect(area);
        x = drawn.right() - w;
        y = drawn.top();
    } else {
        x = area.left() + keyPos_.x() * area.width();
        y = area.top()  + keyPos_.y() * area.height();
    }
    // Never off the edge, however the window was resized after the drag.
    x = std::clamp(x, area.left(), std::max(area.left(), area.right() - w));
    y = std::clamp(y, area.top(),  std::max(area.top(),  area.bottom() - h));
    return QRectF(x, y, w, h);
}

void GraphView::resetKey()
{
    keyPos_ = QPointF(-1.0, -1.0);
    update();
}

void GraphView::mousePressEvent(QMouseEvent* ev)
{
    const QRectF area(rect());
    const QRectF kr = keyRect(area, QFontMetricsF(keyFont(fitScale(area))));
    if (ev->button() == Qt::LeftButton && !kr.isEmpty()
        && kr.contains(ev->position())) {
        keyDrag_ = true;
        keyGrab_ = ev->position() - kr.topLeft();
        setCursor(Qt::ClosedHandCursor);
        ev->accept();
        return;
    }
    QWidget::mousePressEvent(ev);
}

void GraphView::mouseMoveEvent(QMouseEvent* ev)
{
    if (!keyDrag_) { QWidget::mouseMoveEvent(ev); return; }
    const QRectF area(rect());
    if (area.width() <= 0 || area.height() <= 0) return;
    const QPointF tl = ev->position() - keyGrab_;
    keyPos_ = QPointF(std::clamp((tl.x() - area.left()) / area.width(), 0.0, 1.0),
                      std::clamp((tl.y() - area.top()) / area.height(), 0.0, 1.0));
    update();
    ev->accept();
}

void GraphView::mouseReleaseEvent(QMouseEvent* ev)
{
    if (keyDrag_) {
        keyDrag_ = false;
        unsetCursor();
        ev->accept();
        return;
    }
    QWidget::mouseReleaseEvent(ev);
}

// Put it back where it started, for anyone who dragged it somewhere unhelpful.
// The inverse of what render() does to place a node: undo the fit-to-pane
// scale and the centring, then test the boxes in the layout's own units.
QString GraphView::nodeAt(const QPointF& pos) const
{
    if (placed_.isEmpty() || natW_ <= 0) return {};
    const QRectF area(rect());
    const double sc = fitScale(area);
    if (sc <= 0) return {};
    const QRectF drawn = drawnRect(area);
    const QPointF at((pos.x() - drawn.left()) / sc, (pos.y() - drawn.top()) / sc);
    for (const auto& q : placed_) {
        const double h = isWellKind(kinds_.value(q.name, KindGroup)) ? natBoxH_ - 3 : natBoxH_;
        const QRectF box(q.x - q.w / 2, q.depth * natRowGap_ + (natBoxH_ - h) / 2, q.w, h);
        if (box.contains(at)) return q.name;
    }
    return {};
}

void GraphView::mouseDoubleClickEvent(QMouseEvent* ev)
{
    const QRectF area(rect());
    const QRectF kr = keyRect(area, QFontMetricsF(keyFont(fitScale(area))));
    if (!kr.isEmpty() && kr.contains(ev->position())) { resetKey(); ev->accept(); return; }
    const QString n = nodeAt(ev->position());
    if (!n.isEmpty()) { emit nodeActivated(n); ev->accept(); return; }
    QWidget::mouseDoubleClickEvent(ev);
}

void GraphView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    render(p, QRectF(rect()));
}

// ===========================================================================
// StructurePanel
// ===========================================================================

StructurePanel::StructurePanel(QWidget* parent) : QWidget(parent)
{
    openBtn_ = new QPushButton(QStringLiteral("Open DATA..."));
    openBtn_->setToolTip(QStringLiteral(
        "read a deck's group tree and network. The deck is parsed as flow "
        "parses it, so INCLUDEs are followed and the structure is the one the "
        "simulator would use."));
    shapeBox_ = new QComboBox;
    shapeBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    shapeBox_->setMinimumWidth(240);
    shapeBox_->setToolTip(QStringLiteral(
        "when the structure changed. GRUPTREE lives in SCHEDULE and is free to "
        "change, so this lists the dates where it did - not every report step, "
        "most of which repeat what came before."));
    // opm-common's own wellgraph tool warns that a field with many wells makes
    // an unreadable graph and offers --separate-well-groups for it. Same
    // problem here: Norne's 36 wells are fine inline, a few hundred are not.
    showWells_ = new QCheckBox(QStringLiteral("wells"));
    showWells_->setChecked(true);
    showWells_->setToolTip(QStringLiteral(
        "list the wells under their group. Turn off on a field with many of "
        "them to read the group hierarchy on its own."));
    viewBox_ = new QComboBox;
    viewBox_->addItem(QStringLiteral("group tree"));
    viewBox_->addItem(QStringLiteral("network"));
    viewBox_->setToolTip(QStringLiteral(
        "which graph to draw. The tree on the left is always the group "
        "hierarchy; this is what the picture beside it shows."));
    picBtn_ = new QPushButton(QStringLiteral("Save picture..."));
    picBtn_->setEnabled(false);
    picBtn_->setToolTip(QStringLiteral(
        "write the drawing as it stands to PNG or PDF. The same painting code "
        "draws the screen and the file, so the file is what you are looking "
        "at - vector in the PDF, at whatever size you ask for in the PNG."));
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(QStringLiteral("filter groups and wells..."));
    filter_->setClearButtonEnabled(true);
    bar_ = new QProgressBar; bar_->setRange(0, 100); bar_->setVisible(false);
    bar_->setMaximumWidth(160);

    auto* row = new QHBoxLayout;
    row->addWidget(openBtn_);
    row->addWidget(new QLabel(QStringLiteral("Structure at:")));
    row->addWidget(shapeBox_);
    row->addWidget(showWells_);
    row->addWidget(new QLabel(QStringLiteral("Draw:")));
    row->addWidget(viewBox_);
    wellBtn_ = new QPushButton(QStringLiteral("Well structure..."));
    wellBtn_->setToolTip(QStringLiteral(
        "draw the selected well's own structure: its segments, and the "
        "connections that reach the grid\n(or double-click the well, in "
        "either pane)"));
    wellBtn_->setEnabled(false);
    row->addWidget(wellBtn_);
    row->addWidget(picBtn_);
    row->addWidget(bar_);
    row->addWidget(filter_, 1);

    status_ = new QLabel(QStringLiteral("open a *.DATA file to see its group tree"));
    status_->setWordWrap(true);
    netInfo_ = new QLabel;
    netInfo_->setStyleSheet(QStringLiteral("color:#555b61;"));

    tree_ = new QTreeWidget;
    // Deep trees indent a long way, and resizeColumnToContents() then makes
    // column 0 wider than a narrow pane - at which point the rows that matter
    // most show nothing but their own indentation. Give it a floor.
    tree_->setMinimumWidth(250);
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({ QStringLiteral("Group / well"), QStringLiteral("Contents") });
    tree_->header()->setStretchLastSection(true);

    graph_ = new GraphView;

    auto* right = new QWidget;
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->addWidget(netInfo_);
    rl->addWidget(graph_, 1);

    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(tree_);
    split->addWidget(right);
    // Every pixel the window gains goes to the drawing. The names in the tree
    // are as long as they are going to get, so a wider window only adds empty
    // column to it, while the drawing can always use the room.
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    // After the first layout, not during it: sizes set on a splitter that has
    // no size yet are rescaled out of all recognition once it gets one.
    QTimer::singleShot(0, this, [split] {
        split->setSizes({ 330, std::max(400, split->width() - 330) });
    });

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(row);
    lay->addWidget(status_);
    lay->addWidget(split, 1);

    connect(openBtn_, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, QStringLiteral("Open input deck"),
            flowgui::startDir(QStringLiteral("deck"), model_.deckPath),
            QStringLiteral("Eclipse decks (*.DATA *.data);;All files (*)"));
        if (f.isEmpty()) return;
        flowgui::rememberDir(QStringLiteral("deck"), f);
        openDeck(f);
    });
    connect(shapeBox_, &QComboBox::currentIndexChanged, this,
            [this](int i) { showShape(i); });
    connect(filter_, &QLineEdit::textChanged, this,
            [this](const QString& t) { applyFilter(t.trimmed()); });
    connect(showWells_, &QCheckBox::toggled, this,
            [this](bool) { showShape(shapeBox_->currentIndex()); });
    connect(picBtn_, &QPushButton::clicked, this, [this] { exportPicture(); });
    connect(viewBox_, &QComboBox::currentIndexChanged, this, [this](int) { refreshGraph(); });
    // Selecting in the tree marks the same node in the drawing, which is the
    // cheapest way to tie a name in a list to a box in a picture.
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        auto* it = tree_->currentItem();
        graph_->setHighlight(it ? it->text(0) : QString());
        wellBtn_->setEnabled(it && it->data(0, Qt::UserRole).toBool());
    });
    // A well is worth opening on its own: the group tree says where it hangs,
    // its own drawing says how it is put together. Double-click either pane.
    connect(tree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* it, int) {
                if (it && it->data(0, Qt::UserRole).toBool()) showWellStructure(it->text(0));
            });
    connect(graph_, &GraphView::nodeActivated, this, [this](const QString& n) {
        const int i = shapeBox_->currentIndex();
        if (i < 0 || i >= model_.shapes.size()) return;
        if (!model_.shapes[i].find(n)) showWellStructure(n);   // not a group: a well
    });
    connect(wellBtn_, &QPushButton::clicked, this, [this] {
        auto* it = tree_->currentItem();
        if (it && it->data(0, Qt::UserRole).toBool()) showWellStructure(it->text(0));
    });

    poll_ = new QTimer(this);
    poll_->setInterval(120);
    connect(poll_, &QTimer::timeout, this, [this] { bar_->setValue(progress_.load()); });
}

StructurePanel::~StructurePanel()
{
    cancel_.store(true);
    if (worker_) { worker_->quit(); worker_->wait(8000); }
}

void StructurePanel::openDeck(const QString& dataFile)
{
    if (dataFile.isEmpty()) return;
    if (worker_) { pending_ = dataFile; cancel_.store(true); return; }
    startLoad(dataFile);
}

void StructurePanel::startLoad(const QString& dataFile)
{
    cancel_.store(false);
    progress_.store(0);
    bar_->setValue(0); bar_->setVisible(true);
    openBtn_->setEnabled(false);
    status_->setText(QStringLiteral("reading %1 ...").arg(QFileInfo(dataFile).fileName()));
    poll_->start();
    worker_ = QThread::create([this, dataFile] {
        model_ = readDeckStructure(dataFile, &cancel_, &progress_);
    });
    connect(worker_, &QThread::finished, this, [this] { finishLoad(); });
    worker_->start();
}

void StructurePanel::finishLoad()
{
    poll_->stop();
    bar_->setVisible(false);
    openBtn_->setEnabled(true);
    if (worker_) { worker_->deleteLater(); worker_ = nullptr; }
    // A second deck asked for while the first was still reading.
    if (!pending_.isEmpty()) {
        const QString next = pending_;
        pending_.clear();
        startLoad(next);
        return;
    }

    shapeBox_->blockSignals(true);
    shapeBox_->clear();
    for (const auto& s : model_.shapes)
        shapeBox_->addItem(QStringLiteral("%1   (step %2)")
                               .arg(s.when.toString(QStringLiteral("yyyy-MM-dd")))
                               .arg(s.step));
    shapeBox_->blockSignals(false);

    if (!model_.ok) {
        status_->setText(QStringLiteral("could not read %1: %2")
                             .arg(QFileInfo(model_.deckPath).fileName(), model_.problem));
        status_->setStyleSheet(QStringLiteral("color:#8a1f1f;"));
        picBtn_->setEnabled(false);
        tree_->clear();
        graph_->setGraph({}, {}, QStringLiteral("no deck loaded"));
        return;
    }
    status_->setStyleSheet(QString());
    picBtn_->setEnabled(true);
    const auto& first = model_.shapes.first();
    status_->setText(QStringLiteral(
        "%1: %2 group(s), %3 well(s) over %4 schedule step(s); the structure "
        "changes %5 time(s)")
        .arg(QFileInfo(model_.deckPath).fileName())
        .arg(first.groups.size()).arg(model_.shapes.last().wellCount())
        .arg(model_.scheduleSteps).arg(model_.shapes.size()));
    // The last shape is the field as it ends up, which is the one people mean
    // when they ask what a deck looks like.
    shapeBox_->setCurrentIndex(shapeBox_->count() - 1);
    showShape(shapeBox_->count() - 1);
}

void StructurePanel::showShape(int index)
{
    tree_->clear();
    if (index < 0 || index >= model_.shapes.size()) {
        graph_->setGraph({}, {}, QStringLiteral("no deck loaded"));
        return;
    }
    const Structure& s = model_.shapes[index];

    // Depth-first from the roots, so the tree on screen is the tree in the
    // deck. A group whose parent is missing is still shown, at the top: a
    // deck that names a parent it never defines should look wrong, not vanish.
    QSet<QString> placed;
    std::function<void(const QString&, QTreeWidgetItem*)> add =
        [&](const QString& name, QTreeWidgetItem* parent) {
            const GroupNode* g = s.find(name);
            if (!g || placed.contains(name)) return;
            placed.insert(name);
            auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
            it->setText(0, name);
            QStringList what;
            if (!g->childGroups.isEmpty())
                what << QStringLiteral("%1 group(s)").arg(g->childGroups.size());
            if (!g->wells.isEmpty())
                what << QStringLiteral("%1 well(s)").arg(g->wells.size());
            it->setText(1, what.join(QStringLiteral(", ")));
            QFont f = it->font(0); f.setBold(true); it->setFont(0, f);
            // Same colours as the drawing, so the two panes read as one thing.
            if (!g->wells.isEmpty())
                it->setForeground(0, QBrush(GraphView::kindText(GraphView::KindWellGroup)));
            for (const auto& c : g->childGroups) add(c, it);
            if (!showWells_->isChecked()) return;
            for (const auto& w : g->wells) {
                auto* wi = new QTreeWidgetItem(it);
                wi->setText(0, w);
                wi->setData(0, Qt::UserRole, true);      // a well, not a group
                const int k = wellKind(s, w);
                wi->setForeground(0, QBrush(GraphView::kindText(k)));
                wi->setText(1, s.injectors.contains(w)
                                   ? injectName(s.injectors.value(w))
                                   : QStringLiteral("producer"));
                wi->setForeground(1, QBrush(QColor(0x77, 0x7f, 0x88)));
            }
        };
    for (const auto& g : s.groups)
        if (g.parent.isEmpty() || !s.find(g.parent)) add(g.name, nullptr);
    for (const auto& g : s.groups) add(g.name, nullptr);   // orphans, if any
    // Open it if it fits. A collapsed tree is the same as no tree when what
    // you came to see - the wells - hangs four levels down, and most decks are
    // small enough to show whole. Past that it is a wall of names, so only the
    // groups near the top are opened and the rest is there to be asked for.
    int rows = s.groups.size();
    if (showWells_->isChecked())
        for (const auto& g : s.groups) rows += g.wells.size();
    if (rows <= 80) tree_->expandAll();
    else            tree_->expandToDepth(1);
    tree_->resizeColumnToContents(0);

    refreshGraph();
    netInfo_->setText(!s.netActive
        ? QStringLiteral("no network defined at this date")
        : QStringLiteral("%1 network: %2 node(s), %3 branch(es)")
              .arg(s.netStandard ? QStringLiteral("standard (GRUPNET)")
                                 : QStringLiteral("extended (BRANPROP)"))
              .arg(s.netNodes.size()).arg(s.branches.size()));
    applyFilter(filter_->text().trimmed());
}

// Feed the drawing from whichever graph is asked for. Both come out of the
// same Structure, and both are edges pointing from a node to the one above it.
void StructurePanel::refreshGraph()
{
    const int i = shapeBox_->currentIndex();
    if (i < 0 || i >= model_.shapes.size()) {
        graph_->setGraph({}, {}, QStringLiteral("no deck loaded"));
        return;
    }
    const Structure& s = model_.shapes[i];
    QStringList nodes;
    QVector<GraphView::Edge> edges;

    QHash<QString, int> kinds;

    if (viewBox_->currentIndex() == 0) {          // the group hierarchy
        for (const auto& g : s.groups) {
            nodes << g.name;
            // A group that holds wells is where the hierarchy stops and the
            // field begins - worth telling apart from one that only routes
            // other groups, whether or not the wells themselves are drawn.
            // On having wells, not on being a leaf: a group that holds neither
            // wells nor groups yet is not a well group, it is an empty one.
            kinds[g.name] = g.wells.isEmpty() ? GraphView::KindGroup
                                              : GraphView::KindWellGroup;
            if (!g.parent.isEmpty() && s.find(g.parent))
                edges.push_back({ g.name, g.parent, {} });
        }
        // Wells only when they are wanted: a field with hundreds of them makes
        // a picture nobody can read, which is the warning opm-common's own
        // wellgraph tool gives about exactly this.
        if (showWells_->isChecked())
            for (const auto& g : s.groups)
                for (const auto& w : g.wells) {
                    nodes << w;
                    kinds[w] = wellKind(s, w);
                    edges.push_back({ w, g.name, {} });
                }
        graph_->setGraph(nodes, edges,
                         QStringLiteral("this deck defines no groups"), kinds);
    } else {                                       // the network
        nodes = s.netNodes;
        for (const auto& n : nodes) kinds[n] = GraphView::KindNetwork;
        for (const auto& b : s.branches)
            edges.push_back({ b.down, b.up,
                              b.vfp > 0 ? QStringLiteral("VFP %1").arg(b.vfp) : QString() });
        graph_->setGraph(nodes, edges,
                         QStringLiteral("this deck defines no network"), kinds);
    }
}

// Straight to PNG or PDF, from the painter that draws the screen - so the file
// is the picture, not a second rendering of it that can drift from it. The PDF
// is vector, which is what a report wants; the PNG is drawn at three times the
// on-screen size so it survives being scaled.
// One well drawn on its own: the segment tree of a multisegment well with the
// connections hanging off the segments they belong to, or - for a well that is
// not segmented - simply the cells it reaches.
//
// The shape of it follows opm-common's own WellStructureViz, so this picture
// and one from plot_ms_wells read the same way: edges run towards the
// wellhead, a segment says which branch it is on, connections are labelled
// with the deck's 1-based (i,j,k) and coloured its lightgreen. What differs is
// that this is drawn in-process, like the group graph beside it, so it needs
// no Graphviz and no file written to disk to be looked at.
void StructurePanel::showWellStructure(const QString& well)
{
    const int idx = shapeBox_->currentIndex();
    if (idx < 0 || idx >= model_.shapes.size()) return;
    const Structure& shape = model_.shapes[idx];
    const WellShape ws = wellShapeAt(model_, shape.step, well);
    if (!ws.ok) {
        QMessageBox::information(this, QStringLiteral("Well structure"),
            ws.problem.isEmpty() ? QStringLiteral("nothing to draw for %1").arg(well)
                                 : ws.problem);
        return;
    }

    QStringList nodes;
    QVector<GraphView::Edge> edges;
    QHash<QString, int> kinds;

    // The wellhead, coloured the way the group tree colours it.
    const QString head = ws.name;
    nodes << head;
    kinds[head] = wellKind(shape, ws.name);

    // Segments, and the name each one is drawn under.
    QHash<int, QString> segNode;
    for (const auto& sg : ws.segs) {
        QString label = QStringLiteral("Seg %1").arg(sg.nr);
        if (sg.branch > 1) label += QStringLiteral(" (b%1)").arg(sg.branch);
        if (!sg.device.isEmpty()) label += QLatin1Char(' ') + sg.device;
        segNode[sg.nr] = label;
        nodes << label;
        kinds[label] = sg.device.isEmpty() ? GraphView::KindSegment
                                           : GraphView::KindSegDevice;
    }
    for (const auto& sg : ws.segs)
        edges.push_back({ segNode.value(sg.nr),
                          sg.outlet > 0 ? segNode.value(sg.outlet, head) : head,
                          {} });

    // Connections, on their segment where there is one.
    QSet<QString> used;
    for (const auto& c : ws.conns) {
        QString label = QStringLiteral("(%1,%2,%3)").arg(c.i).arg(c.j).arg(c.k);
        // A cell can be perforated more than once; keep the names apart so the
        // two do not collapse into one box.
        if (used.contains(label)) {
            int n = 2;
            while (used.contains(label + QStringLiteral(" #%1").arg(n))) ++n;
            label += QStringLiteral(" #%1").arg(n);
        }
        used.insert(label);
        if (!c.open) label += QStringLiteral(" shut");
        nodes << label;
        kinds[label] = GraphView::KindConnection;
        edges.push_back({ label,
                          c.segment > 0 ? segNode.value(c.segment, head) : head,
                          {} });
    }

    // One window per well: asking again redraws it at the date now being
    // looked at rather than stacking a second copy of the same well.
    QDialog* dlg = wellWindows_.value(well);
    GraphView* gv = dlg ? dlg->findChild<GraphView*>() : nullptr;
    QLabel* cap = dlg ? dlg->findChild<QLabel*>() : nullptr;
    if (!dlg) {
        dlg = new QDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setWindowFlags(Qt::Window);      // a dialog is given no maximise
        auto* lay = new QVBoxLayout(dlg);
        cap = new QLabel;
        cap->setTextInteractionFlags(Qt::TextSelectableByMouse);
        lay->addWidget(cap);
        gv = new GraphView;
        lay->addWidget(gv, 1);
        dlg->resize(820, 620);
        wellWindows_.insert(well, dlg);
        connect(dlg, &QObject::destroyed, this, [this, well] { wellWindows_.remove(well); });
    }

    dlg->setWindowTitle(QStringLiteral("%1 - well structure").arg(ws.name));
    cap->setText(QStringLiteral("%1  -  %2  -  %3, %4 connection(s)%5")
        .arg(ws.name,
             ws.injector ? (shape.injectors.contains(ws.name)
                                ? injectName(shape.injectors.value(ws.name))
                                : QStringLiteral("injector"))
                         : QStringLiteral("producer"),
             ws.msw ? QStringLiteral("%1 segment(s)").arg(ws.segs.size())
                    : QStringLiteral("not segmented"))
        .arg(ws.conns.size())
        .arg(shape.when.isValid()
                 ? QStringLiteral("  -  at %1").arg(shape.when.toString(QStringLiteral("d MMM yyyy")))
                 : QString()));
    gv->setGraph(nodes, edges, QStringLiteral("this well has no connections"), kinds);
    if (dlg->isMinimized()) dlg->showNormal(); else dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void StructurePanel::exportPicture()
{
    if (graph_->isEmpty()) {
        status_->setText(QStringLiteral("nothing drawn to save"));
        return;
    }
    const QString base = QFileInfo(model_.deckPath).completeBaseName()
                         + (viewBox_->currentIndex() == 0 ? QStringLiteral("_groups")
                                                          : QStringLiteral("_network"));
    QString sel;
    QString f = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save picture"),
        QFileInfo(model_.deckPath).absolutePath() + QLatin1Char('/') + base,
        QStringLiteral("PDF (*.pdf);;PNG image (*.png)"), &sel);
    if (f.isEmpty()) return;

    const bool pdf = sel.startsWith(QStringLiteral("PDF"))
                     || f.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive);
    if (pdf && !f.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) f += QStringLiteral(".pdf");
    if (!pdf && !f.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) f += QStringLiteral(".png");

    const QSize on = graph_->size();
    bool ok = false;
    if (pdf) {
        QPdfWriter w(f);
        w.setPageSize(QPageSize(QSizeF(11.0, 11.0 * double(on.height()) / std::max(1, on.width())),
                                QPageSize::Inch, QStringLiteral("graph")));
        w.setPageMargins(QMarginsF(0, 0, 0, 0));
        w.setResolution(300);
        QPainter p(&w);
        ok = p.isActive();
        if (ok) graph_->render(p, QRectF(0, 0, w.width(), w.height()));
    } else {
        QImage img(on * 3, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::white);
        QPainter p(&img);
        graph_->render(p, QRectF(0, 0, img.width(), img.height()));
        p.end();
        ok = img.save(f);
    }
    status_->setText(ok ? QStringLiteral("wrote %1").arg(f)
                        : QStringLiteral("could not write %1").arg(f));
    status_->setStyleSheet(ok ? QString() : QStringLiteral("color:#8a1f1f;"));
}

void StructurePanel::applyFilter(const QString& needle)
{
    std::function<bool(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* it) {
        bool self = needle.isEmpty()
                    || it->text(0).contains(needle, Qt::CaseInsensitive);
        bool child = false;
        for (int i = 0; i < it->childCount(); ++i)
            child = visit(it->child(i)) || child;
        it->setHidden(!needle.isEmpty() && !self && !child);
        if (!needle.isEmpty() && child) it->setExpanded(true);
        return self || child;
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) visit(tree_->topLevelItem(i));
}

} // namespace flowgui
