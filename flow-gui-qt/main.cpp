/*
  flow-gui-qt - a cross-platform (Windows / Linux / macOS) Qt 6 GUI front
  end for running OPM Flow simulations: job queue with live progress,
  MPI/OpenMP run options, streamed simulator log, and (when built with
  summary support) plots of summary vectors via opm-common's ESmry.

  This file is part of the opm_flow_windows build harness and is licensed
  under the GNU General Public License v3+ like the OPM project itself.
*/

#include "FlowGuiWindow.h"

#include <QApplication>
#include <QPalette>
#include <QStyleHints>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    // --version: print and exit (headless smoke test, no QApplication needed)
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("flow-gui-qt 0.2.0\n");
            return 0;
        }
    }

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("OPM"));
    QApplication::setApplicationName(QStringLiteral("flow-gui-qt"));

    // Bright, platform-independent appearance: do not follow a dark system
    // color scheme; use the Fusion style with an explicit light palette.
    app.setStyle(QStringLiteral("Fusion"));
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    app.styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif
    QPalette pal;
    pal.setColor(QPalette::Window,          QColor(0xf4, 0xf6, 0xf8));
    pal.setColor(QPalette::WindowText,      Qt::black);
    pal.setColor(QPalette::Base,            Qt::white);
    pal.setColor(QPalette::AlternateBase,   QColor(0xec, 0xf0, 0xf4));
    pal.setColor(QPalette::Text,            Qt::black);
    pal.setColor(QPalette::Button,          QColor(0xe8, 0xec, 0xf0));
    pal.setColor(QPalette::ButtonText,      Qt::black);
    pal.setColor(QPalette::ToolTipBase,     QColor(0xff, 0xff, 0xe1));
    pal.setColor(QPalette::ToolTipText,     Qt::black);
    pal.setColor(QPalette::Highlight,       QColor(0x2f, 0x6f, 0xd0));
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::PlaceholderText, QColor(0x80, 0x88, 0x90));
    pal.setColor(QPalette::Disabled, QPalette::Text,       QColor(0x9a, 0xa0, 0xa6));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x9a, 0xa0, 0xa6));
    pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x9a, 0xa0, 0xa6));
    app.setPalette(pal);

    FlowGuiWindow win;
    win.show();
    return app.exec();
}
