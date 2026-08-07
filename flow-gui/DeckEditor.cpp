/*
  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  DeckEditor implementation. Part of the opm_flow_windows harness; GPL v3+
  (see repository LICENSE).
*/
#include "DeckEditor.h"

#include "GuiPaths.h"

#include <QAction>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QSplitter>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <memory>

namespace {

const char* kSections[] = { "RUNSPEC", "GRID", "EDIT", "PROPS",
                            "REGIONS", "SOLUTION", "SUMMARY", "SCHEDULE" };

bool isSectionKw(const QString& kw)
{
    for (const char* s : kSections)
        if (kw == QLatin1String(s)) return true;
    return false;
}

// A keyword line: an uppercase token at column 0 (Eclipse convention),
// optionally followed only by whitespace/comment.
const QRegularExpression kKeywordRe(QStringLiteral(
    R"(^([A-Z][A-Z0-9_-]{0,7})\s*(?:--.*)?$)"));

// roles on tree items
constexpr int RoleFile = Qt::UserRole;
constexpr int RoleLine = Qt::UserRole + 1;

// Tree filter: an item stays visible if it or any descendant matches the
// needle (keyword or location column, case-insensitive); ancestors of a
// match are expanded so the hit is actually on screen.
bool filterItem(QTreeWidgetItem* it, const QString& needle)
{
    const bool selfMatch = needle.isEmpty()
        || it->text(0).contains(needle, Qt::CaseInsensitive)
        || it->text(1).contains(needle, Qt::CaseInsensitive);
    bool childMatch = false;
    for (int i = 0; i < it->childCount(); ++i)
        childMatch = filterItem(it->child(i), needle) || childMatch;
    it->setHidden(!needle.isEmpty() && !selfMatch && !childMatch);
    if (!needle.isEmpty() && childMatch) it->setExpanded(true);
    return selfMatch || childMatch;
}

// The file name a deck line refers to, if any: the quoted token of an
// INCLUDE argument line ('include/MULTZ.grdecl' /), the argument of a
// single-line "INCLUDE 'f' /", or a bare unquoted token. Comments and the
// record terminator are stripped; backslashes are accepted so decks written
// on Windows also resolve on Linux. *spanStart/*spanLen report where the
// token sits in the line (quotes included) so a click can be required to
// land on the file name itself rather than anywhere on the line.
QString deckFileToken(const QString& line, int* spanStart = nullptr,
                      int* spanLen = nullptr)
{
    if (spanStart) *spanStart = -1;
    if (spanLen)   *spanLen   = 0;
    QString t = line;
    const int c = t.indexOf(QLatin1String("--"));
    if (c >= 0) t = t.left(c);

    const int q1 = t.indexOf(QLatin1Char('\''));
    if (q1 >= 0) {                                  // quoted: 'path/file'
        const int q2 = t.indexOf(QLatin1Char('\''), q1 + 1);
        if (q2 <= q1 + 1) return {};
        if (spanStart) *spanStart = q1;
        if (spanLen)   *spanLen   = q2 - q1 + 1;
        QString v = t.mid(q1 + 1, q2 - q1 - 1).trimmed();
        v.replace(QLatin1Char('\\'), QLatin1Char('/'));
        return v;
    }
    // bare token, skipping a leading INCLUDE keyword and the terminator
    static const QRegularExpression wordRe(QStringLiteral(R"(\S+)"));
    auto it = wordRe.globalMatch(t);
    while (it.hasNext()) {
        const auto m = it.next();
        QString w = m.captured();
        if (w.compare(QLatin1String("INCLUDE"), Qt::CaseInsensitive) == 0) continue;
        if (w == QLatin1String("/")) break;
        if (w.endsWith(QLatin1Char('/'))) w.chop(1);
        if (w.isEmpty()) continue;
        if (spanStart) *spanStart = int(m.capturedStart());
        if (spanLen)   *spanLen   = w.size();
        w.replace(QLatin1Char('\\'), QLatin1Char('/'));
        return w;
    }
    return {};
}

void setExpandedRecursively(QTreeWidgetItem* it, bool expanded)
{
    it->setExpanded(expanded);
    for (int i = 0; i < it->childCount(); ++i)
        setExpandedRecursively(it->child(i), expanded);
}

} // namespace

// ===========================================================================
// DeckHighlighter
// ===========================================================================
DeckHighlighter::DeckHighlighter(QTextDocument* doc)
    : QSyntaxHighlighter(doc)
{}

void DeckHighlighter::highlightBlock(const QString& text)
{
    static const QRegularExpression numRe(QStringLiteral(
        R"(\b\d+\*?(?:\d+(?:\.\d*)?(?:[eEdD][+-]?\d+)?)?\b|\b\d*\.\d+(?:[eEdD][+-]?\d+)?\b)"));
    static const QRegularExpression strRe(QStringLiteral(R"('[^']*')"));

    QTextCharFormat numFmt;   numFmt.setForeground(QColor(0x0b, 0x66, 0x6b));
    QTextCharFormat strFmt;   strFmt.setForeground(QColor(0x9a, 0x30, 0x0e));
    QTextCharFormat kwFmt;    kwFmt.setForeground(QColor(0x24, 0x35, 0x8a));
    kwFmt.setFontWeight(QFont::Bold);
    QTextCharFormat secFmt;   secFmt.setForeground(QColor(0x8a, 0x1f, 0x6e));
    secFmt.setFontWeight(QFont::Bold);
    QTextCharFormat slashFmt; slashFmt.setForeground(QColor(0x24, 0x35, 0x8a));
    slashFmt.setFontWeight(QFont::Bold);
    QTextCharFormat comFmt;   comFmt.setForeground(QColor(0x4e, 0x7d, 0x3a));
    comFmt.setFontItalic(true);

    // numbers
    auto it = numRe.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        setFormat(m.capturedStart(), m.capturedLength(), numFmt);
    }
    // quoted strings
    it = strRe.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        setFormat(m.capturedStart(), m.capturedLength(), strFmt);
    }
    // keyword at column 0
    const auto km = kKeywordRe.match(text);
    if (km.hasMatch())
        setFormat(0, km.capturedLength(1),
                  isSectionKw(km.captured(1)) ? secFmt : kwFmt);
    // record terminator
    for (int i = 0; i < text.size(); ++i)
        if (text[i] == QLatin1Char('/')) setFormat(i, 1, slashFmt);
    // comment wins over everything after "--"
    const int c = text.indexOf(QLatin1String("--"));
    if (c >= 0) setFormat(c, text.size() - c, comFmt);
}

// ===========================================================================
// DeckTextEdit (line numbers)
// ===========================================================================
namespace {
class LineNumberArea : public QWidget
{
public:
    explicit LineNumberArea(DeckTextEdit* ed) : QWidget(ed), ed_(ed) {}
    QSize sizeHint() const override { return { ed_->lineNumberAreaWidth(), 0 }; }
protected:
    void paintEvent(QPaintEvent* ev) override { ed_->lineNumberAreaPaintEvent(ev); }
private:
    DeckTextEdit* ed_;
};
} // namespace

