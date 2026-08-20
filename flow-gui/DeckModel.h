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
#include <QColor>
#include <QFont>
#include <QPointF>
#include <QMap>
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
class QFontMetricsF;
class QMouseEvent;
class QPainter;
class QPushButton;
class QThread;
class QTimer;
class QTreeWidget;
class QLineEdit;

namespace flowgui {

// What an injector puts back into the reservoir.
enum class Inject { Water = 0, Gas, Oil, Multi };

// Spelled out for the tree, which has room to be precise where the key on the
// drawing groups oil and multi-phase together as "other".
QString injectName(Inject d);

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
    // Which of the wells inject, and what they put back. Kept per moment
    // rather than once for the run, because a well can be converted - and
    // converted from one phase to another - and the drawing should say what it
    // was doing at the date being looked at, not what it ends up doing.
    // Producers are simply absent. Ordered, so fingerprint() is stable.
    QMap<QString, Inject> injectors;
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
    // In the order the key lists them: the hierarchy, then what hangs off it.
    enum Kind { KindGroup = 0, KindWellGroup, KindProducer,
                KindInjWater, KindInjGas, KindInjOther, KindNetwork };

    explicit GraphView(QWidget* parent = nullptr);
    // `from` points at its parent/uptree node, so edges run child -> parent.
    void setGraph(const QStringList& nodes, const QVector<Edge>& edges,
                  const QString& emptyText,
                  const QHash<QString, int>& kinds = {});
    void setHighlight(const QString& node);
    // Paint at an arbitrary size, for export as well as for the screen.
    void render(QPainter& p, const QRectF& area) const;
    bool isEmpty() const { return placed_.isEmpty(); }

    // Put the key back in its default corner.
    void resetKey();

protected:
    void paintEvent(QPaintEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    void mouseDoubleClickEvent(QMouseEvent* ev) override;
    QSize minimumSizeHint() const override;

private:
    void relayout();
    // `thin` scales pen widths up when the drawing is scaled down, so lines
    // do not vanish on a field that has to be fitted into a small pane.
    void paintNode(QPainter& p, const QRectF& r, const QString& text,
                   int kind, double thin, bool hot) const;
    // Which kinds are on screen, in the order the key lists them; empty when
    // there is only one and so nothing to tell apart.
    QVector<int> keyKinds() const;
    // Where the key sits for a drawing of this size, default corner included.
    QRectF keyRect(const QRectF& area, const QFontMetricsF& fm) const;
    QFont  keyFont() const;
    // What scale the drawing is shown at, and where it lands.
    double fitScale(const QRectF& area) const;
    QRectF drawnRect(const QRectF& area) const;
    QFont  nodeFont() const;
    static QString kindName(int kind);
    static bool    isWellKind(int kind);
    static QPointF edgePoint(const QRectF& r, bool rect, const QPointF& towards);
    static void    drawArrow(QPainter& p, const QPointF& tip, const QPointF& from,
                             double size);

public:
    // The palette, shared with the tree pane so the two cannot drift apart.
    static void   kindColours(int kind, QColor& fill, QColor& line, QColor& ink);
    static QColor kindText(int kind);

private:

    // In the layout's own units: x is the node's centre, w its width.
    struct Placed { QString name; int depth = 0; double x = 0; double w = 0; };
    QHash<QString, int> kinds_;
    QStringList     nodes_;
    QVector<Edge>   edges_;
    QVector<Placed> placed_;
    QString         empty_;
    QString         highlight_;
    int             maxDepth_ = 0;
    // The drawing's natural size, which render() fits into the pane.
    double          natW_ = 0, natH_ = 0, natBoxH_ = 0, natRowGap_ = 0;
    // Normalised to the drawing area so it survives a resize and comes out of
    // the export where it went on screen. Negative x = the default corner.
    QPointF         keyPos_{ -1.0, -1.0 };
    bool            keyDrag_ = false;
    QPointF         keyGrab_;
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
