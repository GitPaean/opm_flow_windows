/*
  DeckRegions - how many regions of each kind a deck sets up, read from its
  text alone.

  Not through opm-common's EclipseState: a deck using something the simulator
  does not support yet - several EOS regions, say - fails to load there, and
  that is exactly the kind of deck whose regions are worth checking.

  A count is the number of distinct region numbers the deck assigns, over
  every cell of the grid, active or not. Whole arrays, EQUALS, COPY, ADD,
  MULTIPLY, MAXVALUE/MINVALUE and BOX are followed. Numbers are tracked as a
  set, not per cell, so an arithmetic edit confined to a box, or an OPERATE,
  leaves the count approximate - and flagged as such.

  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QVector>

#include <array>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace flowgui {

struct RegionCount {
    QString name;               // SATNUM, FIPZON, ... or "reservoir EOS"
    QString meaning;            // what the region number selects
    QList<int> numbers;         // distinct region numbers in use, ascending
    bool defaulted = false;     // never assigned: every cell is region 1
    bool approx = false;        // an edit was not followed exactly
    int  declared = 0;          // the RUNSPEC maximum, 0 where none applies
    bool declaredDefault = false;   // ... taken from the keyword's default
    QString declaredBy;         // "TABDIMS item 1"
    QString file;               // where it is first assigned
    int  line = 0;

    int  count() const { return defaulted ? 1 : int(numbers.size()); }
    int  highest() const { return defaulted ? 1 : numbers.isEmpty() ? 0 : numbers.last(); }
    bool overDeclared() const { return declared > 0 && highest() > declared; }
};

struct DeckRegions {
    int nx = 0, ny = 0, nz = 0;
    int comps = 0;                   // COMPS: > 0 for a compositional deck
    QVector<RegionCount> regions;    // the region arrays
    QVector<RegionCount> eos;        // reservoir and surface EOS, when compositional
};

// "1-3, 5, 8-9"
QString regionNumbersText(const QList<int>& numbers);

// Fed the deck one line at a time, in the order the simulator reads it, with
// each INCLUDE expanded where it stands - the walk the deck editor already
// makes to build its keyword tree.
class RegionScanner
{
public:
    void reset();
    void beginFile(const QString& path);
    void endFile();
    void feed(const QString& line, int lineNo);
    DeckRegions result() const;

private:
    enum class Kind { Skip, Array, Single, Multi };
    using Box = std::array<int, 6>;              // I1 I2 J1 J2 K1 K2, 1-based
    using Item = std::optional<QString>;         // empty where defaulted (n*)

    struct Array {
        std::set<int> numbers;
        bool assigned = false, approx = false;
        QString file;
        int line = 0;
    };
    struct Dims {
        std::vector<Item> items;
        QString file;
        int line = 0;
    };

    void startKeyword(const QString& kw, int lineNo);
    void closeRecord();
    void finishKeyword();
    void applyRecord();
    void addValue(QStringView token);
    void assign(const QString& name, const std::set<int>& numbers, bool partial,
                bool unsure = false);
    std::optional<int> intAt(std::size_t i) const;
    QString nameAt(std::size_t i) const;
    Box  fullBox() const { return { 1, nx_, 1, ny_, 1, nz_ }; }
    bool partialBox(std::size_t from) const;
    bool boxPartial() const;
    std::set<int> numbersOf(const QString& name) const;
    std::optional<int> dim(const QString& kw, std::size_t item) const;

    QStringList files_;               // the INCLUDE stack, innermost last
    QString kw_;
    Kind    kind_ = Kind::Skip;
    int     kwLine_ = 0;
    bool    recordOpen_ = false;
    int     recLine_ = 0;
    std::vector<Item> rec_;           // the record being read
    std::set<int>     values_;        // the region array being read

    int nx_ = 0, ny_ = 0, nz_ = 0, comps_ = 0;
    std::optional<Box> box_;
    std::map<QString, Array> arrays_;
    QHash<QString, Dims> dims_;
    std::set<int> surfaceEos_;        // FIELDSEP item 8
    QString sepFile_;
    int     sepLine_ = 0;
};

} // namespace flowgui