DeckTextEdit::DeckTextEdit(QWidget* parent)
    : QPlainTextEdit(parent)
{
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setLineWrapMode(QPlainTextEdit::NoWrap);
    lineArea_ = new LineNumberArea(this);
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this](int) {
        setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
    });
    connect(this, &QPlainTextEdit::updateRequest, this,
            [this](const QRect& r, int dy) {
        if (dy) lineArea_->scroll(0, dy);
        else    lineArea_->update(0, r.y(), lineArea_->width(), r.height());
    });
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

int DeckTextEdit::lineNumberAreaWidth() const
{
    int digits = 1;
    for (int m = qMax(1, blockCount()); m >= 10; m /= 10) ++digits;
    return 10 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void DeckTextEdit::mouseDoubleClickEvent(QMouseEvent* ev)
{
    QPlainTextEdit::mouseDoubleClickEvent(ev);   // keep the word selection
    emit doubleClickedAt(cursorForPosition(ev->pos()).position());
}

void DeckTextEdit::resizeEvent(QResizeEvent* ev)
{
    QPlainTextEdit::resizeEvent(ev);
    const QRect cr = contentsRect();
    lineArea_->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void DeckTextEdit::lineNumberAreaPaintEvent(QPaintEvent* ev)
{
    QPainter p(lineArea_);
    p.fillRect(ev->rect(), QColor(0xee, 0xf1, 0xf4));
    QTextBlock block = firstVisibleBlock();
    int top = int(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + int(blockBoundingRect(block).height());
    p.setPen(QColor(0x8a, 0x93, 0x9c));
    while (block.isValid() && top <= ev->rect().bottom()) {
        if (block.isVisible() && bottom >= ev->rect().top())
            p.drawText(0, top, lineArea_->width() - 4,
                       fontMetrics().height(), Qt::AlignRight,
                       QString::number(block.blockNumber() + 1));
        block = block.next();
        top = bottom;
        bottom = top + int(blockBoundingRect(block).height());
    }
}

// ===========================================================================
// DeckEditorWidget
// ===========================================================================
DeckEditorWidget::DeckEditorWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* top = new QVBoxLayout(this);

    {
        auto* row = new QHBoxLayout;
        auto* bopen = new QPushButton(QStringLiteral("Open DATA..."));
        auto* bsave = new QPushButton(QStringLiteral("Save"));
        auto* ball  = new QPushButton(QStringLiteral("Save all"));
        auto* bclose = new QPushButton(QStringLiteral("Close all"));
        bclose->setToolTip(QStringLiteral(
            "close every open file - following INCLUDEs leaves a lot of tabs "
            "behind, and the files themselves are not touched"));
        auto* brel  = new QPushButton(QStringLiteral("Reload"));
        brel->setToolTip(QStringLiteral(
            "re-read the current file from disk (picks up edits made outside "
            "the GUI; unmodified tabs reload by themselves)"));
        undoBtn_ = new QPushButton(QStringLiteral("Undo"));
        redoBtn_ = new QPushButton(QStringLiteral("Redo"));
        undoBtn_->setToolTip(QStringLiteral("undo the last edit in this file (Ctrl+Z)"));
        redoBtn_->setToolTip(QStringLiteral("redo the last undone edit (Ctrl+Y / Ctrl+Shift+Z)"));
        undoBtn_->setEnabled(false);
        redoBtn_->setEnabled(false);
        auto* bcom  = new QPushButton(QStringLiteral("Toggle comment"));
        bcom->setToolTip(QStringLiteral(
            "comment the selected lines, or uncomment them when they already "
            "are comments - the current line if nothing is selected (Ctrl+/)"));
        auto* bfind = new QPushButton(QStringLiteral("Find / Replace"));
        bfind->setToolTip(QStringLiteral(
            "search this file and replace matches (Ctrl+F finds, Ctrl+H replaces)"));
        auto* bscan = new QPushButton(QStringLiteral("Rescan structure"));
        row->addWidget(bopen); row->addWidget(bsave); row->addWidget(ball);
        row->addWidget(bclose); row->addWidget(brel);
        // Back and Forward through the places the caret has been. A deck is
        // read by hopping - a keyword in the tree, down into an INCLUDE, back
        // out - and the tab bar only remembers which file, never where in it.
        backBtn_ = new QPushButton(QStringLiteral("< Back"));
        fwdBtn_  = new QPushButton(QStringLiteral("Forward >"));
        backBtn_->setToolTip(QStringLiteral(
            "back to the last place you jumped from or were editing (Alt+Left)"));
        fwdBtn_->setToolTip(QStringLiteral("forward again (Alt+Right)"));
        backBtn_->setEnabled(false);
        fwdBtn_->setEnabled(false);
        row->addWidget(undoBtn_); row->addWidget(redoBtn_);
        row->addWidget(backBtn_); row->addWidget(fwdBtn_);
        row->addWidget(bcom); row->addWidget(bfind);
        row->addWidget(bscan); row->addStretch(1);
        top->addLayout(row);

        connect(backBtn_, &QPushButton::clicked, this, [this] { goJump(-1); });
        connect(fwdBtn_,  &QPushButton::clicked, this, [this] { goJump(+1); });
        for (const auto& [seq, dir] : { std::pair{ QKeySequence(Qt::ALT | Qt::Key_Left),  -1 },
                                        std::pair{ QKeySequence(Qt::ALT | Qt::Key_Right), +1 } }) {
            auto* s = new QShortcut(seq, this);
            s->setContext(Qt::WidgetWithChildrenShortcut);
            connect(s, &QShortcut::activated, this, [this, dir] { goJump(dir); });
        }

        connect(brel, &QPushButton::clicked, this,
                [this] { reloadTab(tabs_->currentIndex(), false); });
        connect(bcom, &QPushButton::clicked, this, [this] { toggleComment(); });
        connect(bfind, &QPushButton::clicked, this, [this] { showFindBar(true); });
        connect(undoBtn_, &QPushButton::clicked, this, [this] {
            if (auto* ed = editorAt(tabs_->currentIndex())) { ed->undo(); ed->setFocus(); }
        });
        connect(redoBtn_, &QPushButton::clicked, this, [this] {
            if (auto* ed = editorAt(tabs_->currentIndex())) { ed->redo(); ed->setFocus(); }
        });

        connect(bopen, &QPushButton::clicked, this, [this] {
            const QString f = QFileDialog::getOpenFileName(
                this, QStringLiteral("Open input deck"),
                flowgui::startDir(QStringLiteral("deck"), rootDeck_),
                QStringLiteral("Eclipse decks (*.DATA *.data);;All files (*)"));
            if (f.isEmpty()) return;
            flowgui::rememberDir(QStringLiteral("deck"), f);
            openDeck(f);
        });
        connect(bsave, &QPushButton::clicked, this, [this] { saveTab(tabs_->currentIndex()); });
        connect(ball,  &QPushButton::clicked, this, [this] { saveAll(); });
        connect(bclose, &QPushButton::clicked, this, [this] { closeAllTabs(); });
        connect(bscan, &QPushButton::clicked, this, [this] { scanDeck(); });
        auto* sc = new QShortcut(QKeySequence::Save, this);
        connect(sc, &QShortcut::activated, this, [this] { saveTab(tabs_->currentIndex()); });
    }

    auto* split = new QSplitter;
    {
        // Left pane: keyword filter + expand/collapse over the structure tree.
        auto* left = new QWidget;
        auto* ll = new QVBoxLayout(left);
        ll->setContentsMargins(0, 0, 0, 0);
        treeFilter_ = new QLineEdit;
        treeFilter_->setPlaceholderText(QStringLiteral("filter keywords..."));
        treeFilter_->setClearButtonEnabled(true);
        ll->addWidget(treeFilter_);

        auto* brow = new QHBoxLayout;
        auto* bexp    = new QPushButton(QStringLiteral("Expand"));
        bexp->setToolTip(QStringLiteral("expand the selected item and everything below it"));
        auto* bcol    = new QPushButton(QStringLiteral("Collapse"));
        bcol->setToolTip(QStringLiteral("collapse the selected item and everything below it"));
        auto* bexpAll = new QPushButton(QStringLiteral("Expand all"));
        auto* bcolAll = new QPushButton(QStringLiteral("Collapse all"));
        brow->addWidget(bexp); brow->addWidget(bcol);
        brow->addWidget(bexpAll); brow->addWidget(bcolAll);
        brow->addStretch(1);
        ll->addLayout(brow);

        tree_ = new QTreeWidget;
        tree_->setHeaderLabels({ QStringLiteral("Section / keyword"), QStringLiteral("Location") });
        tree_->setColumnWidth(0, 240);
        ll->addWidget(tree_, 1);
        split->addWidget(left);

        connect(bexp, &QPushButton::clicked, this, [this] {
            if (auto* it = tree_->currentItem()) setExpandedRecursively(it, true);
            else setStatus(QStringLiteral("select a section or include first (or use Expand all)"));
        });
        connect(bcol, &QPushButton::clicked, this, [this] {
            if (auto* it = tree_->currentItem()) setExpandedRecursively(it, false);
            else setStatus(QStringLiteral("select a section or include first (or use Collapse all)"));
        });
        connect(bexpAll, &QPushButton::clicked, tree_, &QTreeWidget::expandAll);
        connect(bcolAll, &QPushButton::clicked, tree_, &QTreeWidget::collapseAll);
        connect(treeFilter_, &QLineEdit::textChanged, this,
                [this](const QString& t) { filterTree(t.trimmed()); });
    }
    tabs_ = new QTabWidget;
    tabs_->setTabsClosable(true);
    tabs_->setDocumentMode(true);
    split->addWidget(tabs_);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({ 340, 700 });
    top->addWidget(split, 1);

    // Find bar (Ctrl+F): wrap-around search in the current editor tab with
    // all matches highlighted. Enter/F3 next, Shift+Enter/Shift+F3 previous,
    // Esc hides. Hidden until first use.
    {
        findBar_ = new QWidget;
        auto* bars = new QVBoxLayout(findBar_);
        bars->setContentsMargins(0, 0, 0, 0);
        bars->setSpacing(2);

        auto* findRow = new QWidget;
        auto* fl = new QHBoxLayout(findRow);
        fl->setContentsMargins(0, 0, 0, 0);
        fl->addWidget(new QLabel(QStringLiteral("Find:")));
        findEdit_ = new QLineEdit;
        findEdit_->setClearButtonEnabled(true);
        fl->addWidget(findEdit_, 1);
        auto* bprev = new QPushButton(QStringLiteral("Prev"));
        auto* bnext = new QPushButton(QStringLiteral("Next"));
        // Deck keywords are upper case, so an exact search is the useful
        // default; untick to fall back to case-insensitive matching.
        caseChk_ = new QCheckBox(QStringLiteral("match case"));
        caseChk_->setChecked(true);
        caseChk_->setToolTip(QStringLiteral(
            "search and replace exactly as typed (on), or ignoring case (off)"));
        fl->addWidget(caseChk_);
        // A deck is not one file, and the keyword you are hunting is as likely
        // to be three INCLUDEs down as in front of you. On, the search runs off
        // the end of this file into the next one the scan found, opening it as
        // it goes, and Replace All spans the lot.
        deckChk_ = new QCheckBox(QStringLiteral("whole deck"));
        deckChk_->setToolTip(QStringLiteral(
            "search every file of the scanned deck - the .DATA and each "
            "INCLUDE - instead of only this tab.\n\n"
            "Files are opened as they are reached, and Replace All leaves them "
            "open and modified: nothing is written until you Save."));
        fl->addWidget(deckChk_);
        // Visible way into replace - a shortcut alone is too easy to miss.
        replaceToggle_ = new QPushButton(QStringLiteral("Replace..."));
        replaceToggle_->setCheckable(true);
        replaceToggle_->setToolTip(QStringLiteral("show/hide the replace row (Ctrl+H)"));
        findInfo_ = new QLabel;
        findInfo_->setMinimumWidth(90);
        auto* bclose = new QPushButton(QStringLiteral("Close"));
        fl->addWidget(bprev); fl->addWidget(bnext);
        fl->addWidget(replaceToggle_);
        fl->addWidget(findInfo_); fl->addWidget(bclose);
        bars->addWidget(findRow);

        // Replace row: shown by Ctrl+H, hidden for a plain Ctrl+F search.
        replaceRow_ = new QWidget;
        auto* rl = new QHBoxLayout(replaceRow_);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->addWidget(new QLabel(QStringLiteral("Replace:")));
        replaceEdit_ = new QLineEdit;
        replaceEdit_->setClearButtonEnabled(true);
        rl->addWidget(replaceEdit_, 1);
        auto* brep    = new QPushButton(QStringLiteral("Replace"));
        auto* brepAll = new QPushButton(QStringLiteral("Replace all"));
        brep->setToolTip(QStringLiteral("replace the current match and jump to the next"));
        brepAll->setToolTip(QStringLiteral("replace every match in this file (one undo step)"));
        rl->addWidget(brep); rl->addWidget(brepAll);
        rl->addSpacing(findInfo_->minimumWidth());
        bars->addWidget(replaceRow_);
        replaceRow_->hide();
        findBar_->hide();
        top->addWidget(findBar_);

        connect(brep,    &QPushButton::clicked, this, [this] { replaceCurrent(); });
        connect(brepAll, &QPushButton::clicked, this, [this] { replaceAll(); });
        connect(replaceEdit_, &QLineEdit::returnPressed, this, [this] { replaceCurrent(); });
        connect(replaceToggle_, &QPushButton::toggled, this, [this](bool on) {
            replaceRow_->setVisible(on);
            if (on) replaceEdit_->setFocus();
        });
        connect(caseChk_, &QCheckBox::toggled, this, [this](bool) {
            updateFindHighlights();   // the match set changes with the flag
        });

        connect(bnext, &QPushButton::clicked, this, [this] { findNext(false); });
        connect(bprev, &QPushButton::clicked, this, [this] { findNext(true); });
        connect(bclose, &QPushButton::clicked, this, [this] { hideFindBar(); });
        connect(findEdit_, &QLineEdit::returnPressed, this, [this] { findNext(false); });
        connect(findEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
            // incremental: re-anchor at the current match start so typing
            // extends the match in place instead of walking forward
            if (auto* ed = editorAt(tabs_->currentIndex())) {
                QTextCursor c = ed->textCursor();
                c.setPosition(c.selectionStart());
                ed->setTextCursor(c);
            }
            updateFindHighlights();
            if (!findEdit_->text().isEmpty()) findNext(false);
        });

        // Register each sequence exactly ONCE: two QShortcuts sharing a
        // sequence make Qt emit activatedAmbiguously() and fire neither -
        // which silently kills the binding. QKeySequence::Replace already is
        // Ctrl+H on Windows and on most Linux themes, so the explicit Ctrl+H
        // below would otherwise collide with it.
        QSet<QString> bound;
        auto addShortcut = [this, &bound](const QKeySequence& seq, auto slot) {
            const QString key = seq.toString(QKeySequence::PortableText);
            if (seq.isEmpty() || bound.contains(key)) return;
            bound.insert(key);
            auto* sc = new QShortcut(seq, this);
            sc->setContext(Qt::WidgetWithChildrenShortcut);
            connect(sc, &QShortcut::activated, this, slot);
        };
        addShortcut(QKeySequence::Find,     [this] { showFindBar(false); });
        // Ctrl+H first so it is the one that survives; the platform sequence
        // is added too when it happens to differ (some themes use Ctrl+R).
        addShortcut(QKeySequence(Qt::CTRL | Qt::Key_H), [this] { showFindBar(true); });
        addShortcut(QKeySequence::Replace,  [this] { showFindBar(true); });
        addShortcut(QKeySequence::FindNext, [this] { findNext(false); });     // F3
        addShortcut(QKeySequence::FindPrevious, [this] { findNext(true); });
        addShortcut(QKeySequence(Qt::CTRL | Qt::Key_Slash), [this] { toggleComment(); });
        auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), findBar_);
        esc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(esc, &QShortcut::activated, this, [this] { hideFindBar(); });

        connect(tabs_, &QTabWidget::currentChanged, this, [this](int i) {
            if (findBar_->isVisible()) updateFindHighlights();
            refreshUndoButtons();
            // the file may have changed while this tab was in the background
            if (auto* ed = editorAt(i); ed && diskChanged(ed)) onDiskChange(
                ed->property("filePath").toString());
        });
    }

    status_ = new QLabel(QStringLiteral("open a *.DATA file to edit the deck and its includes"));
    top->addWidget(status_);

    // Notice deck edits made outside the GUI (another editor, a script).
    watcher_ = new QFileSystemWatcher(this);
    connect(watcher_, &QFileSystemWatcher::fileChanged, this,
            [this](const QString& p) { onDiskChange(p); });

    connect(tree_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* it, int) {
        const QString f = it->data(0, RoleFile).toString();
        if (!f.isEmpty()) openFile(f, it->data(0, RoleLine).toInt());
    });
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int i) { closeTab(i); });
}

