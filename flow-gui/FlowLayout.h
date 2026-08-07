/*
  FlowLayout - a horizontal layout that wraps onto further lines when it is
  not given the width to put everything on one.

  A toolbar built with QHBoxLayout demands the sum of its widgets as a hard
  minimum width, and a window inherits that from its widest tab: flow-gui's
  Summary Plots toolbar alone asked for a little over 2000 px, more than a
  1920 laptop panel has, so the window could not be fitted to such a screen
  at all and the window manager had nothing to maximise it into. Wrapping
  turns that hard floor into the width of the widest single widget, and on a
  wide screen the row still comes out exactly as before, on one line.

  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QLayout>
#include <QList>

class QWidget;

class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget* parent = nullptr, int margin = 0,
                        int hSpacing = 6, int vSpacing = 4);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int count() const override;
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;

    Qt::Orientations expandingDirections() const override { return {}; }
    bool  hasHeightForWidth() const override { return true; }
    int   heightForWidth(int width) const override;
    void  setGeometry(const QRect& rect) override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;

    // A widget wrapping this layout, ready to drop into a QVBoxLayout with
    // addWidget(): it carries the size policy that makes a parent layout ask
    // for the wrapped height instead of the one-line height.
    static QWidget* host(FlowLayout** out, QWidget* parent = nullptr);

private:
    int doLayout(const QRect& rect, bool testOnly) const;

    QList<QLayoutItem*> items_;
    int hSpace_;
    int vSpace_;
};
