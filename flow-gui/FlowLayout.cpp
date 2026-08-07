/*
  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  FlowLayout implementation. Part of the opm_flow_windows harness; GPL v3+
  (see repository LICENSE).
*/
#include "FlowLayout.h"

#include <QWidget>

FlowLayout::FlowLayout(QWidget* parent, int margin, int hSpacing, int vSpacing)
    : QLayout(parent), hSpace_(hSpacing), vSpace_(vSpacing)
{
    setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem* it = takeAt(0)) delete it;
}

void FlowLayout::addItem(QLayoutItem* item)      { items_.append(item); }
int  FlowLayout::count() const                   { return int(items_.size()); }

QLayoutItem* FlowLayout::itemAt(int index) const
{
    return items_.value(index);
}

QLayoutItem* FlowLayout::takeAt(int index)
{
    return (index >= 0 && index < items_.size()) ? items_.takeAt(index) : nullptr;
}

int FlowLayout::heightForWidth(int width) const
{
    return doLayout(QRect(0, 0, width, 0), true);
}

void FlowLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

QSize FlowLayout::sizeHint() const
{
    // What one line would take: on a screen wide enough this is what the row
    // gets, so it looks exactly like the QHBoxLayout it replaces.
    QSize s(0, 0);
    for (const QLayoutItem* it : items_) {
        const QSize h = it->sizeHint();
        s.setWidth(s.width() + h.width() + hSpace_);
        s.setHeight(qMax(s.height(), h.height()));
    }
    const QMargins m = contentsMargins();
    return s + QSize(m.left() + m.right(), m.top() + m.bottom());
}

QSize FlowLayout::minimumSize() const
{
    // The point of the whole class: the floor is the widest single widget,
    // not the sum of them all.
    QSize s(0, 0);
    for (const QLayoutItem* it : items_) s = s.expandedTo(it->minimumSize());
    const QMargins m = contentsMargins();
    return s + QSize(m.left() + m.right(), m.top() + m.bottom());
}

int FlowLayout::doLayout(const QRect& rect, bool testOnly) const
{
    const QMargins m = contentsMargins();
    const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
    int x = area.x();
    int y = area.y();
    int lineHeight = 0;

    for (QLayoutItem* it : items_) {
        const QSize hint = it->sizeHint();
        int next = x + hint.width();
        if (next > area.right() + 1 && lineHeight > 0) {   // wrap
            x = area.x();
            y += lineHeight + vSpace_;
            next = x + hint.width();
            lineHeight = 0;
        }
        if (!testOnly)
            it->setGeometry(QRect(QPoint(x, y), hint));
        x = next + hSpace_;
        lineHeight = qMax(lineHeight, hint.height());
    }
    return y + lineHeight - rect.y() + m.bottom();
}

QWidget* FlowLayout::host(FlowLayout** out, QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* lay = new FlowLayout(w);
    // Vertically the widget must be allowed to grow as lines wrap, and the
    // parent layout has to ask how tall it is at the width it is given.
    QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Minimum);
    sp.setHeightForWidth(true);
    w->setSizePolicy(sp);
    if (out) *out = lay;
    return w;
}