void DeckEditorWidget::setStatus(const QString& s) { status_->setText(s); }

// -- where you have been ----------------------------------------------------

void DeckEditorWidget::noteJump()
{
    auto* ed = editorAt(tabs_->currentIndex());
    if (!ed) return;
    pushJump(ed->property("filePath").toString(), ed->textCursor().blockNumber());
}

void DeckEditorWidget::pushJump(const QString& path, int line)
{
    if (navigating_ || path.isEmpty()) return;
    // Near enough to where we already are is the same place. Without this the
    // history fills with a stop per keystroke and Back walks a line at a time.
    if (jumpAt_ >= 0 && jumpAt_ < jumps_.size()) {
        const Jump& cur = jumps_[jumpAt_];
        if (cur.path == path && std::abs(cur.line - line) <= 6) {
            jumps_[jumpAt_].line = line;      // keep the freshest line
            return;
        }
    }
    // A new place forks the history: what lay ahead belonged to the branch we
    // just left, which is what Back/Forward means everywhere else.
    while (jumps_.size() > jumpAt_ + 1) jumps_.removeLast();
    jumps_.append({ path, line });
    // Old enough is forgotten; nobody navigates back a hundred stops.
    constexpr int kMaxJumps = 100;
    while (jumps_.size() > kMaxJumps) jumps_.removeFirst();
    jumpAt_ = jumps_.size() - 1;
    refreshNavButtons();
}

