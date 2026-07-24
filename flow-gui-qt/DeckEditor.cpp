/*
  DeckEditor implementation. Part of the opm_flow_windows harness; GPL v3+
  (see repository LICENSE).
*/
#include "DeckEditor.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QShortcut>
#include <QSplitter>
#include <QTabWidget>
#include <QTextBlock>
#include <QTreeWidget>
#include <QVBoxLayout>

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
        auto* bscan = new QPushButton(QStringLiteral("Rescan structure"));
        row->addWidget(bopen); row->addWidget(bsave); row->addWidget(ball);
        row->addWidget(bscan); row->addStretch(1);
        top->addLayout(row);

        connect(bopen, &QPushButton::clicked, this, [this] {
            const QString f = QFileDialog::getOpenFileName(
                this, QStringLiteral("Open input deck"), QString(),
                QStringLiteral("Eclipse decks (*.DATA *.data);;All files (*)"));
            if (!f.isEmpty()) openDeck(f);
        });
        connect(bsave, &QPushButton::clicked, this, [this] { saveTab(tabs_->currentIndex()); });
        connect(ball,  &QPushButton::clicked, this, [this] { saveAll(); });
        connect(bscan, &QPushButton::clicked, this, [this] { scanDeck(); });
        auto* sc = new QShortcut(QKeySequence::Save, this);
        connect(sc, &QShortcut::activated, this, [this] { saveTab(tabs_->currentIndex()); });
    }

    auto* split = new QSplitter;
    tree_ = new QTreeWidget;
    tree_->setHeaderLabels({ QStringLiteral("Section / keyword"), QStringLiteral("Location") });
    tree_->setColumnWidth(0, 240);
    split->addWidget(tree_);
    tabs_ = new QTabWidget;
    tabs_->setTabsClosable(true);
    tabs_->setDocumentMode(true);
    split->addWidget(tabs_);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({ 340, 700 });
    top->addWidget(split, 1);

    status_ = new QLabel(QStringLiteral("open a *.DATA file to edit the deck and its includes"));
    top->addWidget(status_);

    connect(tree_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* it, int) {
        const QString f = it->data(0, RoleFile).toString();
        if (!f.isEmpty()) openFile(f, it->data(0, RoleLine).toInt());
    });
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int i) { closeTab(i); });
}

void DeckEditorWidget::setStatus(const QString& s) { status_->setText(s); }

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
        tab = tabs_->addTab(ed, QFileInfo(path).fileName());
        tabs_->setTabToolTip(tab, QDir::toNativeSeparators(path));
        connect(ed->document(), &QTextDocument::modificationChanged, this,
                [this, ed](bool) {
            for (int i = 0; i < tabs_->count(); ++i)
                if (tabs_->widget(i) == ed) { updateTabTitle(i); break; }
        });
    }
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
    }
}

void DeckEditorWidget::updateTabTitle(int tab)
{
    auto* ed = editorAt(tab);
    if (!ed) return;
    QString t = QFileInfo(ed->property("filePath").toString()).fileName();
    if (ed->document()->isModified()) t += QLatin1Char('*');
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
    tabs_->removeTab(tab);
    ed->deleteLater();
}

bool DeckEditorWidget::hasUnsavedChanges() const
{
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto* ed = editorAt(i); ed && ed->document()->isModified())
            return true;
    return false;
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
    it->setExpanded(name != QLatin1String("SCHEDULE"));   // SCHEDULE can be huge
    return it;
}

void DeckEditorWidget::scanDeck()
{
    tree_->clear();
    if (rootDeck_.isEmpty()) return;
    QString section = QStringLiteral("(preamble)");
    int fileBudget = 128;                    // safety cap on include fan-out
    scanFile(rootDeck_, nullptr, nullptr, section, 0, fileBudget);
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
