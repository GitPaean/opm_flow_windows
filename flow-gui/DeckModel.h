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
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <atomic>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
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
// Node-link drawing of the network. A network is not a tree - it can have
// several roots, and GRUPNET and BRANPROP describe different things - so it
// gets a diagram of its own rather than another indented list.
class NetworkView : public QWidget
{
    Q_OBJECT
public:
    explicit NetworkView(QWidget* parent = nullptr);
    void setStructure(const Structure* s);

protected:
    void paintEvent(QPaintEvent* ev) override;
    QSize minimumSizeHint() const override;

private:
    void layoutNodes();
    const Structure* s_ = nullptr;
    struct Placed { QString name; int depth = 0; double x = 0, y = 0; };
    QVector<Placed> placed_;
    int maxDepth_ = 0;
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
    void exportGraphviz();

    QPushButton*  openBtn_ = nullptr;
    QPushButton*  exportBtn_ = nullptr;
    QCheckBox*    showWells_ = nullptr;
    QComboBox*    shapeBox_ = nullptr;
    QLineEdit*    filter_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel*       status_ = nullptr;
    QLabel*       netInfo_ = nullptr;
    QTreeWidget*  tree_ = nullptr;
    NetworkView*  net_ = nullptr;
    QTimer*       poll_ = nullptr;
    QThread*      worker_ = nullptr;

    DeckStructure     model_;
    QString           pending_;
    std::atomic<int>  progress_{0};
    std::atomic<bool> cancel_{false};
};

} // namespace flowgui
