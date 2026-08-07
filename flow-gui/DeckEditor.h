/*
  DeckEditor - edit Eclipse-style input decks (*.DATA) and their INCLUDE
  files. A section tree (RUNSPEC ... SCHEDULE) lists every keyword with its
  source file and line - INCLUDEs expanded recursively - and clicking a node
  opens the real file at that line in a tabbed editor with syntax
  highlighting. Edits are saved to the original files (never to a flattened
  copy), so shared include files stay consistent across decks.

  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QPlainTextEdit>
#include <QSyntaxHighlighter>
#include <QWidget>

class QCheckBox;
class QFileSystemWatcher;
class QJsonObject;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

// --------------------------------------------------------------------------
// Eclipse deck syntax highlighting: comments, section/regular keywords,
// quoted strings, numbers (incl. n*value repeats), record terminators.
class DeckHighlighter : public QSyntaxHighlighter
{
public:
    explicit DeckHighlighter(QTextDocument* doc);
protected:
    void highlightBlock(const QString& text) override;
};

// --------------------------------------------------------------------------
// Plain-text editor with a line-number margin.
class DeckTextEdit : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit DeckTextEdit(QWidget* parent = nullptr);
    int  lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent* ev);

signals:
    // Character position of a double click - used to follow INCLUDE paths.
    void doubleClickedAt(int position);

protected:
    void resizeEvent(QResizeEvent* ev) override;
    void mouseDoubleClickEvent(QMouseEvent* ev) override;

private:
    QWidget* lineArea_ = nullptr;
};

// --------------------------------------------------------------------------
class DeckEditorWidget : public QWidget
{
public:
    explicit DeckEditorWidget(QWidget* parent = nullptr);

    // Open a deck: scans its structure into the tree and shows the file.
    void openDeck(const QString& dataFile);
    // Open a single file (tab) and optionally jump to a 1-based line.
    void openFile(const QString& path, int line = -1);
    // True if any open tab has unsaved changes.
    bool hasUnsavedChanges() const;
    // Write every modified tab back to its file. Called before a run: flow
    // re-reads the decks from disk, so unsaved edits must land first.
    void saveAllTabs();

    // Session state: the scanned deck, the open tabs and which one was in
    // front, so the same files come back up in the next session. Only the
    // file names travel - the text itself lives on disk.
    QJsonObject uiState() const;
    void restoreUiState(const QJsonObject& state);

private:
    QTreeWidget* tree_       = nullptr;
    QLineEdit*   treeFilter_ = nullptr;   // filters the structure tree
    QTabWidget*  tabs_       = nullptr;
    QLabel*      status_     = nullptr;
    QWidget*     findBar_    = nullptr;   // Ctrl+F in-editor search
    QLineEdit*   findEdit_   = nullptr;
    QLabel*      findInfo_   = nullptr;   // match count / "not found"
    QWidget*     replaceRow_ = nullptr;   // Ctrl+H extension of the find bar
    QLineEdit*   replaceEdit_ = nullptr;
    QPushButton* replaceToggle_ = nullptr;   // shows/hides replaceRow_
    QCheckBox*   caseChk_ = nullptr;         // match case (on by default)
    QCheckBox*   deckChk_ = nullptr;         // search every file, not this tab
    QPushButton* undoBtn_ = nullptr;
    QPushButton* redoBtn_ = nullptr;
    QPushButton* backBtn_ = nullptr;
    QPushButton* fwdBtn_  = nullptr;

    // Where the caret has been, oldest first, with jumpAt_ pointing at where
    // it is now. A deck is read by hopping - a keyword in the tree, into an
    // INCLUDE, back out again - and the tab bar cannot say where in a file you
    // were, only which file. Back and Forward walk this instead.
    struct Jump { QString path; int line; };
    QVector<Jump> jumps_;
    int  jumpAt_ = -1;
    bool navigating_ = false;   // a jump being replayed is not a new one

    // Root deck first, then every INCLUDE in the order the scan met them.
    // The list the whole-deck search walks.
    QStringList deckFiles_;
    // Watches the open files so edits made outside the GUI are noticed.
    QFileSystemWatcher* watcher_ = nullptr;
    QString      rootDeck_;
    // Sections the user has opened; the tree starts closed and a rescan
    // puts back what was open rather than folding it up again.
    QSet<QString> expandedSections_;

    void filterTree(const QString& needle);
    void showFindBar(bool withReplace);
    void hideFindBar();
    void findNext(bool backward);
    void updateFindHighlights();
    void replaceCurrent();
    void replaceAll();
    // Search flags honouring the "match case" box (case sensitive by default).
    QTextDocument::FindFlags findFlags(bool backward = false) const;
    // The INCLUDE'd file referenced at this character position, if any:
    // absolute path of an existing file, else empty.
    QString includeTargetAt(DeckTextEdit* ed, int position) const;
    void openIncludeAt(DeckTextEdit* ed, int position);
    void refreshUndoButtons();
    // Record the caret's present spot as somewhere worth coming back to.
    void noteJump();
    void pushJump(const QString& path, int line);
    void goJump(int dir);             // -1 back, +1 forward
    void refreshNavButtons();
    // Carry the search on into the deck's other files, opening them as it
    // goes. Returns true if it landed on a match.
    bool findInDeck(bool backward);
    void replaceAllInDeck();
    // Toggle "--" comments on the selected lines (or the current line).
    void toggleComment();
    // Re-read a tab from disk. force = discard unsaved changes without asking.
    void reloadTab(int tab, bool force);
    void onDiskChange(const QString& path);
    void rememberDiskStamp(DeckTextEdit* ed);
    bool diskChanged(DeckTextEdit* ed) const;
    void watchPath(const QString& path);
    void scanDeck();
    void scanFile(const QString& path, QTreeWidgetItem* sectionParent,
                  QTreeWidgetItem* fileParent, QString& currentSection,
                  int depth, int& fileBudget);
    QTreeWidgetItem* sectionItem(const QString& name);
    int  tabForPath(const QString& path) const;
    DeckTextEdit* editorAt(int tab) const;
    bool saveTab(int tab);
    void saveAll();
    void closeTab(int tab);
    void closeAllTabs();            // every tab, one prompt for the lot
    void updateTabTitle(int tab);
    void setStatus(const QString& s);
};
