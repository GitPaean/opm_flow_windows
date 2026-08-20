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
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QThread>
#include <QTimeZone>
#include <QTimer>
#include <QTreeWidget>
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
#include <opm/input/eclipse/Schedule/Schedule.hpp>
#include <opm/utility/GroupStructureViz.hpp>

#include <algorithm>
#include <exception>
#include <functional>
#include <memory>

namespace flowgui {

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
    return bits.join(QLatin1Char(';')) + QStringLiteral("|") + nb.join(QLatin1Char(';'))
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
        for (const auto& w : g.wells())  n.wells       << QString::fromStdString(w);
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

        Opm::EclipseState es(deck);
        if (stop()) { out.problem = QStringLiteral("cancelled"); return out; }
        tick(55);

        auto python = std::make_shared<Opm::Python>();
        Opm::Schedule sched(deck, es, ctx, guard, python);
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
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::white);
    setPalette(pal);
}

void GraphView::setGraph(const QStringList& nodes, const QVector<Edge>& edges,
                         const QString& emptyText)
{
    nodes_ = nodes; edges_ = edges; empty_ = emptyText;
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
void GraphView::relayout()
{
    placed_.clear();
    maxDepth_ = 0;
    if (nodes_.isEmpty()) return;

    QHash<QString, QStringList> parents;   // node -> the nodes it points at
    for (const auto& e : edges_) parents[e.from] << e.to;

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

    QHash<QString, double> xs;
    for (auto& layer : layers)
        for (int i = 0; i < layer.size(); ++i)
            xs[layer[i]] = layer.size() > 1 ? double(i) / (layer.size() - 1) : 0.5;

    for (int sweep = 0; sweep < 4; ++sweep) {
        for (int d = 1; d <= maxDepth_; ++d) {
            QStringList& layer = layers[d];
            QHash<QString, double> bary;
            for (const auto& n : layer) {
                const QStringList ups = parents.value(n);
                if (ups.isEmpty()) { bary[n] = xs.value(n); continue; }
                double sum = 0; int cnt = 0;
                for (const auto& u : ups) { sum += xs.value(u, 0.5); ++cnt; }
                bary[n] = cnt ? sum / cnt : xs.value(n);
            }
            std::sort(layer.begin(), layer.end(),
                      [&bary](const QString& a, const QString& b) { return bary[a] < bary[b]; });
            for (int i = 0; i < layer.size(); ++i)
                xs[layer[i]] = layer.size() > 1 ? double(i) / (layer.size() - 1) : 0.5;
        }
    }

    for (const auto& n : nodes_) {
        Placed p;
        p.name = n;
        p.depth = depth.value(n);
        p.x = xs.value(n, 0.5);
        placed_.push_back(p);
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

    QFont nf = p.font(); nf.setPointSizeF(9.0); nf.setBold(true);
    const QFontMetricsF fm(nf);
    const double boxH = fm.height() + 10;
    const double rowGap = maxDepth_ > 0
        ? (area.height() - boxH - 24) / maxDepth_ : 0.0;

    auto boxOf = [&](const Placed& q) {
        const double w = std::max(54.0, fm.horizontalAdvance(q.name) + 18);
        const double cx = area.left() + 20 + q.x * std::max(1.0, area.width() - 40 - w) + w / 2;
        const double cy = area.top() + 12 + q.depth * rowGap + boxH / 2;
        return QRectF(cx - w / 2, cy - boxH / 2, w, boxH);
    };
    auto find = [&](const QString& n) -> const Placed* {
        for (const auto& q : placed_) if (q.name == n) return &q;
        return nullptr;
    };

    // Edges first, so a box always sits on top of the lines that reach it.
    QFont ef = p.font(); ef.setPointSizeF(7.5); ef.setBold(false);
    for (const auto& e : edges_) {
        const Placed* a = find(e.from);
        const Placed* b = find(e.to);
        if (!a || !b) continue;
        const QRectF ra = boxOf(*a), rb = boxOf(*b);
        const QPointF from(ra.center().x(), ra.top());
        const QPointF to(rb.center().x(), rb.bottom());
        p.setPen(QPen(QColor(0x9a, 0xa3, 0xac), 1.4));
        // A gentle S rather than a straight line: with several children meeting
        // one parent, straight lines converge into an unreadable star.
        QPainterPath path(from);
        const double midY = (from.y() + to.y()) / 2;
        path.cubicTo(QPointF(from.x(), midY), QPointF(to.x(), midY), to);
        p.drawPath(path);
        if (!e.label.isEmpty()) {
            p.setFont(ef);
            p.setPen(QColor(0x8a, 0x6d, 0x3b));
            p.drawText(QRectF((from.x() + to.x()) / 2 - 34, midY - 8, 68, 14),
                       Qt::AlignCenter, e.label);
        }
    }

    p.setFont(nf);
    for (const auto& q : placed_) {
        const QRectF r = boxOf(q);
        const bool root = (q.depth == 0);
        const bool hot  = (!highlight_.isEmpty() && q.name == highlight_);
        p.setPen(QPen(hot ? QColor(0xd5, 0x5e, 0x00) : QColor(0x7a, 0x86, 0x92),
                      hot ? 2.0 : 1.2));
        p.setBrush(root ? QColor(0x00, 0x3c, 0x65)
                        : hot ? QColor(0xff, 0xf1, 0xdd) : QColor(0xe8, 0xf6, 0xef));
        p.drawRoundedRect(r, 4, 4);
        p.setPen(root ? Qt::white : QColor(0x22, 0x26, 0x2b));
        p.drawText(r, Qt::AlignCenter, q.name);
    }
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
    exportBtn_ = new QPushButton(QStringLiteral("Export Graphviz..."));
    exportBtn_->setEnabled(false);
    picBtn_ = new QPushButton(QStringLiteral("Save picture..."));
    picBtn_->setEnabled(false);
    picBtn_->setToolTip(QStringLiteral(
        "write the drawing as it stands to PNG or PDF. The same painting code "
        "draws the screen and the file, so the file is what you are looking "
        "at - vector in the PDF, at whatever size you ask for in the PNG."));
    exportBtn_->setToolTip(QStringLiteral(
        "write the group structure as Graphviz .gv, through opm-common's own "
        "writer - the same files the wellgraph tool produces.\n\n"
        "Render with:  dot -Tpdf <file> -o out.pdf"));
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
    row->addWidget(picBtn_);
    row->addWidget(exportBtn_);
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
    tree_->setMinimumWidth(340);
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
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    // After the first layout, not during it: sizes set on a splitter that has
    // no size yet are rescaled out of all recognition once it gets one.
    QTimer::singleShot(0, this, [split] { split->setSizes({ 420, 780 }); });

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
    connect(exportBtn_, &QPushButton::clicked, this, [this] { exportGraphviz(); });
    connect(picBtn_, &QPushButton::clicked, this, [this] { exportPicture(); });
    connect(viewBox_, &QComboBox::currentIndexChanged, this, [this](int) { refreshGraph(); });
    // Selecting in the tree marks the same node in the drawing, which is the
    // cheapest way to tie a name in a list to a box in a picture.
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        auto* it = tree_->currentItem();
        graph_->setHighlight(it ? it->text(0) : QString());
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
        exportBtn_->setEnabled(false);
        picBtn_->setEnabled(false);
        tree_->clear();
        graph_->setGraph({}, {}, QStringLiteral("no deck loaded"));
        return;
    }
    status_->setStyleSheet(QString());
    exportBtn_->setEnabled(true);
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
            for (const auto& c : g->childGroups) add(c, it);
            if (!showWells_->isChecked()) return;
            for (const auto& w : g->wells) {
                auto* wi = new QTreeWidgetItem(it);
                wi->setText(0, w);
                wi->setForeground(0, QBrush(QColor(0x24, 0x35, 0x8a)));
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

// Hand the deck to opm-common's own writer, so what comes out is the same
// artefact its wellgraph tool produces - the same files, renderable the same
// way, comparable with anyone else's. Graphviz does a layout job no hand-rolled
// painter here is going to match on a field with hundreds of groups.
//
// It needs a Schedule, which the extraction does not keep: holding an
// EclipseState open for the life of a tab to save half a second on a rare
// action is a poor trade. So it parses again, on this thread, behind a wait
// cursor.
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

    if (viewBox_->currentIndex() == 0) {          // the group hierarchy
        for (const auto& g : s.groups) {
            nodes << g.name;
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
                    edges.push_back({ w, g.name, {} });
                }
        graph_->setGraph(nodes, edges, QStringLiteral("this deck defines no groups"));
    } else {                                       // the network
        nodes = s.netNodes;
        for (const auto& b : s.branches)
            edges.push_back({ b.down, b.up,
                              b.vfp > 0 ? QStringLiteral("VFP %1").arg(b.vfp) : QString() });
        graph_->setGraph(nodes, edges,
                         QStringLiteral("this deck defines no network"));
    }
}

// Straight to PNG or PDF, from the painter that draws the screen - so the file
// is the picture, not a second rendering of it that can drift from it. The PDF
// is vector, which is what a report wants; the PNG is drawn at three times the
// on-screen size so it survives being scaled.
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

void StructurePanel::exportGraphviz()
{
    if (model_.deckPath.isEmpty()) return;
    const QString suggested = QFileInfo(model_.deckPath).absolutePath()
                              + QLatin1Char('/')
                              + QFileInfo(model_.deckPath).completeBaseName();
    QString base = QFileDialog::getSaveFileName(
        this, QStringLiteral("Write Graphviz files (a base name)"), suggested,
        QStringLiteral("Graphviz (*.gv);;All files (*)"));
    if (base.isEmpty()) return;
    // The writer appends its own "_well_groups.gv" / "_group_structure.gv".
    if (base.endsWith(QStringLiteral(".gv"), Qt::CaseInsensitive)) base.chop(3);

    status_->setText(QStringLiteral("writing Graphviz for %1 ...")
                         .arg(QFileInfo(model_.deckPath).fileName()));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString problem;
    try {
        auto ctx = relaxedContext();
        Opm::ErrorGuard guard;
        Opm::Parser parser;
        const auto deck = parser.parseFile(model_.deckPath.toStdString(), ctx, guard);
        Opm::EclipseState es(deck);
        auto python = std::make_shared<Opm::Python>();
        Opm::Schedule sched(deck, es, ctx, guard, python);
        // Separate files when the wells are hidden here: that is the same
        // judgement, and the tool's own help gives the same advice for a field
        // with many of them.
        Opm::writeWellGroupGraph(sched, base.toStdString(), !showWells_->isChecked());
    } catch (const std::exception& e) {
        problem = QString::fromLocal8Bit(e.what());
    } catch (...) {
        problem = QStringLiteral("unknown error");
    }
    QApplication::restoreOverrideCursor();
    status_->setText(problem.isEmpty()
        ? QStringLiteral("wrote %1_*.gv   -   render with:  dot -Tpdf %1_well_groups.gv "
                         "-o out.pdf").arg(base)
        : QStringLiteral("could not write Graphviz: %1").arg(problem));
    status_->setStyleSheet(problem.isEmpty() ? QString()
                                             : QStringLiteral("color:#8a1f1f;"));
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
