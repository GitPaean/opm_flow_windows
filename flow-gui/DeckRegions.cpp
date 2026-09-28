/*
  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  DeckRegions implementation. Part of the opm_flow_windows harness;
  GPL v3+ (see repository LICENSE).
*/
#include "DeckRegions.h"

#include <QRegularExpression>
#include <QStringView>

#include <algorithm>
#include <cmath>

namespace flowgui {

namespace {

// A keyword line, as the deck editor's tree reads one: an upper-case token at
// column 0, alone on its line but for a comment.
const QRegularExpression kKeywordRe(QStringLiteral(
    R"(^([A-Z][A-Z0-9_-]{0,7})\s*(?:--.*)?$)"));

// A RUNSPEC maximum: which keyword and item hold it, and its default.
struct DimRef {
    const char* kw = nullptr;
    int item = 0;               // 1-based, as the manual numbers them
    int dflt = 0;               // 0: no default worth quoting
};

struct Spec {
    const char* name;
    const char* meaning;
    DimRef dim, alt;            // alt is read when dim's keyword is absent
    bool always = false;        // listed even when the deck never sets it
};

const Spec kSpecs[] = {
    { "SATNUM",  "saturation functions",        { "TABDIMS", 1, 1 }, {}, true },
    { "IMBNUM",  "imbibition saturation functions", { "TABDIMS", 1, 1 }, {} },
    { "KRNUMX",  "directional saturation, X",   { "TABDIMS", 1, 1 }, {} },
    { "KRNUMY",  "directional saturation, Y",   { "TABDIMS", 1, 1 }, {} },
    { "KRNUMZ",  "directional saturation, Z",   { "TABDIMS", 1, 1 }, {} },
    { "IMBNUMX", "directional imbibition, X",   { "TABDIMS", 1, 1 }, {} },
    { "IMBNUMY", "directional imbibition, Y",   { "TABDIMS", 1, 1 }, {} },
    { "IMBNUMZ", "directional imbibition, Z",   { "TABDIMS", 1, 1 }, {} },
    { "PVTNUM",  "PVT",                         { "TABDIMS", 2, 1 }, {}, true },
    { "EQLNUM",  "equilibration",               { "EQLDIMS", 1, 1 }, {}, true },
    { "FIPNUM",  "fluid-in-place reporting",    { "REGDIMS", 1, 1 }, { "TABDIMS", 5, 1 }, true },
    { "ENDNUM",  "end-point depth tables",      { "ENDSCALE", 3, 1 }, { "TABDIMS", 8, 1 } },
    { "ROCKNUM", "rock compaction",             { "ROCKCOMP", 2, 1 }, { "TABDIMS", 13, 0 } },
    { "MISCNUM", "miscibility",                 { "MISCIBLE", 1, 1 }, {} },
    { "EOSNUM",  "reservoir EOS",               { "TABDIMS", 9, 1 }, {} },
    { "MULTNUM", "inter-region multipliers",    {}, {} },
    { "FLUXNUM", "flux regions",                { "REGDIMS", 4, 0 }, {} },
    { "OPERNUM", "OPERATER regions",            { "REGDIMS", 7, 0 }, {} },
};

// Keywords whose defaults hold whether or not the deck gives them.
bool alwaysDimensioned(const QString& kw)
{
    return kw == QLatin1String("TABDIMS") || kw == QLatin1String("EQLDIMS")
        || kw == QLatin1String("REGDIMS");
}

bool isFipSet(const QString& kw)
{
    // FIPxxx, but not the keywords that merely start the same way.
    return kw.startsWith(QLatin1String("FIP")) && kw.size() >= 4
        && kw != QLatin1String("FIPOWG") && kw != QLatin1String("FIPSEP")
        && kw != QLatin1String("FIP_PROBE");
}

const Spec* specOf(const QString& kw)
{
    for (const Spec& s : kSpecs)
        if (kw == QLatin1String(s.name)) return &s;
    return nullptr;
}

bool isRegionArray(const QString& kw) { return specOf(kw) || isFipSet(kw); }

// Cells a box-limited assignment leaves alone keep this.
bool defaultsToOne(const QString& kw)
{
    static const QStringList one = {
        QStringLiteral("SATNUM"), QStringLiteral("IMBNUM"), QStringLiteral("PVTNUM"),
        QStringLiteral("EQLNUM"), QStringLiteral("FIPNUM"), QStringLiteral("ENDNUM"),
        QStringLiteral("EOSNUM"), QStringLiteral("MULTNUM"), QStringLiteral("FLUXNUM") };
    return one.contains(kw) || isFipSet(kw);
}

bool isSingleRecord(const QString& kw)
{
    static const QStringList kws = {
        QStringLiteral("DIMENS"), QStringLiteral("COMPS"), QStringLiteral("BOX"),
        QStringLiteral("TABDIMS"), QStringLiteral("EQLDIMS"), QStringLiteral("REGDIMS"),
        QStringLiteral("ROCKCOMP"), QStringLiteral("MISCIBLE"), QStringLiteral("ENDSCALE") };
    return kws.contains(kw);
}

bool isMultiRecord(const QString& kw)
{
    static const QStringList kws = {
        QStringLiteral("EQUALS"), QStringLiteral("COPY"), QStringLiteral("ADD"),
        QStringLiteral("MULTIPLY"), QStringLiteral("MAXVALUE"), QStringLiteral("MINVALUE"),
        QStringLiteral("OPERATE"), QStringLiteral("OPERATER"), QStringLiteral("FIELDSEP") };
    return kws.contains(kw);
}

std::optional<int> toInt(QStringView t)
{
    bool ok = false;
    const int i = t.toInt(&ok);
    if (ok) return i;
    const double d = t.toDouble(&ok);      // "2." or "2.0" still names region 2
    if (ok && std::isfinite(d)) return int(std::lround(d));
    return std::nullopt;
}

} // namespace

QString regionNumbersText(const QList<int>& numbers)
{
    QStringList parts;
    for (qsizetype i = 0; i < numbers.size();) {
        qsizetype j = i;
        while (j + 1 < numbers.size() && numbers[j + 1] == numbers[j] + 1) ++j;
        parts << (j == i ? QString::number(numbers[i])
                         : QStringLiteral("%1-%2").arg(numbers[i]).arg(numbers[j]));
        i = j + 1;
    }
    return parts.join(QStringLiteral(", "));
}

void RegionScanner::reset() { *this = RegionScanner(); }

void RegionScanner::beginFile(const QString& path)
{
    // A keyword's data never runs across an INCLUDE boundary.
    startKeyword(QString(), 0);
    files_ << path;
}

void RegionScanner::endFile()
{
    startKeyword(QString(), 0);
    if (!files_.isEmpty()) files_.removeLast();
}

void RegionScanner::startKeyword(const QString& kw, int lineNo)
{
    // Whatever was open is complete, '/' or not.
    if (kind_ == Kind::Single && !rec_.empty()) applyRecord();
    if (kind_ != Kind::Skip) finishKeyword();

    kw_ = kw;
    kwLine_ = lineNo;
    if (kw == QLatin1String("ENDBOX")) { box_.reset(); kind_ = Kind::Skip; return; }
    kind_ = kw.isEmpty()          ? Kind::Skip
          : isRegionArray(kw)     ? Kind::Array
          : isSingleRecord(kw)    ? Kind::Single
          : isMultiRecord(kw)     ? Kind::Multi
                                  : Kind::Skip;
}

void RegionScanner::feed(const QString& line, int lineNo)
{
    if (const auto m = kKeywordRe.match(line); m.hasMatch()) {
        startKeyword(m.captured(1), lineNo);
        return;
    }
    if (kind_ == Kind::Skip) return;

    const QStringView s(line);
    const qsizetype n = s.size();
    for (qsizetype i = 0; i < n;) {
        const QChar c = s[i];
        if (c.isSpace()) { ++i; continue; }
        if (c == QLatin1Char('-') && i + 1 < n && s[i + 1] == QLatin1Char('-')) return;
        if (!recordOpen_) { recordOpen_ = true; recLine_ = lineNo; }
        if (c == QLatin1Char('/')) { closeRecord(); return; }   // the rest is comment
        QStringView tok;
        if (c == QLatin1Char('\'')) {
            const qsizetype e = s.indexOf(QLatin1Char('\''), i + 1);
            const qsizetype end = e < 0 ? n : e;
            tok = s.mid(i + 1, end - i - 1);
            i = end + 1;
        } else {
            qsizetype j = i;
            while (j < n && !s[j].isSpace() && s[j] != QLatin1Char('/')
                   && s[j] != QLatin1Char('\''))
                ++j;
            tok = s.mid(i, j - i);
            i = j;
        }
        if (kind_ == Kind::Array) { addValue(tok); continue; }
        // n* defaults n items; n*v repeats v.
        const qsizetype star = c == QLatin1Char('\'') ? -1 : tok.indexOf(QLatin1Char('*'));
        const auto times = star > 0 ? toInt(tok.left(star)) : std::nullopt;
        if (times && *times > 0 && *times < 100) {
            const QStringView v = tok.mid(star + 1);
            for (int k = 0; k < *times; ++k)
                rec_.push_back(v.isEmpty() ? Item() : Item(v.toString()));
        } else {
            rec_.push_back(tok.toString());
        }
    }
}

void RegionScanner::addValue(QStringView token)
{
    const qsizetype star = token.indexOf(QLatin1Char('*'));
    const QStringView v = star < 0 ? token : token.mid(star + 1);
    if (v.isEmpty()) return;               // n* assigns nothing
    if (const auto x = toInt(v)) values_.insert(*x);
}

void RegionScanner::closeRecord()
{
    switch (kind_) {
    case Kind::Array:
        finishKeyword();
        break;
    case Kind::Single:
        applyRecord();
        finishKeyword();
        break;
    case Kind::Multi:
        if (rec_.empty()) finishKeyword();   // the empty record ends the list
        else              applyRecord();
        break;
    case Kind::Skip:
        break;
    }
    rec_.clear();
    recordOpen_ = false;
}

void RegionScanner::finishKeyword()
{
    if (kind_ == Kind::Array && !values_.empty()) {
        recLine_ = kwLine_;
        assign(kw_, values_, boxPartial());
    }
    kind_ = Kind::Skip;
    kw_.clear();
    values_.clear();
    rec_.clear();
    recordOpen_ = false;
}

std::optional<int> RegionScanner::intAt(std::size_t i) const
{
    return i < rec_.size() && rec_[i] ? toInt(*rec_[i]) : std::nullopt;
}

QString RegionScanner::nameAt(std::size_t i) const
{
    return i < rec_.size() && rec_[i] ? rec_[i]->trimmed().toUpper() : QString();
}

bool RegionScanner::boxPartial() const
{
    return box_ && (nx_ == 0 || *box_ != fullBox());
}

// A record's own box, items from..from+5, each defaulted one taken from the
// current BOX or the whole grid.
bool RegionScanner::partialBox(std::size_t from) const
{
    Box b = box_.value_or(fullBox());
    bool given = false;
    for (std::size_t i = 0; i < 6; ++i)
        if (const auto v = intAt(from + i)) { b[i] = *v; given = true; }
    if (nx_ > 0) return b != fullBox();
    return given || box_.has_value();
}

std::set<int> RegionScanner::numbersOf(const QString& name) const
{
    if (const auto it = arrays_.find(name); it != arrays_.end() && it->second.assigned)
        return it->second.numbers;
    return defaultsToOne(name) ? std::set<int>{ 1 } : std::set<int>{};
}

void RegionScanner::assign(const QString& name, const std::set<int>& numbers,
                           bool partial, bool unsure)
{
    Array& a = arrays_[name];
    if (!a.assigned) {
        a.file = files_.isEmpty() ? QString() : files_.last();
        a.line = recLine_;
        // The cells outside the box keep the default.
        if (partial && defaultsToOne(name)) a.numbers = { 1 };
    }
    if (partial) {
        a.numbers.insert(numbers.begin(), numbers.end());
        a.approx = a.approx || unsure;
    } else {
        a.numbers = numbers;
        a.approx = unsure;
    }
    a.assigned = true;
}

void RegionScanner::applyRecord()
{
    const QString kw = kw_;
    if (kw == QLatin1String("DIMENS")) {
        nx_ = intAt(0).value_or(0); ny_ = intAt(1).value_or(0); nz_ = intAt(2).value_or(0);
        return;
    }
    if (kw == QLatin1String("COMPS")) { comps_ = intAt(0).value_or(0); return; }
    if (kw == QLatin1String("BOX")) {
        Box b = fullBox();
        for (std::size_t i = 0; i < 6; ++i)
            if (const auto v = intAt(i)) b[i] = *v;
        box_ = b;
        return;
    }
    if (kind_ == Kind::Single) {
        dims_[kw] = { rec_, files_.isEmpty() ? QString() : files_.last(), kwLine_ };
        return;
    }
    if (kw == QLatin1String("FIELDSEP")) {
        if (sepFile_.isEmpty()) { sepFile_ = files_.value(files_.size() - 1); sepLine_ = recLine_; }
        if (const auto e = intAt(7)) surfaceEos_.insert(*e);
        return;
    }

    const QString name = nameAt(0);
    if (kw == QLatin1String("EQUALS")) {
        if (const auto v = intAt(1); v && isRegionArray(name))
            assign(name, { *v }, partialBox(2));
        return;
    }
    if (kw == QLatin1String("COPY")) {
        const QString dst = nameAt(1);
        if (!isRegionArray(dst)) return;
        const auto src = arrays_.find(name);
        const bool srcSure = isRegionArray(name)
            && (src == arrays_.end() || !src->second.approx);
        assign(dst, numbersOf(name), partialBox(2), !srcSure);
        return;
    }
    if (kw == QLatin1String("OPERATE") || kw == QLatin1String("OPERATER")) {
        // Arithmetic on other arrays: the numbers it leaves are not followed.
        if (isRegionArray(name)) {
            Array& a = arrays_[name];
            if (!a.assigned) {
                a.file = files_.value(files_.size() - 1);
                a.line = recLine_;
                a.numbers = numbersOf(name);
                a.assigned = true;
            }
            a.approx = true;
        }
        return;
    }
    // ADD, MULTIPLY, MAXVALUE, MINVALUE
    const auto v = intAt(1);
    if (!v || !isRegionArray(name)) return;
    std::set<int> out;
    for (const int x : numbersOf(name)) {
        out.insert(kw == QLatin1String("ADD")      ? x + *v
                 : kw == QLatin1String("MULTIPLY") ? x * *v
                 : kw == QLatin1String("MAXVALUE") ? std::min(x, *v)
                                                   : std::max(x, *v));
    }
    const bool partial = partialBox(2);
    // Box-limited, the old numbers may survive outside the box or may not.
    assign(name, out, partial, partial);
}

std::optional<int> RegionScanner::dim(const QString& kw, std::size_t item) const
{
    const auto it = dims_.find(kw);
    if (it == dims_.end() || item == 0 || item > it->items.size() || !it->items[item - 1])
        return std::nullopt;
    return toInt(*it->items[item - 1]);
}

DeckRegions RegionScanner::result() const
{
    DeckRegions out;
    out.nx = nx_; out.ny = ny_; out.nz = nz_;
    out.comps = comps_;

    const auto declare = [this](RegionCount& c, DimRef d, DimRef alt) {
        for (const DimRef& r : { d, alt }) {
            if (!r.kw) continue;
            const QString kw = QString::fromLatin1(r.kw);
            if (!dims_.contains(kw)) continue;
            const auto v = dim(kw, std::size_t(r.item));
            c.declared = v.value_or(r.dflt);
            c.declaredDefault = !v;
            c.declaredBy = QStringLiteral("%1 item %2").arg(kw).arg(r.item);
            return;
        }
        // Neither given: only a keyword that is always in force has a default
        // that means anything.
        for (const DimRef& r : { d, alt }) {
            if (!r.kw || r.dflt <= 0 || !alwaysDimensioned(QString::fromLatin1(r.kw)))
                continue;
            c.declared = r.dflt;
            c.declaredDefault = true;
            c.declaredBy = QStringLiteral("%1 item %2").arg(QString::fromLatin1(r.kw)).arg(r.item);
            return;
        }
    };
    const auto where = [this](RegionCount& c, const QString& kw) {
        if (const auto it = dims_.find(kw); it != dims_.end()) {
            c.file = it->file;
            c.line = it->line;
        }
    };
    const auto fill = [this, &declare, &where](RegionCount& c, const QString& name,
                                               const Spec& s) {
        if (const auto it = arrays_.find(name); it != arrays_.end() && it->second.assigned) {
            for (const int x : it->second.numbers) if (x > 0) c.numbers << x;
            c.approx = it->second.approx;
            c.file = it->second.file;
            c.line = it->second.line;
        } else {
            c.defaulted = true;
        }
        declare(c, s.dim, s.alt);
        if (c.file.isEmpty() && s.dim.kw) where(c, QString::fromLatin1(s.dim.kw));
    };

    for (const Spec& s : kSpecs) {
        const QString name = QString::fromLatin1(s.name);
        const bool set = arrays_.count(name) && arrays_.at(name).assigned;
        const bool eos = name == QLatin1String("EOSNUM") && comps_ > 0;
        if (!eos && (s.always || set)) {
            RegionCount c;
            c.name = name;
            c.meaning = QString::fromLatin1(s.meaning);
            fill(c, name, s);
            out.regions << c;
        }
        // The user's own FIP sets follow FIPNUM, which they are declared by.
        if (name == QLatin1String("FIPNUM")) {
            for (const auto& [fip, arr] : arrays_) {
                if (fip == name || !isFipSet(fip) || !arr.assigned) continue;
                RegionCount c;
                c.name = fip;
                c.meaning = QStringLiteral("fluid-in-place reporting, own set");
                fill(c, fip, s);
                out.regions << c;
            }
        }
        if (eos) {
            RegionCount c;
            c.name = QStringLiteral("reservoir EOS");
            c.meaning = QStringLiteral("EOSNUM");
            fill(c, name, s);
            out.eos << c;
        }
    }

    if (comps_ > 0) {
        RegionCount c;
        c.name = QStringLiteral("surface EOS");
        c.meaning = QStringLiteral("FIELDSEP item 8");
        for (const int x : surfaceEos_) if (x > 0) c.numbers << x;
        c.file = sepFile_;
        c.line = sepLine_;
        declare(c, { "TABDIMS", 10, 1 }, {});
        if (c.file.isEmpty()) where(c, QStringLiteral("TABDIMS"));
        out.eos << c;
    }
    return out;
}

} // namespace flowgui
