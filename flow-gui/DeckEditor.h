/*
  DeckEditor - edit Eclipse-style input decks (*.DATA) and their INCLUDE
  files. A section tree (RUNSPEC ... SCHEDULE) lists every keyword with its
  source file and line - INCLUDEs expanded recursively - and clicking a node
  opens the real file at that line in a tabbed editor with syntax
  highlighting. Edits are saved to the original files (never to a flattened
  copy), so shared include files stay consistent across decks.

  Copyright (C) 2026 SINTEF Digital

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QPlainTextEdit>
#include <QSyntaxHighlighter>
#include <QWidget>

class QFileSystemWatcher;
class QLabel;
class QLineEdit;
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
protected:
    void resizeEvent(QResizeEvent* ev) override;
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
    // Watches the open files so edits made outside the GUI are noticed.
    QFileSystemWatcher* watcher_ = nullptr;
    QString      rootDeck_;

    void filterTree(const QString& needle);
    void showFindBar(bool withReplace);
    void hideFindBar();
    void findNext(bool backward);
    void updateFindHighlights();
    void replaceCurrent();
    void replaceAll();
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
    void updateTabTitle(int tab);
    void setStatus(const QString& s);
};