void DeckEditorWidget::goJump(int dir)
{
    const int to = jumpAt_ + dir;
    if (to < 0 || to >= jumps_.size()) return;
    jumpAt_ = to;
    const Jump j = jumps_[to];
    navigating_ = true;                 // replaying, not travelling
    openFile(j.path, j.line + 1);       // openFile takes a 1-based line
    navigating_ = false;
    refreshNavButtons();
    setStatus(QStringLiteral("%1:%2   (%3 of %4)")
                  .arg(QFileInfo(j.path).fileName())
                  .arg(j.line + 1).arg(jumpAt_ + 1).arg(jumps_.size()));
}

void DeckEditorWidget::refreshNavButtons()
{
    if (backBtn_) backBtn_->setEnabled(jumpAt_ > 0);
    if (fwdBtn_)  fwdBtn_->setEnabled(jumpAt_ >= 0 && jumpAt_ + 1 < jumps_.size());
}

void DeckEditorWidget::refreshUndoButtons()
{
    auto* ed = editorAt(tabs_->currentIndex());
    undoBtn_->setEnabled(ed && ed->document()->isUndoAvailable());
    redoBtn_->setEnabled(ed && ed->document()->isRedoAvailable());
}

// -- tree filter / find bar ---------------------------------------------------
void DeckEditorWidget::filterTree(const QString& needle)
{
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        filterItem(tree_->topLevelItem(i), needle);
}

void DeckEditorWidget::showFindBar(bool withReplace)
{
    // seed from the editor's selection (single-line only)
    if (auto* ed = editorAt(tabs_->currentIndex())) {
        const QString sel = ed->textCursor().selectedText();
        if (!sel.isEmpty() && !sel.contains(QChar(0x2029)))
            findEdit_->setText(sel);
    }
    findBar_->show();
    replaceToggle_->setChecked(withReplace);   // drives the replace row
    replaceRow_->setVisible(withReplace);      // (also when already checked)
    findEdit_->setFocus();
    findEdit_->selectAll();
    updateFindHighlights();
}

void DeckEditorWidget::hideFindBar()
{
    findBar_->hide();
    if (auto* ed = editorAt(tabs_->currentIndex())) {
        ed->setExtraSelections({});
        ed->setFocus();
    }
}

QTextDocument::FindFlags DeckEditorWidget::findFlags(bool backward) const
{
    QTextDocument::FindFlags fl;
    if (backward) fl |= QTextDocument::FindBackward;
    if (!caseChk_ || caseChk_->isChecked()) fl |= QTextDocument::FindCaseSensitively;
    return fl;
}

void DeckEditorWidget::findNext(bool backward)
{
    auto* ed = editorAt(tabs_->currentIndex());
    if (!ed) return;
    if (!findBar_->isVisible()) { showFindBar(false); return; }   // bare F3
    const QString needle = findEdit_->text();
    if (needle.isEmpty()) return;
    const QTextDocument::FindFlags fl = findFlags(backward);
    if (!ed->find(needle, fl)) {
        // Off the end of this file. Across the deck that means carry on into
        // the next one rather than wrapping here, or the search never leaves
        // the tab it started in.
        if (deckChk_ && deckChk_->isChecked() && findInDeck(backward)) return;
        QTextCursor c = ed->textCursor();                    // wrap around once
        c.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
        ed->setTextCursor(c);
        if (!ed->find(needle, fl)) {
            findInfo_->setText(QStringLiteral("not found"));
            return;
        }
    }
    ed->centerCursor();
}

