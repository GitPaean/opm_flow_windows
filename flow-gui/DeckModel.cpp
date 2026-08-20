/*
  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  DeckModel implementation. Part of the opm_flow_windows harness;
  GPL v3+ (see repository LICENSE).
*/
#include "DeckModel.h"

#include "GuiPaths.h"

#include <QApplication>

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
// NetworkView
// ===========================================================================

NetworkView::NetworkView(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(140);
}

void NetworkView::setStructure(const Structure* s)
{
    s_ = s;
    layoutNodes();
    update();
}

// Depth is distance to a root, following branches uptree. Nodes at the same
// depth share a row, which is enough structure to read the plumbing without
// a graph library.
void NetworkView::layoutNodes()
{
    placed_.clear();
    maxDepth_ = 0;
    if (!s_ || !s_->netActive || s_->netNodes.isEmpty()) return;

    auto uptreeOf = [this](const QString& n) -> QString {
        for (const auto& b : s_->branches) if (b.down == n) return b.up;
        return {};
    };
    QVector<int> depth(s_->netNodes.size(), 0);
    for (int i = 0; i < s_->netNodes.size(); ++i) {
        QString cur = s_->netNodes[i];
        int d = 0;
        // Bounded: a cycle would otherwise walk forever, and a malformed
        // network is not worth hanging the window over.
        for (int guard = 0; guard < 64; ++guard) {
            const QString up = uptreeOf(cur);
            if (up.isEmpty()) break;
            cur = up; ++d;
        }
        depth[i] = d;
        maxDepth_ = std::max(maxDepth_, d);
    }
    QVector<int> perRow(maxDepth_ + 1, 0);
    for (int d : depth) ++perRow[d];
    QVector<int> seen(maxDepth_ + 1, 0);
    for (int i = 0; i < s_->netNodes.size(); ++i) {
        Placed p;
        p.name = s_->netNodes[i];
        p.depth = depth[i];
        const int n = std::max(1, perRow[depth[i]]);
        p.x = (seen[depth[i]] + 0.5) / n;
        p.y = maxDepth_ > 0 ? double(depth[i]) / maxDepth_ : 0.5;
        ++seen[depth[i]];
        placed_.push_back(p);
    }
}

QSize NetworkView::minimumSizeHint() const { return { 260, 140 }; }

void NetworkView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::white);
    p.setRenderHint(QPainter::Antialiasing);
    if (!s_ || !s_->netActive || placed_.isEmpty()) {
        p.setPen(QColor(0x88, 0x8e, 0x94));
        p.drawText(rect(), Qt::AlignCenter,
                   s_ ? QStringLiteral("this deck defines no network")
                      : QStringLiteral("no deck loaded"));
        return;
    }
    const double mx = 60, my = 30;
    auto px = [&](const Placed& q) { return mx + q.x * (width() - 2 * mx); };
    // Depth 0 is a root - the node everything drains towards - and it goes at
    // the TOP, so the diagram reads the same way down as the group tree beside
    // it. y is already depth/maxDepth, so it needs no flipping.
    auto py = [&](const Placed& q) { return my + q.y * (height() - 2 * my); };
    auto at = [&](const QString& n) -> const Placed* {
        for (const auto& q : placed_) if (q.name == n) return &q;
        return nullptr;
    };

    QFont bf = p.font(); bf.setPointSizeF(7.5);
    for (const auto& b : s_->branches) {
        const Placed* d = at(b.down);
        const Placed* u = at(b.up);
        if (!d || !u) continue;
        const QPointF a(px(*d), py(*d)), c(px(*u), py(*u));
        p.setPen(QPen(QColor(0x7a, 0x86, 0x92), 1.6));
        p.drawLine(a, c);
        // Which VFP table the branch lifts through: the one number that says
        // what a branch actually does, and it is free to carry.
        if (b.vfp > 0) {
            p.setFont(bf);
            p.setPen(QColor(0x8a, 0x6d, 0x3b));
            p.drawText(QRectF((a.x() + c.x()) / 2 - 30, (a.y() + c.y()) / 2 - 8, 60, 14),
                       Qt::AlignCenter, QStringLiteral("VFP %1").arg(b.vfp));
        }
    }
    QFont f = p.font(); f.setPointSizeF(8.5); p.setFont(f);
    for (const auto& q : placed_) {
        const QPointF c(px(q), py(q));
        const bool root = (q.depth == 0);   // a terminal node, not a leaf
        p.setBrush(root ? QColor(0x00, 0x3c, 0x65) : QColor(0x14, 0xb9, 0x78));
        p.setPen(Qt::NoPen);
        p.drawEllipse(c, 6, 6);
        p.setPen(QColor(0x22, 0x26, 0x2b));
        p.drawText(QRectF(c.x() - 60, c.y() - 22, 120, 16),
                   Qt::AlignCenter, q.name);
    }
}

} // namespace flowgui

// ===========================================================================
// StructurePanel
// ===========================================================================
namespace flowgui {

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
    exportBtn_ = new QPushButton(QStringLiteral("Export Graphviz..."));
    exportBtn_->setEnabled(false);
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
    row->addWidget(exportBtn_);
    row->addWidget(bar_);
    row->addWidget(filter_, 1);

    status_ = new QLabel(QStringLiteral("open a *.DATA file to see its group tree"));
    status_->setWordWrap(true);
    netInfo_ = new QLabel;
    netInfo_->setStyleSheet(QStringLiteral("color:#555b61;"));

    tree_ = new QTreeWidget;
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({ QStringLiteral("Group / well"), QStringLiteral("Contents") });
    tree_->header()->setStretchLastSection(true);

    net_ = new NetworkView;

    auto* right = new QWidget;
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->addWidget(netInfo_);
    rl->addWidget(net_, 1);

    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(tree_);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setSizes({ 600, 420 });

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
        tree_->clear();
        net_->setStructure(nullptr);
        return;
    }
    status_->setStyleSheet(QString());
    exportBtn_->setEnabled(true);
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
    if (index < 0 || index >= model_.shapes.size()) { net_->setStructure(nullptr); return; }
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
    tree_->expandToDepth(1);
    tree_->resizeColumnToContents(0);

    net_->setStructure(&s);
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
