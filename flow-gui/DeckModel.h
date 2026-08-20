/*
  DeckModel - the shape of a field: which groups feed which, where the wells
  hang, and how the network is plumbed.

  Read from the deck through opm-common's own Parser and Schedule rather than
  by scanning text. GRUPTREE, WELSPECS, BRANPROP and GRUPNET all contribute to
  a structure that is rebuilt at every schedule step, and reproducing that
  faithfully by hand is a losing game - the parser is right here and it is what
  the simulator itself uses.

  Which is also why the structure is a sequence and not a picture: GRUPTREE
  lives in SCHEDULE and is free to change. Norne opens with three wells and
  finishes with dozens. Only the steps at which something actually changed are
  kept, so a 248-step run usually collapses to a handful of distinct shapes.

  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QDateTime>
#include <QHash>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <atomic>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPainter;
class QPushButton;
class QThread;
class QTimer;
class QTreeWidget;
class QLineEdit;

namespace flowgui {

struct GroupNode {
    QString     name;
    QString     parent;          // empty for FIELD
    QStringList childGroups;
    QStringList wells;
};

struct NetBranch {
    QString down, up;            // downtree node -> uptree node
    int     vfp = -1;            // VFP table the branch is lifted through, or -1
};

// The field's shape at one moment.
struct Structure {
    QDateTime         when;
    int               step = 0;      // schedule step it was first seen at
    QVector<GroupNode> groups;
    // Which of the wells inject. Kept per moment rather than once for the run,
    // because a well can be converted, and the drawing should say what it was
    // doing at the date being looked at rather than what it ends up doing.
    QStringList        injectors;
    QVector<NetBranch> branches;
    QStringList        netNodes;
    bool               netActive = false;
    bool               netStandard = false;   // GRUPNET rather than BRANPROP

    const GroupNode* find(const QString& name) const;
    int  wellCount() const;
    // Everything that decides whether two moments look the same, so a run of
    // identical steps can collapse to one entry.
    QString fingerprint() const;
};

struct DeckStructure {
    bool    ok = false;
    QString problem;
    QString deckPath;
    QVector<Structure> shapes;       // only where something changed
    int     scheduleSteps = 0;
    QStringList warnings;            // what the permissive parse let through
};

// Parse a deck and extract its structure over time. Slow enough to want a
// thread (about half a second for Norne, longer for a big deck), so `cancel`
// is polled between schedule steps.
DeckStructure readDeckStructure(const QString& dataFile,
                                std::atomic<bool>* cancel = nullptr,
                                std::atomic<int>* progress = nullptr);

// ---------------------------------------------------------------------------
// Node-link drawing, for the group tree and for the network alike. Both are
// directed graphs over named nodes, and neither is reliably a tree - a network
// can have several roots - so one widget draws both.
//
// The layout is done here rather than handed to Graphviz: keeping it in-process
// is what makes the drawing adjustable, and it is the same painting code that
// then writes the PNG and the PDF, so what is exported is exactly what is on
// screen.
class GraphView : public QWidget
{
    Q_OBJECT
public:
    struct Edge { QString from, to; QString label; };
    // What a node IS, which decides how it is drawn. A group that only holds
    // wells is the bottom of the management hierarchy and reads differently
    // from one that holds groups, so it is worth telling apart at a glance.
    enum Kind { KindGroup = 0, KindWellGroup, KindProducer, KindInjector,
                KindNetwork };

    explicit GraphView(QWidget* parent = nullptr);
    // `from` points at its parent/uptree node, so edges run child -> parent.
    void setGraph(const QStringList& nodes, const QVector<Edge>& edges,
                  const QString& emptyText,
                  const QHash<QString, int>& kinds = {});
    void setHighlight(const QString& node);
    // Paint at an arbitrary size, for export as well as for the screen.
    void render(QPainter& p, const QRectF& area) const;
    bool isEmpty() const { return placed_.isEmpty(); }

protected:
    void paintEvent(QPaintEvent* ev) override;
    QSize minimumSizeHint() const override;

private:
    void relayout();
    void paintNode(QPainter& p, const QRectF& r, const QString& text,
                   int kind, bool root, bool hot) const;
    static QString kindName(int kind);

    struct Placed { QString name; int depth = 0; double x = 0; };
    QHash<QString, int> kinds_;
    QStringList     nodes_;
    QVector<Edge>   edges_;
    QVector<Placed> placed_;
    QString         empty_;
    QString         highlight_;
    int             maxDepth_ = 0;
};

// ---------------------------------------------------------------------------
class StructurePanel : public QWidget
{
    Q_OBJECT
public:
    explicit StructurePanel(QWidget* parent = nullptr);
    ~StructurePanel() override;

    // Show this deck. Safe to call with a deck already loaded.
    void openDeck(const QString& dataFile);

private:
    void startLoad(const QString& dataFile);
    void finishLoad();
    void showShape(int index);
    void applyFilter(const QString& needle);
    void exportPicture();      // PNG or PDF, straight from the painter
    void refreshGraph();

    QPushButton*  openBtn_ = nullptr;
    QPushButton*  picBtn_ = nullptr;
    QCheckBox*    showWells_ = nullptr;
    QComboBox*    shapeBox_ = nullptr;
    QLineEdit*    filter_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel*       status_ = nullptr;
    QLabel*       netInfo_ = nullptr;
    QTreeWidget*  tree_ = nullptr;
    GraphView*    graph_ = nullptr;
    QComboBox*    viewBox_ = nullptr;   // group tree, or network
    QTimer*       poll_ = nullptr;
    QThread*      worker_ = nullptr;

    DeckStructure     model_;
    QString           pending_;
    std::atomic<int>  progress_{0};
    std::atomic<bool> cancel_{false};
};

} // namespace flowgui