// Replace across every file of the deck. Asked for first: this reaches files
// the user may never have opened, and a wrong needle typed into a shared
// INCLUDE is a mess to unpick by hand.
//
// Each file is opened and edited in its tab, one undo step apiece, and left
// MODIFIED rather than written. Nothing reaches disk until Save or Save all,
// so the whole run can be read over first - and abandoned with Reload.
void DeckEditorWidget::replaceAllInDeck()
{
    const QString needle = findEdit_->text();
    if (needle.isEmpty()) return;
    const QString repl = replaceEdit_->text();
    if (deckFiles_.isEmpty()) {
        setStatus(QStringLiteral("no deck scanned - open a .DATA file first"));
        return;
    }
    const auto a = QMessageBox::question(this, QStringLiteral("Deck editor"),
        QStringLiteral("Replace \"%1\" with \"%2\" through all %3 file(s) of "
                       "%4?\n\nFiles are opened and changed but NOT saved - "
                       "review them, then Save all.")
            .arg(needle, repl).arg(deckFiles_.size())
            .arg(QFileInfo(rootDeck_).fileName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (a != QMessageBox::Yes) return;

    const QTextDocument::FindFlags fl = findFlags();
    const int startTab = tabs_->currentIndex();
    int total = 0, touched = 0;
    for (const QString& path : deckFiles_) {
        openFile(path);
        auto* ed = editorAt(tabs_->currentIndex());
        if (!ed || ed->property("filePath").toString() != path) continue;
        QTextCursor block(ed->document());
        block.beginEditBlock();                 // one undo step per file
        int n = 0;
        QTextCursor f = ed->document()->find(needle, 0, fl);
        while (!f.isNull()) {
            f.insertText(repl);
            f = ed->document()->find(needle, f, fl);
            ++n;
        }
        block.endEditBlock();
        if (n) { total += n; ++touched; }
    }
    if (startTab >= 0 && startTab < tabs_->count()) tabs_->setCurrentIndex(startTab);
    updateFindHighlights();
    setStatus(total
        ? QStringLiteral("replaced %1 occurrence(s) of \"%2\" in %3 file(s) - "
                         "not saved yet").arg(total).arg(needle).arg(touched)
        : QStringLiteral("\"%1\" not found anywhere in the deck").arg(needle));
}

// Continue the search through the deck's other files, in scan order from the
// one in front, opening each as it is reached and stopping at the first match.
// Wraps back to where it started, so every file is looked at exactly once.
bool DeckEditorWidget::findInDeck(bool backward)
{
    if (deckFiles_.isEmpty()) return false;
    const QString needle = findEdit_->text();
    auto* here = editorAt(tabs_->currentIndex());
    const QString from = here ? here->property("filePath").toString() : QString();
    int start = deckFiles_.indexOf(from);
    if (start < 0) start = 0;

    const int n = deckFiles_.size();
    for (int k = 1; k <= n; ++k) {
        const int i = backward ? (start - k + n * n) % n : (start + k) % n;
        const QString path = deckFiles_.at(i);
        // Opening it is the only way to search it, and leaves it where the
        // user can see what was found and edit it.
        openFile(path);
        auto* ed = editorAt(tabs_->currentIndex());
        if (!ed || ed->property("filePath").toString() != path) continue;
        QTextCursor c = ed->textCursor();
        c.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
        ed->setTextCursor(c);
        if (ed->find(needle, findFlags(backward))) {
            ed->centerCursor();
            updateFindHighlights();
            pushJump(path, ed->textCursor().blockNumber());
            findInfo_->setText(QStringLiteral("in %1").arg(QFileInfo(path).fileName()));
            return true;
        }
        if (path == from) break;      // all the way round, back where we began
    }
    findInfo_->setText(QStringLiteral("not in this deck"));
    return false;
}

void DeckEditorWidget::updateFindHighlights()
{
    auto* ed = editorAt(tabs_->currentIndex());
    if (!ed) { findInfo_->clear(); return; }
    const QString needle = findEdit_->text();
    QList<QTextEdit::ExtraSelection> sels;
    int count = 0;
    constexpr int cap = 5000;   // keep huge decks responsive
    if (!needle.isEmpty()) {
        QTextCharFormat fmt;
        fmt.setBackground(QColor(0xff, 0xe0, 0x66));
        QTextCursor c(ed->document());
        const QTextDocument::FindFlags fl = findFlags();
        while (count < cap) {
            c = ed->document()->find(needle, c, fl);
            if (c.isNull()) break;
            QTextEdit::ExtraSelection s;
            s.cursor = c;
            s.format = fmt;
            sels.push_back(s);
            ++count;
        }
    }
    ed->setExtraSelections(sels);
    if (needle.isEmpty())    findInfo_->clear();
    else if (count >= cap)   findInfo_->setText(QStringLiteral("%1+ matches").arg(cap));
    else                     findInfo_->setText(QStringLiteral("%1 match(es)").arg(count));
}

void DeckEditorWidget::replaceCurrent()
{
    auto* ed = editorAt(tabs_->currentIndex());
    if (!ed) return;
    const QString needle = findEdit_->text();
    if (needle.isEmpty()) return;
    // Replace only when the current selection IS the match (i.e. after a
    // Find); otherwise this press just moves to the first match.
    const bool cs = !caseChk_ || caseChk_->isChecked();
    QTextCursor c = ed->textCursor();
    if (c.hasSelection() && c.selectedText().compare(
            needle, cs ? Qt::CaseSensitive : Qt::CaseInsensitive) == 0) {
        c.insertText(replaceEdit_->text());
        ed->setTextCursor(c);
    }
    findNext(false);
    updateFindHighlights();
}

void DeckEditorWidget::replaceAll()
{
    if (deckChk_ && deckChk_->isChecked()) { replaceAllInDeck(); return; }
    auto* ed = editorAt(tabs_->currentIndex());
    if (!ed) return;
    const QString needle = findEdit_->text();
    if (needle.isEmpty()) return;
    const QString repl = replaceEdit_->text();

    const QTextDocument::FindFlags fl = findFlags();
    QTextCursor block(ed->document());
    block.beginEditBlock();               // one undo step for the whole run
    int n = 0;
    QTextCursor f = ed->document()->find(needle, 0, fl);
    while (!f.isNull()) {
        f.insertText(repl);                        // cursor lands after the new
        f = ed->document()->find(needle, f, fl);   // text, so a repl containing
        ++n;                                       // the needle cannot loop
    }
    block.endEditBlock();

    updateFindHighlights();
    setStatus(n ? QStringLiteral("replaced %1 occurrence(s) of \"%2\" in %3")
                      .arg(n).arg(needle, tabs_->tabText(tabs_->currentIndex()))
                : QStringLiteral("\"%1\" not found").arg(needle));
}

void DeckEditorWidget::toggleComment()
{
    auto* ed = editorAt(tabs_->currentIndex());
    if (!ed) return;
    QTextCursor c = ed->textCursor();
    QTextDocument* doc = ed->document();
    const int firstNo = doc->findBlock(c.selectionStart()).blockNumber();
    int lastNo = doc->findBlock(c.selectionEnd()).blockNumber();
    // a selection ending exactly at a line start does not include that line
    if (lastNo > firstNo && doc->findBlock(c.selectionEnd()).position() == c.selectionEnd())
        --lastNo;

    // Uncomment only if every non-blank line in the range is commented.
    bool allCommented = true;
    bool anyContent   = false;
    for (int n = firstNo; n <= lastNo; ++n) {
        const QString t = doc->findBlockByNumber(n).text();
        if (t.trimmed().isEmpty()) continue;
        anyContent = true;
        if (!t.trimmed().startsWith(QLatin1String("--"))) { allCommented = false; break; }
    }
    if (!anyContent) return;

    QTextCursor block(doc);
    block.beginEditBlock();
    for (int n = firstNo; n <= lastNo; ++n) {
        const QTextBlock b = doc->findBlockByNumber(n);
        const QString t = b.text();
        if (t.trimmed().isEmpty()) continue;
        QTextCursor cc(b);
        if (allCommented) {
            const int at = t.indexOf(QLatin1String("--"));
            if (at < 0) continue;
            cc.setPosition(b.position() + at);
            cc.setPosition(b.position() + at + 2, QTextCursor::KeepAnchor);
            cc.removeSelectedText();
        } else {
            cc.setPosition(b.position());       // column 0: unambiguous for
            cc.insertText(QStringLiteral("--")); // every deck parser
        }
    }
    block.endEditBlock();
    setStatus(allCommented
        ? QStringLiteral("uncommented %1 line(s)").arg(lastNo - firstNo + 1)
        : QStringLiteral("commented %1 line(s)").arg(lastNo - firstNo + 1));
}

// -- following INCLUDEs --------------------------------------------------------
QString DeckEditorWidget::includeTargetAt(DeckTextEdit* ed, int position) const
{
    if (!ed) return {};
    const QString dir = QFileInfo(ed->property("filePath").toString()).absolutePath();
    const QTextBlock b = ed->document()->findBlock(position);
    if (!b.isValid()) return {};

    int start = -1, len = 0;
    const QString tok = deckFileToken(b.text(), &start, &len);
    if (tok.isEmpty() || start < 0) return {};

    // The click must land on the file name itself - clicking the INCLUDE
    // keyword (or anywhere else on the line) does not navigate.
    const int col = position - b.position();
    if (col < start || col > start + len) return {};

    // And the token must resolve to a real file, which keeps ordinary quoted
    // strings (well names, 'OPEN', ...) from being treated as paths.
    const QString abs = QDir::cleanPath(QDir(dir).filePath(tok));
    const QFileInfo fi(abs);
    return (fi.exists() && fi.isFile()) ? abs : QString();
}

void DeckEditorWidget::openIncludeAt(DeckTextEdit* ed, int position)
{
    const QString target = includeTargetAt(ed, position);
    if (target.isEmpty()) return;
    openFile(target);
    setStatus(QStringLiteral("opened %1").arg(QDir::toNativeSeparators(target)));
}

// -- external changes / reload ------------------------------------------------
void DeckEditorWidget::rememberDiskStamp(DeckTextEdit* ed)
{
    const QFileInfo fi(ed->property("filePath").toString());
    ed->setProperty("diskMtime", fi.lastModified().toMSecsSinceEpoch());
    ed->setProperty("diskSize",  qint64(fi.size()));
    ed->setProperty("diskStale", false);
}

bool DeckEditorWidget::diskChanged(DeckTextEdit* ed) const
{
    const QFileInfo fi(ed->property("filePath").toString());
    if (!fi.exists()) return false;
    return fi.lastModified().toMSecsSinceEpoch() != ed->property("diskMtime").toLongLong()
        || qint64(fi.size()) != ed->property("diskSize").toLongLong();
}

void DeckEditorWidget::watchPath(const QString& path)
{
    // QSaveFile (and most editors) replace rather than rewrite the file,
    // which drops the watch - so re-adding is the normal case, not an error.
    if (watcher_ && !watcher_->files().contains(path)) watcher_->addPath(path);
}

void DeckEditorWidget::onDiskChange(const QString& path)
{
    watchPath(path);                       // re-arm after a replace-on-save
    const int tab = tabForPath(path);
    if (tab < 0) { if (watcher_) watcher_->removePath(path); return; }
    auto* ed = editorAt(tab);
    if (!ed || !diskChanged(ed)) return;   // our own save, or a no-op touch

    if (!ed->document()->isModified()) {   // safe to take the new content
        reloadTab(tab, true);
        setStatus(QStringLiteral("%1 changed on disk - reloaded")
                      .arg(QDir::toNativeSeparators(path)));
        return;
    }
    // Unsaved edits here: never discard them silently, just flag it.
    ed->setProperty("diskStale", true);
    updateTabTitle(tab);
    tabs_->setTabToolTip(tab, QStringLiteral("%1\nchanged on disk since it was "
                                             "opened - Reload to discard your "
                                             "edits and take the new content")
                                  .arg(QDir::toNativeSeparators(path)));
    setStatus(QStringLiteral("%1 changed on disk but has unsaved edits here - "
                             "use Reload to discard them, or Save to overwrite")
                  .arg(QFileInfo(path).fileName()));
}

void DeckEditorWidget::reloadTab(int tab, bool force)
{
    auto* ed = editorAt(tab);
    if (!ed) return;
    const QString path = ed->property("filePath").toString();
    if (!force && ed->document()->isModified()) {
        const auto a = QMessageBox::question(this, QStringLiteral("Deck editor"),
            QStringLiteral("%1 has unsaved changes.\nDiscard them and re-read "
                           "the file from disk?").arg(tabs_->tabText(tab)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (a != QMessageBox::Yes) return;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        setStatus(QStringLiteral("cannot re-read %1").arg(QDir::toNativeSeparators(path)));
        return;
    }
    // keep the caret roughly where it was
    const int line = ed->textCursor().blockNumber();
    const int scroll = ed->verticalScrollBar()->value();
    ed->setPlainText(QString::fromLatin1(f.readAll()));
    ed->document()->setModified(false);
    const QTextBlock b = ed->document()->findBlockByNumber(line);
    if (b.isValid()) ed->setTextCursor(QTextCursor(b));
    ed->verticalScrollBar()->setValue(scroll);
    rememberDiskStamp(ed);
    watchPath(path);
    tabs_->setTabToolTip(tab, QDir::toNativeSeparators(path));
    updateTabTitle(tab);
    if (findBar_->isVisible()) updateFindHighlights();
    // The new content may hold different keywords or INCLUDEs, and the file
    // may be an include of the open deck - rescan so the tree stays honest.
    if (!rootDeck_.isEmpty()) scanDeck();
}

// -- tabs -------------------------------------------------------------------
int DeckEditorWidget::tabForPath(const QString& path) const
{
    const QString canon = QFileInfo(path).canonicalFilePath();
    for (int i = 0; i < tabs_->count(); ++i)
        if (tabs_->widget(i)->property("filePath").toString() == canon) return i;
    return -1;
}

DeckTextEdit* DeckEditorWidget::editorAt(int tab) const
{
    return qobject_cast<DeckTextEdit*>(tabs_->widget(tab));
}

void DeckEditorWidget::openFile(const QString& path, int line)
{
    int tab = tabForPath(path);
    if (tab < 0) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            setStatus(QStringLiteral("cannot open %1").arg(QDir::toNativeSeparators(path)));
            return;
        }
        // byte-exact round trip for ASCII decks: Latin-1 both directions
        const QString text = QString::fromLatin1(f.readAll());
        auto* ed = new DeckTextEdit;
        ed->setPlainText(text);
        ed->document()->setModified(false);
        new DeckHighlighter(ed->document());
        ed->setProperty("filePath", QFileInfo(path).canonicalFilePath());
        rememberDiskStamp(ed);
        watchPath(ed->property("filePath").toString());
        tab = tabs_->addTab(ed, QFileInfo(path).fileName());
        tabs_->setTabToolTip(tab, QDir::toNativeSeparators(path));
        connect(ed->document(), &QTextDocument::modificationChanged, this,
                [this, ed](bool) {
            for (int i = 0; i < tabs_->count(); ++i)
                if (tabs_->widget(i) == ed) { updateTabTitle(i); break; }
        });
        // Follow INCLUDEs: double click the file name itself opens it;
        // right click offers it in the context menu.
        connect(ed, &DeckTextEdit::doubleClickedAt, this,
                [this, ed](int pos) { openIncludeAt(ed, pos); });
        // Keep the Undo/Redo buttons in step with this document.
        connect(ed->document(), &QTextDocument::undoAvailable, this,
                [this, ed](bool) { if (ed == editorAt(tabs_->currentIndex())) refreshUndoButtons(); });
        connect(ed->document(), &QTextDocument::redoAvailable, this,
                [this, ed](bool) { if (ed == editorAt(tabs_->currentIndex())) refreshUndoButtons(); });
        // An edit is a place worth being able to come back to - "where was I
        // working" being the commonest reason to want to go back at all.
        // pushJump() folds anything within a few lines into the stop already
        // there, so typing leaves one entry and not one per keystroke.
        connect(ed->document(), &QTextDocument::contentsChanged, this, [this, ed] {
            if (ed == editorAt(tabs_->currentIndex()))
                pushJump(ed->property("filePath").toString(),
                         ed->textCursor().blockNumber());
        });
        ed->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(ed, &QWidget::customContextMenuRequested, this,
                [this, ed](const QPoint& p) {
            const int pos = ed->cursorForPosition(p).position();
            const QString target = includeTargetAt(ed, pos);
            std::unique_ptr<QMenu> menu(ed->createStandardContextMenu(p));
            if (!target.isEmpty()) {
                auto* act = new QAction(
                    QStringLiteral("Open \"%1\"").arg(QFileInfo(target).fileName()),
                    menu.get());
                connect(act, &QAction::triggered, this,
                        [this, target] { openFile(target); });
                menu->insertAction(menu->actions().value(0), act);
                menu->insertSeparator(menu->actions().value(1));
            }
            menu->exec(ed->viewport()->mapToGlobal(p));
        });
    }
    // Leaving somewhere is worth recording as much as arriving: every jump
    // goes through here - the tree, an INCLUDE, a search landing in another
    // file - so both ends of it are caught in one place.
    if (line > 0) noteJump();
    tabs_->setCurrentIndex(tab);
    if (line > 0) {
        auto* ed = editorAt(tab);
        const QTextBlock b = ed->document()->findBlockByNumber(line - 1);
        if (b.isValid()) {
            QTextCursor c(b);
            ed->setTextCursor(c);
            ed->centerCursor();
        }
        ed->setFocus();
        pushJump(ed->property("filePath").toString(), line - 1);
    }
}

void DeckEditorWidget::updateTabTitle(int tab)
{
    auto* ed = editorAt(tab);
    if (!ed) return;
    QString t = QFileInfo(ed->property("filePath").toString()).fileName();
    if (ed->document()->isModified())     t += QLatin1Char('*');
    if (ed->property("diskStale").toBool()) t += QLatin1Char('!');   // changed on disk
    tabs_->setTabText(tab, t);
}

bool DeckEditorWidget::saveTab(int tab)
{
    auto* ed = editorAt(tab);
    if (!ed) return false;
    const QString path = ed->property("filePath").toString();
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("Deck editor"),
            QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    f.write(ed->toPlainText().toLatin1());
    if (!f.commit()) {
        QMessageBox::warning(this, QStringLiteral("Deck editor"),
            QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    ed->document()->setModified(false);
    rememberDiskStamp(ed);      // our own write must not read back as external
    watchPath(path);            // QSaveFile replaced the file: re-arm the watch
    tabs_->setTabToolTip(tab, QDir::toNativeSeparators(path));
    updateTabTitle(tab);
    setStatus(QStringLiteral("saved %1").arg(QDir::toNativeSeparators(path)));
    return true;
}

void DeckEditorWidget::saveAll()
{
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto* ed = editorAt(i); ed && ed->document()->isModified())
            saveTab(i);
}

void DeckEditorWidget::saveAllTabs() { saveAll(); }

// Following an INCLUDE opens a tab, and a deck of any size leaves a row of
// them behind. Closing them one at a time is the tedious way; this asks once
// for the lot rather than once per modified file.
void DeckEditorWidget::closeAllTabs()
{
    if (tabs_->count() == 0) {
        setStatus(QStringLiteral("no open files to close"));
        return;
    }
    int modified = 0;
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto* ed = editorAt(i); ed && ed->document()->isModified()) ++modified;

    if (modified > 0) {
        const auto a = QMessageBox::question(this, QStringLiteral("Deck editor"),
            QStringLiteral("%1 of the %2 open files %3 unsaved changes. "
                           "Save them before closing?")
                .arg(modified).arg(tabs_->count())
                .arg(modified == 1 ? QStringLiteral("has") : QStringLiteral("have")),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (a == QMessageBox::Cancel) return;
        if (a == QMessageBox::Save) {
            for (int i = 0; i < tabs_->count(); ++i) {
                auto* ed = editorAt(i);
                // A file that will not write keeps everything open: closing
                // the rest would leave the failure buried.
                if (ed && ed->document()->isModified() && !saveTab(i)) return;
            }
        }
    }

    const int n = tabs_->count();
    while (tabs_->count() > 0) {
        auto* ed = editorAt(0);
        const QString path = ed ? ed->property("filePath").toString() : QString();
        tabs_->removeTab(0);
        if (ed) ed->deleteLater();
        if (watcher_ && !path.isEmpty() && tabForPath(path) < 0)
            watcher_->removePath(path);
    }
    setStatus(QStringLiteral("closed %1 file%2").arg(n).arg(n == 1 ? QString()
                                                                  : QStringLiteral("s")));
}

void DeckEditorWidget::closeTab(int tab)
{
    auto* ed = editorAt(tab);
    if (!ed) return;
    if (ed->document()->isModified()) {
        const auto a = QMessageBox::question(this, QStringLiteral("Deck editor"),
            QStringLiteral("%1 has unsaved changes. Save before closing?")
                .arg(tabs_->tabText(tab)),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (a == QMessageBox::Cancel) return;
        if (a == QMessageBox::Save && !saveTab(tab)) return;
    }
    const QString path = ed->property("filePath").toString();
    tabs_->removeTab(tab);
    ed->deleteLater();
    if (watcher_ && tabForPath(path) < 0) watcher_->removePath(path);
}

bool DeckEditorWidget::hasUnsavedChanges() const
{
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto* ed = editorAt(i); ed && ed->document()->isModified())
            return true;
    return false;
}

// -- session state -----------------------------------------------------------
QJsonObject DeckEditorWidget::uiState() const
{
    QJsonObject o;
    if (!rootDeck_.isEmpty())
        o[QStringLiteral("deck")] = QDir::fromNativeSeparators(rootDeck_);
    QJsonArray files;
    for (int i = 0; i < tabs_->count(); ++i) {
        const QString p = tabs_->widget(i)->property("filePath").toString();
        if (!p.isEmpty()) files.append(QDir::fromNativeSeparators(p));
    }
    o[QStringLiteral("files")]   = files;
    o[QStringLiteral("current")] = tabs_->currentIndex();
    // The line the front tab was left on, so a long deck comes back where it
    // was being read rather than at the top.
    if (auto* ed = editorAt(tabs_->currentIndex()))
        o[QStringLiteral("line")] = ed->textCursor().blockNumber() + 1;
    return o;
}

void DeckEditorWidget::restoreUiState(const QJsonObject& state)
{
    if (state.isEmpty()) return;
    const QString deck =
        QDir::toNativeSeparators(state.value(QStringLiteral("deck")).toString());
    if (!deck.isEmpty() && QFileInfo::exists(deck)) openDeck(deck);

    const QJsonArray files = state.value(QStringLiteral("files")).toArray();
    for (const auto& v : files) {
        const QString p = QDir::toNativeSeparators(v.toString());
        if (!p.isEmpty() && QFileInfo::exists(p)) openFile(p);   // no-op if open
    }
    const int cur = state.value(QStringLiteral("current")).toInt(-1);
    if (cur >= 0 && cur < tabs_->count()) tabs_->setCurrentIndex(cur);
    const int line = state.value(QStringLiteral("line")).toInt(-1);
    if (line > 0) {
        if (auto* ed = editorAt(tabs_->currentIndex())) {
            const QTextBlock b = ed->document()->findBlockByNumber(line - 1);
            if (b.isValid()) { ed->setTextCursor(QTextCursor(b)); ed->centerCursor(); }
        }
    }
}

// -- deck structure ----------------------------------------------------------
void DeckEditorWidget::openDeck(const QString& dataFile)
{
    rootDeck_ = dataFile;
    scanDeck();
    openFile(dataFile);
}

QTreeWidgetItem* DeckEditorWidget::sectionItem(const QString& name)
{
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        if (tree_->topLevelItem(i)->text(0) == name)
            return tree_->topLevelItem(i);
    auto* it = new QTreeWidgetItem(tree_, { name, QString() });
    QFont f = it->font(0); f.setBold(true); it->setFont(0, f);
    // Closed to begin with: a deck has eight sections and hundreds of
    // keywords, and a wall of them says less than the sections do. Expand,
    // Expand all and the filter are all one click away. A section the user
    // had open stays open across a rescan (see scanDeck).
    it->setExpanded(expandedSections_.contains(name));
    return it;
}

void DeckEditorWidget::scanDeck()
{
    // A rescan rebuilds the tree, so remember which sections were open - a
    // file changing on disk should not fold the tree back up under you.
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto* it = tree_->topLevelItem(i);
        if (it->isExpanded()) expandedSections_.insert(it->text(0));
        else                  expandedSections_.remove(it->text(0));
    }
    tree_->clear();
    deckFiles_.clear();
    if (rootDeck_.isEmpty()) return;
    QString section = QStringLiteral("(preamble)");
    int fileBudget = 128;                    // safety cap on include fan-out
    scanFile(rootDeck_, nullptr, nullptr, section, 0, fileBudget);
    if (treeFilter_ && !treeFilter_->text().trimmed().isEmpty())
        filterTree(treeFilter_->text().trimmed());
    setStatus(QStringLiteral("%1: structure scanned (%2 sections)")
        .arg(QFileInfo(rootDeck_).fileName())
        .arg(tree_->topLevelItemCount()));
}

void DeckEditorWidget::scanFile(const QString& path, QTreeWidgetItem*,
                                QTreeWidgetItem* fileParent,
                                QString& section, int depth, int& fileBudget)
{
    if (depth > 8 || fileBudget-- <= 0) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (fileParent) fileParent->setText(1, QStringLiteral("missing"));
        return;
    }
    // The deck as a list of files, for the searches that span it. Canonical
    // and deduplicated: one include pulled in from two places is one file, and
    // searching it twice would report every hit in it twice.
    {
        const QString canon = QFileInfo(path).canonicalFilePath();
        if (!canon.isEmpty() && !deckFiles_.contains(canon)) deckFiles_ << canon;
    }
    const QString dir = QFileInfo(path).absolutePath();
    const QString fname = QFileInfo(path).fileName();
    int lineNo = 0;
    bool wantIncludeArg = false;
    QTreeWidgetItem* lastInclude = nullptr;

    while (!f.atEnd()) {
        const QString line = QString::fromLatin1(f.readLine());
        ++lineNo;
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1String("--"))) continue;

        if (wantIncludeArg) {
            // the line after INCLUDE holds ['path' | path] [/]
            QString tok = trimmed;
            const int cm = tok.indexOf(QLatin1String("--"));
            if (cm >= 0) tok = tok.left(cm).trimmed();
            if (tok.endsWith(QLatin1Char('/'))) tok.chop(1);
            tok = tok.trimmed();
            if (tok.startsWith(QLatin1Char('\'')) && tok.endsWith(QLatin1Char('\'')) && tok.size() >= 2)
                tok = tok.mid(1, tok.size() - 2);
            if (!tok.isEmpty()) {
                const QString inc = QDir::cleanPath(QDir(dir).filePath(tok));
                if (lastInclude) {
                    lastInclude->setText(0, QStringLiteral("INCLUDE %1").arg(QFileInfo(inc).fileName()));
                    lastInclude->setData(0, RoleFile, inc);
                    lastInclude->setData(0, RoleLine, 1);
                    lastInclude->setText(1, QDir::toNativeSeparators(inc));
                    scanFile(inc, nullptr, lastInclude, section, depth + 1, fileBudget);
                }
            }
            wantIncludeArg = false;
            lastInclude = nullptr;
            continue;
        }

        const auto m = kKeywordRe.match(line);
        if (!m.hasMatch()) continue;
        const QString kw = m.captured(1);

        if (isSectionKw(kw)) {
            section = kw;
            auto* s = sectionItem(section);
            s->setData(0, RoleFile, path);
            s->setData(0, RoleLine, lineNo);
            s->setText(1, QStringLiteral("%1:%2").arg(fname).arg(lineNo));
            continue;
        }

        QTreeWidgetItem* parent = fileParent ? fileParent : sectionItem(section);
        auto* it = new QTreeWidgetItem(parent,
            { kw, QStringLiteral("%1:%2").arg(fname).arg(lineNo) });
        it->setData(0, RoleFile, path);
        it->setData(0, RoleLine, lineNo);

        if (kw == QLatin1String("INCLUDE")) {
            wantIncludeArg = true;
            lastInclude = it;
        }
    }
}
