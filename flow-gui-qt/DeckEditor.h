/*
  DeckEditor - edit Eclipse-style input decks (*.DATA) and their INCLUDE
  files. A section tree (RUNSPEC ... SCHEDULE) lists every keyword with its
  source file and line - INCLUDEs expanded recursively - and clicking a node
  opens the real file at that line in a tabbed editor with syntax
  highlighting. Edits are saved to the original files (never to a flattened
  copy), so shared include files stay consistent across decks.

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QPlainTextEdit>
#include <QSyntaxHighlighter>
#include <QWidget>

class QLabel;
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

private:
    QTreeWidget* tree_   = nullptr;
    QTabWidget*  tabs_   = nullptr;
    QLabel*      status_ = nullptr;
    QString      rootDeck_;

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
